"""Synthetic ride in local coordinates, equivalent to the firmware's
simulator (src/nav_sim.cpp): an endless random route of straight stretches
and junctions with side roads, and a rider driving along it. Produces the
heading-up window directly, so it only exercises the transport, not the
lat/lon pipeline."""

from __future__ import annotations

import math
import random
from dataclasses import dataclass

from navproto import MAX_ROUTE_POINTS, MAX_SIDE_ROAD_POINTS, MAX_SIDE_ROADS, Maneuver, NavData

M_PER_UNIT = 15.0
HEADING_CHORD = 3.0  # units
VIEW_BEHIND = 12.0
VIEW_AHEAD = 80.0
GENERATE_AHEAD = 110.0
FORGET_BEHIND = 30.0
CORNER_CUT = 2.0
UTURN_RADIUS = 2.5
SPEED_TARGET_INTERVAL_S = 2.5
SPEED_SMOOTH_RATE = 1.2
MIN_SPEED_MS = 5.0


@dataclass
class _RoadPoint:
    x: float
    y: float
    s: float


@dataclass
class _SideRoad:
    points: list[tuple[float, float]]
    s: float


class SyntheticRide:
    def __init__(self, rng: random.Random | None = None):
        self.rng = rng or random.Random()
        self.road: list[_RoadPoint] = []
        self.sides: list[_SideRoad] = []
        self.junctions: list[tuple[float, Maneuver]] = []
        self._add_point(0.0, -VIEW_BEHIND - 8.0)
        self._add_point(0.0, 0.0)
        self.end_heading = math.pi / 2.0
        self.rider_s = self.road[1].s
        self.speed_kmh = 0.0
        self.speed_target = 0.0
        self.until_new_target = 0.0
        self._maintain()

    def _add_point(self, x: float, y: float) -> None:
        s = 0.0
        if self.road:
            prev = self.road[-1]
            s = prev.s + math.hypot(x - prev.x, y - prev.y)
        self.road.append(_RoadPoint(x, y, s))

    def _add_side_road(self, x: float, y: float, angle: float, length: float, s: float) -> None:
        segments = MAX_SIDE_ROAD_POINTS - 1
        bend = math.radians(self.rng.uniform(-50.0, 50.0)) if self.rng.random() < 0.5 else 0.0
        points = [(x, y)]
        for k in range(segments):
            heading = angle + bend * (k + 0.5) / segments
            px, py = points[-1]
            points.append((px + math.cos(heading) * length / segments,
                           py + math.sin(heading) * length / segments))
        self.sides.append(_SideRoad(points, s))

    def _add_branches(self, x: float, y: float, a_in: float, a_out: float, s: float, p: float) -> None:
        for c in (a_in, a_in + math.pi / 2.0, a_in - math.pi / 2.0):
            diff = abs(math.remainder(c - a_out, 2.0 * math.pi))
            if diff < math.radians(35.0) or self.rng.random() >= p:
                continue
            self._add_side_road(x, y, c + math.radians(self.rng.uniform(-12.0, 12.0)),
                                self.rng.uniform(8.0, 14.0), s)

    def _generate_leg(self) -> None:
        end = self.road[-1]
        a = self.end_heading
        straight = self.rng.uniform(12.0, 26.0)
        jx, jy = end.x + math.cos(a) * straight, end.y + math.sin(a) * straight

        if self.rng.random() < 0.45:  # a cross street along the stretch
            t = self.rng.uniform(0.35, 0.65) * straight
            mx, my = end.x + math.cos(a) * t, end.y + math.sin(a) * t
            cross = a + math.pi / 2.0 + math.radians(self.rng.uniform(-10.0, 10.0))
            for side in (cross, cross + math.pi):
                if self.rng.random() < 0.8:
                    self._add_side_road(mx, my, side, self.rng.uniform(6.0, 11.0), end.s + t)

        r = self.rng.random()
        if r < 0.38:
            maneuver, turn = Maneuver.TURN_LEFT, math.radians(self.rng.uniform(55.0, 100.0))
        elif r < 0.76:
            maneuver, turn = Maneuver.TURN_RIGHT, -math.radians(self.rng.uniform(55.0, 100.0))
        elif r < 0.92:
            maneuver, turn = Maneuver.STRAIGHT, 0.0
        else:
            maneuver, turn = Maneuver.U_TURN, -math.pi
        b = a + turn

        if maneuver == Maneuver.STRAIGHT:
            self._add_point(jx, jy)
            junction_s = self.road[-1].s
            self._add_branches(jx, jy, a, b, junction_s, 0.85)
        elif maneuver == Maneuver.U_TURN:
            rx, ry = math.sin(a), -math.cos(a)
            self._add_point(jx, jy)
            junction_s = self.road[-1].s + UTURN_RADIUS
            self._add_point(jx + (math.cos(a) + rx) * UTURN_RADIUS, jy + (math.sin(a) + ry) * UTURN_RADIUS)
            self._add_point(jx + rx * 2.0 * UTURN_RADIUS, jy + ry * 2.0 * UTURN_RADIUS)
            self._add_branches(jx, jy, a, b, junction_s, 0.5)
        else:
            self._add_point(jx - math.cos(a) * CORNER_CUT, jy - math.sin(a) * CORNER_CUT)
            junction_s = self.road[-1].s + CORNER_CUT
            self._add_point(jx + math.cos(b) * CORNER_CUT, jy + math.sin(b) * CORNER_CUT)
            self._add_branches(jx, jy, a, b, junction_s, 0.6)

        self.junctions.append((junction_s, maneuver))
        self.end_heading = b

    def _maintain(self) -> None:
        while self.road[-1].s < self.rider_s + GENERATE_AHEAD:
            self._generate_leg()
        while len(self.road) > 2 and self.road[1].s < self.rider_s - FORGET_BEHIND:
            self.road.pop(0)
        self.sides = [r for r in self.sides if r.s >= self.rider_s - FORGET_BEHIND]
        self.junctions = [j for j in self.junctions if j[0] > self.rider_s]

    def _point_at(self, s: float) -> tuple[float, float]:
        i = 0
        while i + 2 < len(self.road) and s > self.road[i + 1].s:
            i += 1
        a, b = self.road[i], self.road[i + 1]
        t = (s - a.s) / (b.s - a.s) if b.s > a.s else 0.0
        t = min(max(t, 0.0), 1.0)
        return a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t

    def step(self, dt: float) -> NavData:
        """Advances the ride by dt seconds and returns the view."""
        self.until_new_target -= dt
        if self.until_new_target <= 0.0:
            self.speed_target = self.rng.uniform(0.0, 120.0)
            self.until_new_target = SPEED_TARGET_INTERVAL_S
        self.speed_kmh += (self.speed_target - self.speed_kmh) * min(1.0, dt * SPEED_SMOOTH_RATE)
        self.rider_s += max(self.speed_kmh / 3.6, MIN_SPEED_MS) * dt / M_PER_UNIT
        self._maintain()
        return self._view()

    def _view(self) -> NavData:
        px, py = self._point_at(self.rider_s)
        bx, by = self._point_at(self.rider_s - HEADING_CHORD)
        ax, ay = self._point_at(self.rider_s + HEADING_CHORD)
        hx, hy = ax - bx, ay - by
        hl = math.hypot(hx, hy)
        hx, hy = (hx / hl, hy / hl) if hl > 0.0 else (0.0, 1.0)

        def local(x: float, y: float) -> tuple[float, float]:
            dx, dy = x - px, y - py
            return dx * hy - dy * hx, dx * hx + dy * hy

        s_from, s_to = self.rider_s - VIEW_BEHIND, self.rider_s + VIEW_AHEAD
        route = [local(*self._point_at(s_from))]
        corners = [p for p in self.road if s_from < p.s < s_to]
        route += [local(p.x, p.y) for p in corners[: MAX_ROUTE_POINTS - 2]]
        if len(corners) <= MAX_ROUTE_POINTS - 2:
            route.append(local(*self._point_at(s_to)))

        sides = [[local(x, y) for x, y in r.points] for r in self.sides if s_from <= r.s <= s_to]
        junction_s, maneuver = self.junctions[0]
        return NavData(
            maneuver=maneuver,
            distance_to_turn_m=(junction_s - self.rider_s) * M_PER_UNIT,
            total_distance_m=(self.junctions[-1][0] - self.rider_s) * M_PER_UNIT,
            speed_kmh=self.speed_kmh,
            route=route,
            side_roads=sides[:MAX_SIDE_ROADS],
        )
