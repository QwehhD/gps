import json
import math

import pytest

from maneuvers import ORS_TYPES, ROUNDABOUT, ors_steps, ors_turns, pair_up, read_ors_steps
from navgeo import Navigator, Route, Turn, from_local_m
from navproto import Maneuver

LAT0, LON0 = -7.0, 110.0


def offset(east: float, north: float) -> tuple[float, float]:
    return from_local_m(east, north, LAT0, LON0)


def walk(legs) -> list[tuple[float, float]]:
    """Points from (0, 0) along legs of (bearing in degrees, length in metres)."""
    east = north = 0.0
    points = [offset(east, north)]
    for bearing, length in legs:
        east += length * math.sin(math.radians(bearing))
        north += length * math.cos(math.radians(bearing))
        points.append(offset(east, north))
    return points


def step(kind: int, first: int, end: int, name: str = "-", **extra) -> dict:
    return {"distance": 0.0, "duration": 0.0, "type": kind, "name": name, "way_points": [first, end], **extra}


def ors_file(tmp_path, points, steps):
    """An OpenRouteService answer as fetch_route.py saves it."""
    path = tmp_path / "route.geojson"
    path.write_text(json.dumps({"type": "FeatureCollection", "features": [{
        "type": "Feature",
        "properties": {"segments": [{"distance": 0.0, "duration": 0.0, "steps": steps}]},
        "geometry": {"type": "LineString", "coordinates": [[lon, lat] for lat, lon in points]},
    }]}))
    return path


# East 400 m, left (north) 300 m, right (east) 200 m: vertices 0..3.
POINTS = walk([(90, 400), (0, 300), (90, 200)])
STEPS = [step(11, 0, 1, "Jalan Satu"), step(0, 1, 2, "Jalan Dua"), step(1, 2, 3, "Jalan Tiga"), step(10, 3, 3)]


# ---- The mapping table ------------------------------------------------------

@pytest.mark.parametrize("kind,shown", [
    (0, Maneuver.TURN_LEFT), (1, Maneuver.TURN_RIGHT), (2, Maneuver.TURN_LEFT), (3, Maneuver.TURN_RIGHT),
    (4, Maneuver.TURN_LEFT), (5, Maneuver.TURN_RIGHT), (6, None), (7, ROUNDABOUT), (8, None),
    (9, Maneuver.U_TURN), (10, None), (11, None), (12, Maneuver.TURN_LEFT), (13, Maneuver.TURN_RIGHT),
])
def test_every_ors_type_has_a_place_on_the_display(kind, shown):
    assert ORS_TYPES[kind][1] == shown


# ---- Reading steps ----------------------------------------------------------

def test_steps_of_an_ors_answer(tmp_path):
    assert read_ors_steps(ors_file(tmp_path, POINTS, STEPS)) == STEPS


def test_a_drawn_route_has_no_steps(tmp_path):
    drawn = tmp_path / "drawn.geojson"
    drawn.write_text(json.dumps({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [110.0, -7.0]}},
        {"type": "Feature", "properties": {},
         "geometry": {"type": "LineString", "coordinates": [[110.0, -7.0], [110.01, -7.0]]}}]}))
    bare = tmp_path / "bare.geojson"
    bare.write_text(json.dumps({"type": "LineString", "coordinates": [[110.0, -7.0], [110.01, -7.0]]}))
    points = tmp_path / "points.json"
    points.write_text('{"points": [[-7.0, 110.0], [-7.0, 110.01]]}')
    assert [read_ors_steps(p) for p in (drawn, bare, points)] == [None, None, None]


def test_steps_happen_at_their_first_way_point():
    route = Route(POINTS)
    steps = ors_steps(route, STEPS)
    assert [(round(s.s), s.name, s.street, s.maneuver) for s in steps] == [
        (0, "depart", "Jalan Satu", None),
        (400, "left", "Jalan Dua", Maneuver.TURN_LEFT),
        (700, "right", "Jalan Tiga", Maneuver.TURN_RIGHT),
        (900, "arrive", "", None),
    ]
    assert steps[1].angle_deg == pytest.approx(-90.0, abs=0.5)
    assert ors_turns(steps) == [Turn(steps[1].s, Maneuver.TURN_LEFT, steps[1].angle_deg),
                                Turn(steps[2].s, Maneuver.TURN_RIGHT, steps[2].angle_deg)]


def test_an_unknown_type_shows_nothing():
    steps = ors_steps(Route(POINTS), [step(99, 1, 2)])
    assert (steps[0].name, steps[0].maneuver) == ("type 99", None)


@pytest.mark.parametrize("bad", [
    {"type": 0, "way_points": [1, 9]},
    {"type": 0, "way_points": [2, 1]},
    {"type": 0},
    {"way_points": [0, 1]},
])
def test_steps_that_do_not_fit_the_route_are_refused(bad):
    with pytest.raises(ValueError, match="ORS step 0"):
        ors_steps(Route(POINTS), [bad])


# ---- Roundabouts: the way out decides ----------------------------------------

def roundabout(out_bearing: float):
    """East 300 m into a small clockwise roundabout (vertices 1..4), then out
    on out_bearing. Returns the route and its enter-roundabout step."""
    ring = [(135, 15), (225, 15), (315, 15)]  # around the island
    points = walk([(90, 300), *ring, (out_bearing, 300)])
    return Route(points), step(7, 1, 4, "Bundaran", exit_number=2)


@pytest.mark.parametrize("out_bearing,shown", [
    (90, None),  # straight on: nothing to show
    (0, Maneuver.TURN_LEFT),
    (180, Maneuver.TURN_RIGHT),
    (270, Maneuver.U_TURN),
])
def test_a_roundabout_shows_the_turn_of_its_way_out(out_bearing, shown):
    route, enter = roundabout(out_bearing)
    (got,) = ors_steps(route, [enter])
    assert (got.name, got.exit_number, got.maneuver) == ("enter roundabout", 2, shown)
    assert round(got.s) == 300


# ---- Side by side -----------------------------------------------------------

def test_pairs_and_differences():
    route = Route(POINTS)  # the line turns left at 400 m and right at 700 m
    steps = ors_steps(route, [
        step(11, 0, 1),
        step(13, 1, 2),  # keep right where the line turns left
        step(5, 2, 3),  # slight right where the line turns right
        step(4, 3, 3),  # slight left at the end, where the line does not turn
    ])
    pairs = pair_up(steps, route.turns + [Turn(850.0, Maneuver.TURN_LEFT, -40.0)])
    assert [(round(p.s), p.check) for p in pairs] == [
        (0, ""), (400, "different"), (700, "same"), (850, "geometry only"), (900, "ORS only")]


def test_a_turn_pairs_with_the_nearest_step_within_40_m():
    route = Route(POINTS)
    steps = ors_steps(route, [step(0, 1, 2)])
    near = pair_up(steps, [Turn(430.0, Maneuver.TURN_LEFT, -90.0)])
    far = pair_up(steps, [Turn(450.0, Maneuver.TURN_LEFT, -90.0)])
    assert [p.check for p in near] == ["same"]
    assert [p.check for p in far] == ["ORS only", "geometry only"]


# ---- Riding on ORS maneuvers ------------------------------------------------

def test_the_navigator_follows_the_given_turns():
    route = Route(POINTS)
    turns = ors_turns(ors_steps(route, [step(13, 1, 2)]))  # ORS: keep right at 400 m
    nav = Navigator(route, turns).update(*offset(100, 0), 30)
    assert (nav.maneuver, round(nav.distance_to_turn_m)) == (Maneuver.TURN_RIGHT, 300)
    assert Navigator(route).update(*offset(100, 0), 30).maneuver == Maneuver.TURN_LEFT  # the line
