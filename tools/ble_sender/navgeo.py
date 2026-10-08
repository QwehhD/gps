"""Turns a lat/lon route plus a GPS fix into the heading-up route window the
Butamap display draws (docs/dokumentasi-proyek.md, section 13.2).

This is the reference implementation for the phone app. Each update:

1. Map-match the fix onto the route: the closest point on the polyline,
   searched near the previous match so a road that doubles back on itself
   is not confused with its other side. The matched point P0 at route
   distance s0 is where the rider is drawn, so the rider is always on the
   line.
2. Cut the window s0 - 12 .. s0 + 80 units along the route.
3. Project to metres around P0 (equirectangular: plenty for a few km).
4. Simplify to at most 32 points (Douglas-Peucker in metres, so it runs
   after the projection; P0 is kept as a vertex).
5. Heading: the route's direction at the rider, smoothed by the chord from
   3 units behind to 3 units ahead (not the raw GPS course, which is noisy
   at low speed).
6. Rotate heading-up: x = e cos h - n sin h (right), y = e sin h + n cos h
   (ahead), h clockwise from north.
7. Divide by 15 m per unit.

The next maneuver comes from the route's own geometry (turns sharper than
35 degrees). A real app should take maneuvers from its routing API instead.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from navproto import MAX_ROUTE_POINTS, Maneuver, NavData

M_PER_UNIT = 15.0
WINDOW_BEHIND_UNITS = 12.0
WINDOW_AHEAD_UNITS = 80.0
HEADING_CHORD_UNITS = 3.0

M_PER_DEG_LAT = 110540.0
M_PER_DEG_LON_AT_EQUATOR = 111320.0

# Map matching: search this far back/ahead of the previous match first, and
# give up on continuity if the best point there is further than this.
MATCH_BACK_M = 50.0
MATCH_AHEAD_M = 500.0
MATCH_MAX_OFFSET_M = 60.0

# Turn detection on the route geometry.
TURN_MIN_DEG = 35.0
TURN_CHORD_M = 20.0
TURN_MERGE_M = 30.0
UTURN_MIN_DEG = 150.0

LatLon = tuple[float, float]
Point = tuple[float, float]


def to_local_m(lat: float, lon: float, lat0: float, lon0: float) -> Point:
    """(east, north) in metres of (lat, lon) relative to (lat0, lon0)."""
    east = (lon - lon0) * math.cos(math.radians(lat0)) * M_PER_DEG_LON_AT_EQUATOR
    north = (lat - lat0) * M_PER_DEG_LAT
    return east, north


def from_local_m(east: float, north: float, lat0: float, lon0: float) -> LatLon:
    """Inverse of to_local_m()."""
    lat = lat0 + north / M_PER_DEG_LAT
    lon = lon0 + east / (math.cos(math.radians(lat0)) * M_PER_DEG_LON_AT_EQUATOR)
    return lat, lon


def bearing_deg(east: float, north: float) -> float:
    """Direction of the vector (east, north), degrees clockwise from north."""
    return math.degrees(math.atan2(east, north)) % 360.0


def rotate_heading_up(east: float, north: float, heading_deg: float) -> Point:
    """(x, y) as seen by a rider facing heading_deg: +x right, +y ahead.

    Facing east (90), a point to the north comes out at x < 0 (left)."""
    h = math.radians(heading_deg)
    return (
        east * math.cos(h) - north * math.sin(h),
        east * math.sin(h) + north * math.cos(h),
    )


def _point_line_distance(p: Point, a: Point, b: Point) -> float:
    dx, dy = b[0] - a[0], b[1] - a[1]
    length = math.hypot(dx, dy)
    if length == 0.0:
        return math.hypot(p[0] - a[0], p[1] - a[1])
    return abs(dx * (a[1] - p[1]) - dy * (a[0] - p[0])) / length


def douglas_peucker(points: list[Point], tolerance: float) -> list[Point]:
    """Keeps the endpoints and every point further than tolerance from the
    simplified line."""
    if len(points) < 3:
        return list(points)
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        first, last = stack.pop()
        worst, worst_i = 0.0, 0
        for i in range(first + 1, last):
            d = _point_line_distance(points[i], points[first], points[last])
            if d > worst:
                worst, worst_i = d, i
        if worst > tolerance:
            keep[worst_i] = True
            stack.append((first, worst_i))
            stack.append((worst_i, last))
    return [p for p, k in zip(points, keep) if k]


def simplify(points: list[Point], max_points: int, keep_index: int | None = None,
             tolerance: float = 0.5) -> list[Point]:
    """Douglas-Peucker with a tolerance (metres) raised until at most
    max_points remain. points[keep_index], if given, is always kept."""
    while True:
        if keep_index is None:
            out = douglas_peucker(points, tolerance)
        else:
            out = douglas_peucker(points[: keep_index + 1], tolerance)[:-1]
            out += douglas_peucker(points[keep_index:], tolerance)
        if len(out) <= max_points:
            return out
        tolerance *= 1.5


@dataclass
class Turn:
    s: float  # route distance of the turn, metres
    maneuver: Maneuver


class Route:
    """A lat/lon polyline with arc lengths, map matching and turn detection.

    Arc lengths and matching use one planar frame around the first point;
    the window for the display is projected around the rider instead."""

    def __init__(self, points: list[LatLon]):
        if len(points) < 2:
            raise ValueError("a route needs at least 2 points")
        self.latlon = list(points)
        self.lat0, self.lon0 = points[0]
        self.xy = [to_local_m(lat, lon, self.lat0, self.lon0) for lat, lon in points]
        self.cum = [0.0]
        for a, b in zip(self.xy, self.xy[1:]):
            self.cum.append(self.cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
        self.length = self.cum[-1]
        self.turns = self._detect_turns()

    def _segment_at(self, s: float) -> int:
        i = 0
        while i + 2 < len(self.cum) and s > self.cum[i + 1]:
            i += 1
        return i

    def xy_at(self, s: float) -> Point:
        """Planar point at route distance s (clamped to the route)."""
        s = min(max(s, 0.0), self.length)
        i = self._segment_at(s)
        seg = self.cum[i + 1] - self.cum[i]
        t = (s - self.cum[i]) / seg if seg > 0.0 else 0.0
        a, b = self.xy[i], self.xy[i + 1]
        return a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t

    def latlon_at(self, s: float) -> LatLon:
        x, y = self.xy_at(s)
        return from_local_m(x, y, self.lat0, self.lon0)

    def heading_at(self, s: float, chord_m: float) -> float:
        """Route direction at s, from the chord s - chord_m .. s + chord_m."""
        back = self.xy_at(s - chord_m)
        ahead = self.xy_at(s + chord_m)
        if back == ahead:
            back, ahead = self.xy[0], self.xy[1]
        return bearing_deg(ahead[0] - back[0], ahead[1] - back[1])

    def match(self, lat: float, lon: float, prev_s: float | None = None) -> tuple[float, float]:
        """(s, offset): route distance of the point closest to the fix, and
        how far the fix is from it, in metres."""
        p = to_local_m(lat, lon, self.lat0, self.lon0)
        best_all = best_near = (math.inf, 0.0)
        for i, (a, b) in enumerate(zip(self.xy, self.xy[1:])):
            dx, dy = b[0] - a[0], b[1] - a[1]
            len2 = dx * dx + dy * dy
            t = 0.0 if len2 == 0.0 else ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / len2
            t = min(max(t, 0.0), 1.0)
            d = math.hypot(a[0] + dx * t - p[0], a[1] + dy * t - p[1])
            s = self.cum[i] + (self.cum[i + 1] - self.cum[i]) * t
            if d < best_all[0]:
                best_all = (d, s)
            near = prev_s is not None and prev_s - MATCH_BACK_M <= s <= prev_s + MATCH_AHEAD_M
            if near and d < best_near[0]:
                best_near = (d, s)
        best = best_near if best_near[0] <= MATCH_MAX_OFFSET_M else best_all
        return best[1], best[0]

    def _detect_turns(self) -> list[Turn]:
        candidates: list[tuple[float, float]] = []
        for i in range(1, len(self.xy) - 1):
            s = self.cum[i]
            before = bearing_deg(*_sub(self.xy[i], self.xy_at(s - TURN_CHORD_M)))
            after = bearing_deg(*_sub(self.xy_at(s + TURN_CHORD_M), self.xy[i]))
            delta = (after - before + 180.0) % 360.0 - 180.0
            if abs(delta) >= TURN_MIN_DEG:
                candidates.append((s, delta))
        # Several vertices of one curve: keep the sharpest.
        turns: list[tuple[float, float]] = []
        for s, delta in candidates:
            if turns and s - turns[-1][0] < TURN_MERGE_M:
                if abs(delta) > abs(turns[-1][1]):
                    turns[-1] = (s, delta)
            else:
                turns.append((s, delta))
        out = []
        for s, delta in turns:
            if abs(delta) >= UTURN_MIN_DEG:
                maneuver = Maneuver.U_TURN
            elif delta < 0.0:
                maneuver = Maneuver.TURN_LEFT
            else:
                maneuver = Maneuver.TURN_RIGHT
            out.append(Turn(s, maneuver))
        return out

    def window(self, s0: float) -> tuple[list[LatLon], int]:
        """Route points from s0 - 12 to s0 + 80 units (clamped to the route):
        exact cuts at both ends, the route's own vertices in between, and the
        rider's point. Returns (points, index of the rider's point)."""
        s_from = max(0.0, s0 - WINDOW_BEHIND_UNITS * M_PER_UNIT)
        s_to = min(self.length, s0 + WINDOW_AHEAD_UNITS * M_PER_UNIT)
        stations = [s_from, s0, s_to] + [s for s in self.cum if s_from < s < s_to]
        stations = sorted(set(stations))
        return [self.latlon_at(s) for s in stations], stations.index(s0)


def _sub(a: Point, b: Point) -> Point:
    return a[0] - b[0], a[1] - b[1]


class Navigator:
    """Keeps the map-matching state for one ride along a route."""

    def __init__(self, route: Route):
        self.route = route
        self.s: float | None = None

    def update(self, lat: float, lon: float, speed_kmh: float) -> NavData:
        route = self.route
        s0, _ = route.match(lat, lon, self.s)
        self.s = s0
        lat0, lon0 = route.latlon_at(s0)

        points, rider = route.window(s0)
        local = [to_local_m(lat, lon, lat0, lon0) for lat, lon in points]
        local = simplify(local, MAX_ROUTE_POINTS, keep_index=rider)

        heading = route.heading_at(s0, HEADING_CHORD_UNITS * M_PER_UNIT)
        window = []
        for east, north in local:
            x, y = rotate_heading_up(east, north, heading)
            window.append((x / M_PER_UNIT, y / M_PER_UNIT))

        ahead = [t for t in route.turns if t.s > s0]
        if ahead:
            maneuver, distance = ahead[0].maneuver, ahead[0].s - s0
        else:
            maneuver, distance = Maneuver.STRAIGHT, route.length - s0  # to the destination
        return NavData(
            maneuver=maneuver,
            distance_to_turn_m=distance,
            total_distance_m=route.length - s0,
            speed_kmh=speed_kmh,
            route=window,
        )
