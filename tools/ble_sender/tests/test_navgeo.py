import math
import random

import pytest

from gps_sim import SAMPLE_ROUTE, FakeGps, load_route
from navgeo import (
    M_PER_UNIT,
    Navigator,
    Route,
    from_local_m,
    rotate_heading_up,
    simplify,
    to_local_m,
)
from navproto import MSG_MAX_SIZE, Maneuver, encode

LAT0, LON0 = -6.2, 106.8


def offset(east: float, north: float) -> tuple[float, float]:
    """A lat/lon east/north metres away from (LAT0, LON0)."""
    return from_local_m(east, north, LAT0, LON0)


def polyline_distance(p, line):
    """Distance from p to the polyline."""
    best = math.inf
    for a, b in zip(line, line[1:]):
        dx, dy = b[0] - a[0], b[1] - a[1]
        len2 = dx * dx + dy * dy
        t = 0.0 if len2 == 0 else max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / len2))
        best = min(best, math.hypot(a[0] + dx * t - p[0], a[1] + dy * t - p[1]))
    return best


def polyline_length(line):
    return sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(line, line[1:]))


# ---- Building blocks --------------------------------------------------------

def test_facing_east_a_point_to_the_north_is_on_the_left():
    x, y = rotate_heading_up(0.0, 100.0, 90.0)
    assert x < 0
    assert x == pytest.approx(-100.0)
    assert y == pytest.approx(0.0, abs=1e-9)


@pytest.mark.parametrize("heading,east,north,expected", [
    (0.0, 0.0, 10.0, (0.0, 10.0)),     # facing north, north is ahead
    (0.0, 10.0, 0.0, (10.0, 0.0)),     # facing north, east is right
    (90.0, 10.0, 0.0, (0.0, 10.0)),    # facing east, east is ahead
    (180.0, 0.0, 10.0, (0.0, -10.0)),  # facing south, north is behind
    (270.0, 0.0, 10.0, (10.0, 0.0)),   # facing west, north is right
])
def test_heading_up_rotation(heading, east, north, expected):
    x, y = rotate_heading_up(east, north, heading)
    assert (x, y) == pytest.approx(expected, abs=1e-9)


def test_equirectangular_scale():
    east, north = to_local_m(LAT0 + 0.001, LON0 + 0.001, LAT0, LON0)
    assert north == pytest.approx(110.54)
    assert east == pytest.approx(111.32 * math.cos(math.radians(LAT0)))
    assert from_local_m(east, north, LAT0, LON0) == pytest.approx((LAT0 + 0.001, LON0 + 0.001))


def test_simplify_respects_the_limit_and_keeps_the_rider():
    wiggle = [(i * 3.0, 20.0 * math.sin(i / 7.0)) for i in range(400)]
    out = simplify(wiggle, 32, keep_index=123)
    assert len(out) <= 32
    assert wiggle[123] in out
    assert out[0] == wiggle[0] and out[-1] == wiggle[-1]
    assert max(polyline_distance(p, out) for p in wiggle) < 6.0


# ---- Full pipeline ----------------------------------------------------------

def east_then_north_route():
    """1 km east, then 1 km north: a left turn at 1000 m."""
    return Route([offset(0, 0), offset(500, 0), offset(1000, 0), offset(1000, 500), offset(1000, 1000)])


def test_facing_east_the_road_turning_north_goes_left():
    nav = Navigator(east_then_north_route()).update(*offset(700, 0), 30)
    # 300 m east to the corner, then 900 m north: drawn straight ahead, then left.
    assert nav.route[-1] == pytest.approx((-900 / M_PER_UNIT, 300 / M_PER_UNIT))
    assert all(x <= 1e-6 for x, _ in nav.route)
    assert nav.maneuver == Maneuver.TURN_LEFT
    assert nav.distance_to_turn_m == pytest.approx(300.0, abs=0.5)


def test_rider_is_on_the_line_and_window_has_the_right_length():
    route = east_then_north_route()
    fix = offset(400, 8)  # 8 m beside the road
    nav = Navigator(route).update(*fix, 30)
    assert polyline_distance((0.0, 0.0), nav.route) < 1e-6
    # 12 units behind + 80 ahead, in units.
    assert polyline_length(nav.route) == pytest.approx(92.0, abs=0.1)
    # Heading-up on the straight: the line runs along y.
    assert all(abs(x) < 0.01 for x, y in nav.route if y < 20)
    assert nav.route[0][1] == pytest.approx(-12.0, abs=0.01)


def test_window_is_clamped_at_the_route_ends():
    route = east_then_north_route()
    start = Navigator(route).update(*offset(0, 0), 30)
    assert start.route[0] == pytest.approx((0.0, 0.0), abs=1e-6)
    end = Navigator(route).update(*offset(1000, 990), 30)
    assert end.maneuver == Maneuver.STRAIGHT
    assert end.distance_to_turn_m == pytest.approx(10.0, abs=0.5)
    assert end.total_distance_m == pytest.approx(10.0, abs=0.5)


def test_heading_comes_from_the_route_not_the_noisy_fix():
    route = east_then_north_route()
    navigator = Navigator(route)
    rng = random.Random(1)
    for east in range(100, 600, 10):
        nav = navigator.update(*offset(east + rng.gauss(0, 5), rng.gauss(0, 5)), 30)
        ahead = [p for p in nav.route if 0 < p[1] < 20]
        assert all(abs(x) < 0.01 for x, _ in ahead)


def test_matching_follows_a_road_that_doubles_back():
    # North 500 m, U-turn, back south 12 m to the east.
    route = Route([offset(0, 0), offset(0, 500), offset(12, 500), offset(12, 0)])
    navigator = Navigator(route)
    for north in range(0, 500, 20):
        navigator.update(*offset(0, north), 30)
    for north in range(490, 100, -20):
        nav = navigator.update(*offset(12, north), 30)
        assert navigator.s > 500  # on the way back, not the parallel outbound leg
    assert nav.maneuver == Maneuver.STRAIGHT


def test_sample_route_turns():
    route = Route(SAMPLE_ROUTE)
    assert [t.maneuver for t in route.turns] == [
        Maneuver.TURN_RIGHT, Maneuver.TURN_RIGHT, Maneuver.TURN_RIGHT, Maneuver.TURN_LEFT]
    assert 4000 < route.length < 6000


def test_riding_the_sample_route_with_noise():
    route = Route(SAMPLE_ROUTE)
    gps = FakeGps(route, speed_kmh=40, noise_m=5, rng=random.Random(2))
    navigator = Navigator(route)
    last_distance = None
    for _ in range(int(route.length / (40 / 3.6) * 5) - 10):  # one lap at 5 Hz
        nav = navigator.update(*gps.step(0.2), 40)
        msg = encode(nav, 0)
        assert len(msg) <= MSG_MAX_SIZE
        assert 2 <= len(nav.route) <= 32
        assert polyline_distance((0.0, 0.0), nav.route) < 1e-6
        assert abs(navigator.s - gps.s) < 20  # matched within the noise
        if last_distance is not None and nav.maneuver != Maneuver.STRAIGHT:
            assert nav.distance_to_turn_m <= last_distance + 20 or nav.distance_to_turn_m > 200
        last_distance = nav.distance_to_turn_m


def test_load_route_formats(tmp_path):
    points = tmp_path / "points.json"
    points.write_text('{"points": [[-6.1, 106.8], [-6.2, 106.9]]}')
    assert load_route(points) == [(-6.1, 106.8), (-6.2, 106.9)]
    geojson = tmp_path / "route.geojson"
    geojson.write_text('{"type": "FeatureCollection", "features": [{"type": "Feature", '
                       '"geometry": {"type": "LineString", "coordinates": [[106.8, -6.1], [106.9, -6.2]]}}]}')
    assert load_route(geojson) == [(-6.1, 106.8), (-6.2, 106.9)]
    bad = tmp_path / "bad.json"
    bad.write_text("[1, 2]")
    with pytest.raises(ValueError):
        load_route(bad)
