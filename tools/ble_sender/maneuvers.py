"""Where the display's next maneuver comes from.

- ors: the instructions in an OpenRouteService answer
  (properties.segments[].steps). This is what the phone app will use. A
  step happens at its first way point, an index into the route's
  coordinates.
- geometry: the turns in the shape of the line (navgeo.Route.turns), for
  hand-drawn routes that have no steps.

The firmware knows four maneuvers (straight, left, right, U-turn), so the
ORS instruction types are folded onto those (ORS_TYPES). A roundabout has
no maneuver of its own yet: it is shown as the turn its way out makes.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from navgeo import TURN_CHORD_M, TURN_MIN_DEG, UTURN_MIN_DEG, Route, Turn, bearing_deg
from navproto import Maneuver

# Stands for "take the maneuver from the way out of the roundabout".
ROUNDABOUT = "roundabout"

# ORS instruction type -> (name, what the display shows). None: no maneuver
# of its own, the display keeps showing the next one.
ORS_TYPES: dict[int, tuple[str, Maneuver | str | None]] = {
    0: ("left", Maneuver.TURN_LEFT),
    1: ("right", Maneuver.TURN_RIGHT),
    2: ("sharp left", Maneuver.TURN_LEFT),
    3: ("sharp right", Maneuver.TURN_RIGHT),
    4: ("slight left", Maneuver.TURN_LEFT),
    5: ("slight right", Maneuver.TURN_RIGHT),
    6: ("straight", None),
    7: ("enter roundabout", ROUNDABOUT),
    8: ("exit roundabout", None),
    9: ("u-turn", Maneuver.U_TURN),
    10: ("arrive", None),
    11: ("depart", None),
    12: ("keep left", Maneuver.TURN_LEFT),
    13: ("keep right", Maneuver.TURN_RIGHT),
}

# An ORS step and a geometry turn this close together are the same place.
PAIR_M = 40.0


@dataclass
class OrsStep:
    s: float  # route distance where the step happens, metres
    ors_type: int
    name: str  # ORS_TYPES name, or "type N" for a type not in the table
    street: str  # "" when ORS gives none
    exit_number: int | None  # roundabouts only
    maneuver: Maneuver | None  # what the display gets; None = no maneuver
    angle_deg: float  # how much the line turns there, + = right


@dataclass
class Pair:
    """One line of the side-by-side list: an ORS step, a geometry turn, or
    both when they are the same place."""

    s: float
    step: OrsStep | None
    turn: Turn | None
    check: str  # "same", "different", "ORS only", "geometry only", or ""


def read_ors_steps(path: str | Path) -> list[dict] | None:
    """The steps of an OpenRouteService answer saved by fetch_route.py, from
    the same feature read_route_file() takes the line from; None if the
    file has none (a hand-drawn route)."""
    data = json.loads(Path(path).read_bytes())
    if not isinstance(data, dict):
        return None
    features = data.get("features") if data.get("type") == "FeatureCollection" else [data]
    for feature in features if isinstance(features, list) else []:
        if not isinstance(feature, dict) or feature.get("type") != "Feature":
            continue
        geometry = feature.get("geometry")
        if isinstance(geometry, dict) and geometry.get("type") == "LineString":
            segments = (feature.get("properties") or {}).get("segments")
            if not isinstance(segments, list):
                return None
            steps = [step for segment in segments if isinstance(segment, dict)
                     for step in segment.get("steps") or []]
            return steps or None
    return None


def ors_steps(route: Route, raw_steps: list[dict]) -> list[OrsStep]:
    """Maps raw ORS steps onto the route. Raises ValueError for a step
    without a type or with way points outside the route."""
    last = len(route.latlon) - 1
    out = []
    for n, step in enumerate(raw_steps):
        try:
            kind = int(step["type"])
            first, end = (int(i) for i in step["way_points"])
        except (KeyError, TypeError, ValueError):
            raise ValueError(f"ORS step {n} has no usable type and way_points") from None
        if not 0 <= first <= end <= last:
            raise ValueError(f"ORS step {n}: way_points [{first}, {end}] are outside the route's "
                             f"{last + 1} points; is this the file the steps came with?")
        name, shown = ORS_TYPES.get(kind, (f"type {kind}", None))
        s = route.cum[first]
        if shown == ROUNDABOUT:
            angle = _through(route, s, route.cum[end])
            maneuver = _classify(angle)
        else:
            angle = route.direction_change(s)
            maneuver = shown
        street = str(step.get("name") or "").strip()
        exit_number = step.get("exit_number")
        out.append(OrsStep(s, kind, name, "" if street == "-" else street,
                           exit_number if isinstance(exit_number, int) else None, maneuver, angle))
    return out


def ors_turns(steps: list[OrsStep]) -> list[Turn]:
    """The steps that show a maneuver, as turns for navgeo.Navigator."""
    return [Turn(step.s, step.maneuver, step.angle_deg) for step in steps if step.maneuver is not None]


def _through(route: Route, s_in: float, s_out: float) -> float:
    """Direction change from the way into a roundabout to the way out of it,
    degrees, + = right: from the TURN_CHORD_M before the entry to the
    TURN_CHORD_M after the exit."""
    a, b = route.xy_at(s_in - TURN_CHORD_M), route.xy_at(s_in)
    c, d = route.xy_at(s_out), route.xy_at(s_out + TURN_CHORD_M)
    before = bearing_deg(b[0] - a[0], b[1] - a[1])
    after = bearing_deg(d[0] - c[0], d[1] - c[1])
    return (after - before + 180.0) % 360.0 - 180.0


def _classify(angle: float) -> Maneuver | None:
    """The maneuver for a roundabout whose way out turns by angle."""
    if abs(angle) < TURN_MIN_DEG:
        return None  # straight on: nothing to show
    if abs(angle) >= UTURN_MIN_DEG:
        return Maneuver.U_TURN
    return Maneuver.TURN_LEFT if angle < 0.0 else Maneuver.TURN_RIGHT


def pair_up(steps: list[OrsStep], turns: list[Turn]) -> list[Pair]:
    """ORS steps and geometry turns side by side, in route order. A geometry
    turn pairs with the nearest ORS step within PAIR_M that shows a
    maneuver; whatever stays alone is a difference, except ORS steps that
    show nothing (depart, straight, arrive)."""
    paired: dict[int, int] = {}  # step index -> turn index
    for t, turn in enumerate(turns):
        near = [(abs(step.s - turn.s), n) for n, step in enumerate(steps)
                if step.maneuver is not None and n not in paired and abs(step.s - turn.s) <= PAIR_M]
        if near:
            paired[min(near)[1]] = t
    out = []
    for n, step in enumerate(steps):
        if n in paired:
            turn = turns[paired[n]]
            out.append(Pair(step.s, step, turn, "same" if turn.maneuver == step.maneuver else "different"))
        else:
            out.append(Pair(step.s, step, None, "ORS only" if step.maneuver is not None else ""))
    used = set(paired.values())
    out += [Pair(turn.s, None, turn, "geometry only") for t, turn in enumerate(turns) if t not in used]
    out.sort(key=lambda p: p.s)  # stable: a step stays before a lone turn at the same place
    return out
