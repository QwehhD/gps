#!/usr/bin/env python3
"""Writes the test vectors for the phone app's port of the pipeline to
test_vectors/: per case, the input (a lat/lon route, its maneuvers, a GPS
fix) and what navgeo + navproto make of it, down to the 'N' message and its
BLE frames in hex. The port must produce the same bytes.

    python make_test_vectors.py          # (re)write test_vectors/*.json
    python make_test_vectors.py --check  # exit 1 if a file is out of date

Only synthetic coordinates (metres around ORIGIN) go in: these files are
committed, so no personal route may end up in them. test_vectors/README.md
describes the fields.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

from navgeo import (
    HEADING_CHORD_UNITS,
    M_PER_DEG_LAT,
    M_PER_DEG_LON_AT_EQUATOR,
    M_PER_UNIT,
    WINDOW_AHEAD_UNITS,
    WINDOW_BEHIND_UNITS,
    Navigator,
    Route,
    Turn,
    from_local_m,
)
from navproto import COORD_SCALE, MAX_ROUTE_POINTS, Maneuver, NavData, encode, split_frames

OUT_DIR = Path(__file__).resolve().parent / "test_vectors"
ORIGIN = (-7.0, 110.0)  # synthetic, not anybody's place
FRAME_SIZES = (20, 182, 514)  # BLE minimum, iOS, BlueZ
TIE_MARGIN = 1e-6  # quantized values keep this far from a rounding tie


def legs_to_points(legs: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """(east, north) metres from (0, 0) along legs of (bearing deg, length m)."""
    east = north = 0.0
    points = [(east, north)]
    for bearing, length in legs:
        east += length * math.sin(math.radians(bearing))
        north += length * math.cos(math.radians(bearing))
        points.append((east, north))
    return points


def fix_at(route: Route, s: float, right_m: float) -> tuple[float, float]:
    """A GPS fix right_m to the right of the route at distance s (negative:
    to the left)."""
    x, y = route.xy_at(s)
    h = math.radians(route.heading_at(s, 1.0))
    x += right_m * math.cos(h)
    y -= right_m * math.sin(h)
    return from_local_m(x, y, route.lat0, route.lon0)


def arc_legs() -> list[tuple[float, float]]:
    """A long gentle curve, 80 legs of 9 m bending 1.25 degrees each: far more
    vertices in the window than the 32 the message can carry."""
    return [(10.0 + 1.25 * i, 9.0) for i in range(80)]


# name: (description, legs, turns as (vertex index, maneuver), fix distance,
#        fix offset to the right (m), previous match (m) or None, speed, seq)
CASES = {
    "right_turn": ("Heading 30 degrees, a right turn onto 120 degrees 180 m ahead; the fix is 3 m left of "
                   "the road.", [(30, 600), (120, 500)], [(1, Maneuver.TURN_RIGHT)], 420.0, -3.0, None, 32, 1),
    "left_turn": ("Heading 200 degrees, a left turn onto 110 degrees 120 m ahead; the fix is on the road.",
                  [(200, 500), (110, 600)], [(1, Maneuver.TURN_LEFT)], 380.0, 0.0, None, 45, 2),
    "u_turn": ("Heading 350 degrees, a U-turn across a 12 m median 100 m ahead, then back on 170 degrees.",
               [(350, 400), (80, 12), (170, 400)], [(1, Maneuver.U_TURN)], 300.0, 1.5, None, 20, 3),
    "straight": ("One straight road on 45 degrees, no maneuver: the distance is to the end of the route; "
                 "the fix is 5 m right of the road.", [(45, 1200)], [], 200.0, 5.0, None, 60, 4),
    "near_end": ("30 m before the end of the route, past its last turn, with a previous match: the window "
                 "stops at the end and the maneuver is straight to the end. Sequence number 255.",
                 [(300, 500), (30, 400)], [(1, Maneuver.TURN_RIGHT)], 870.0, 0.0, 850.0, 15, 255),
    "curve_simplified": ("Rider in a long gentle curve with more route vertices in the window than the "
                         "message carries: the window is simplified (Douglas-Peucker in metres, tolerance "
                         "raised from 0.5 m by x1.5 until at most 32 points remain).",
                         arc_legs(), [], 300.0, 0.0, None, 50, 77),
}


def build(name: str) -> dict:
    description, legs, turn_spec, fix_s, fix_right, previous_s, speed, seq = CASES[name]
    points = [from_local_m(e, n, *ORIGIN) for e, n in legs_to_points(legs)]
    route = Route(points)
    turns = [Turn(route.cum[i], maneuver, 0.0) for i, maneuver in turn_spec]
    lat, lon = fix_at(route, fix_s, fix_right)

    s0, offset = route.match(lat, lon, previous_s)
    heading = route.heading_at(s0, HEADING_CHORD_UNITS * M_PER_UNIT)
    navigator = Navigator(route, turns)
    navigator.s = previous_s
    nav = navigator.update(lat, lon, speed)
    msg = encode(nav, seq)
    _check_no_ties(name, nav)

    return {
        "name": name,
        "description": description,
        "config": {
            "m_per_unit": M_PER_UNIT,
            "window_behind_units": WINDOW_BEHIND_UNITS,
            "window_ahead_units": WINDOW_AHEAD_UNITS,
            "heading_chord_units": HEADING_CHORD_UNITS,
            "max_route_points": MAX_ROUTE_POINTS,
            "coord_scale": COORD_SCALE,
            "m_per_deg_lat": M_PER_DEG_LAT,
            "m_per_deg_lon_at_equator": M_PER_DEG_LON_AT_EQUATOR,
        },
        "input": {
            "route_latlon": [[lat_, lon_] for lat_, lon_ in points],
            "turns": [{"index": i, "s_m": route.cum[i], "maneuver": m.name} for i, m in turn_spec],
            "fix_latlon": [lat, lon],
            "previous_s_m": previous_s,
            "speed_kmh": speed,
            "seq": seq,
        },
        "expected": {
            "route_length_m": route.length,
            "matched_s_m": s0,
            "offset_m": offset,
            "heading_deg": heading,
            "maneuver": nav.maneuver.name,
            "distance_to_turn_m": nav.distance_to_turn_m,
            "total_distance_m": nav.total_distance_m,
            "window_units": [[x, y] for x, y in nav.route],
            "message_hex": msg.hex(),
            "frames_hex": {str(size): [f.hex() for f in split_frames(msg, seq, size)] for size in FRAME_SIZES},
        },
    }


def _check_no_ties(name: str, nav: NavData) -> None:
    """Refuses a case whose quantized values sit on a rounding tie, where a
    last-bit difference in another language's sin/cos could flip a byte."""
    values = [(v, COORD_SCALE) for p in nav.route for v in p]
    values += [(nav.distance_to_turn_m, 1.0), (nav.total_distance_m, 0.1), (nav.speed_kmh, 1.0)]
    for value, scale in values:
        frac = abs(value * scale) % 1.0
        if abs(frac - 0.5) < TIE_MARGIN:
            raise ValueError(f"{name}: {value!r} x {scale:g} is a rounding tie; move the fix a little")


_PAIR = re.compile(r"\[\s+(-?[\d.e+-]+),\s+(-?[\d.e+-]+)\s+\]")


def render(vector: dict) -> str:
    """Indented JSON, each [a, b] number pair on one line. Floats are written
    in full (shortest round-trip form), so JSON.parse gets the same doubles."""
    return _PAIR.sub(r"[\1, \2]", json.dumps(vector, indent=2)) + "\n"


def main(argv: list[str] | None = None) -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--check", action="store_true", help="only check that the files are up to date")
    args = p.parse_args(argv)

    stale = []
    for name in CASES:
        path = OUT_DIR / f"{name}.json"
        text = render(build(name))
        if args.check:
            if not path.exists() or path.read_text() != text:
                stale.append(path.name)
        else:
            OUT_DIR.mkdir(exist_ok=True)
            path.write_text(text)
            print(f"wrote test_vectors/{path.name}")
    if stale:
        sys.exit(f"out of date: {', '.join(stale)}; run python make_test_vectors.py")


if __name__ == "__main__":
    main()
