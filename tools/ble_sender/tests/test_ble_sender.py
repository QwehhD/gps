import io
import json
import random

import pytest

import ble_sender
from ble_sender import Controls, make_source, parse_args, route_summary
from navgeo import Route, from_local_m
from navproto import Maneuver

LAT0, LON0 = -7.97, 112.63


def offset(east: float, north: float) -> tuple[float, float]:
    return from_local_m(east, north, LAT0, LON0)


def write_route(tmp_path, points, steps=None, name="route.geojson"):
    """A Feature with the line, plus ORS steps in its properties if given."""
    path = tmp_path / name
    properties = {"segments": [{"steps": steps}]} if steps is not None else {}
    path.write_text(json.dumps({"type": "Feature", "properties": properties, "geometry": {
        "type": "LineString", "coordinates": [[lon, lat] for lat, lon in points]}}))
    return path


# East 1000 m, left (north) 500 m.
L_ROUTE = [offset(0, 0), offset(1000, 0), offset(1000, 500)]
L_STEPS = [{"type": 11, "name": "Jalan A", "way_points": [0, 1]},
           {"type": 13, "name": "-", "way_points": [1, 2]},  # ORS: keep right, the line: left
           {"type": 10, "name": "-", "way_points": [2, 2]}]


# ---- Route summary ----------------------------------------------------------

def test_summary_lists_each_turn_with_direction_angle_and_position():
    # East 1000 m, left (north) for 500 m, right (east) again.
    route = Route([offset(0, 0), offset(1000, 0), offset(1000, 500), offset(1600, 500)])
    lines = route_summary(route, "test route").splitlines()
    assert lines[0] == "route: test route"
    assert lines[1] == "  4 points, 2100 m"
    assert lines[2].startswith("  2 turns in the line")
    left_lat, left_lon = offset(1000, 0)
    right_lat, right_lon = offset(1000, 500)
    assert lines[4].split() == ["1", "left", "90°", "1000", "m", f"{left_lat:.6f},{left_lon:.6f}"]
    assert lines[5].split() == ["2", "right", "90°", "1500", "m", f"{right_lat:.6f},{right_lon:.6f}"]


def test_summary_of_a_straight_route():
    summary = route_summary(Route([offset(0, 0), offset(0, 800)]), "straight")
    assert summary.splitlines()[1:] == ["  2 points, 800 m", "  no turns sharper than 35° in the line"]


# ---- Maneuver source --------------------------------------------------------

def test_a_drawn_route_uses_the_turns_in_the_line(tmp_path, capsys):
    path = write_route(tmp_path, L_ROUTE)
    source = make_source(parse_args(["--latlon", str(path), "--dry-run"]), random.Random(1))
    out = capsys.readouterr().out
    assert f"route: {path} (GeoJSON Feature)" in out
    assert "  1 turn in the line (" in out
    assert "ORS steps" not in out
    assert "the display gets the turns in the line\n" in out
    assert source.step(0.2).maneuver == Maneuver.TURN_LEFT


def test_an_ors_route_uses_its_steps_and_lists_the_differences(tmp_path, capsys):
    path = write_route(tmp_path, L_ROUTE, L_STEPS)
    source = make_source(parse_args(["--latlon", str(path), "--dry-run"]), random.Random(1))
    out = capsys.readouterr().out
    assert "3 ORS steps next to the turns in the line" in out
    assert ("  1 difference:\n"
            "    - at 1000 m ORS says keep right (shown as right), the line makes a left 90°") in out
    assert "the display gets the ORS maneuvers" in out
    assert source.step(0.2).maneuver == Maneuver.TURN_RIGHT


def test_geometry_can_be_chosen_for_an_ors_route(tmp_path, capsys):
    path = write_route(tmp_path, L_ROUTE, L_STEPS)
    source = make_source(parse_args(["--latlon", str(path), "--dry-run", "--maneuvers", "geometry"]),
                         random.Random(1))
    assert "the display gets the turns in the line (--maneuvers ors" in capsys.readouterr().out
    assert source.step(0.2).maneuver == Maneuver.TURN_LEFT


def test_ors_maneuvers_need_steps(tmp_path):
    path = write_route(tmp_path, L_ROUTE)
    with pytest.raises(SystemExit, match=r"no OpenRouteService steps.*--maneuvers geometry"):
        make_source(parse_args(["--latlon", str(path), "--maneuvers", "ors"]), random.Random(1))


def test_steps_that_do_not_fit_the_line_are_refused(tmp_path):
    path = write_route(tmp_path, L_ROUTE, [{"type": 0, "way_points": [1, 7]}])
    with pytest.raises(SystemExit, match=r"^error: .*ORS step 0: way_points \[1, 7\]"):
        make_source(parse_args(["--latlon", str(path)]), random.Random(1))


def test_unusable_latlon_file_exits_with_the_reason(tmp_path):
    path = tmp_path / "swapped.geojson"
    path.write_text(json.dumps({"type": "LineString", "coordinates": [[-7.97, 112.63], [-7.98, 112.64]]}))
    with pytest.raises(SystemExit, match=r"^error: .*probably swapped"):
        make_source(parse_args(["--latlon", str(path), "--dry-run"]), random.Random(1))


def test_missing_latlon_file_exits_with_the_reason(tmp_path):
    with pytest.raises(SystemExit, match=r"^error: .*No such file"):
        make_source(parse_args(["--latlon", str(tmp_path / "none.geojson")]), random.Random(1))


# ---- Riding: status line and speed ------------------------------------------

def test_status_says_where_the_rider_is_and_what_comes_next(tmp_path, capsys):
    path = write_route(tmp_path, L_ROUTE)
    source = make_source(parse_args(["--latlon", str(path), "--speed-kmh", "36"]), random.Random(1))
    nav = None
    for _ in range(50):  # 10 s at 10 m/s
        nav = source.step(0.2)
    assert source.status(nav) == "at   100 m | next: left in 900 m | 1400 m to go | 36 km/h"
    source.speed_kmh = 0  # stand still
    source.step(0.2)
    assert source.status(source.step(0.2)).startswith("at   100 m |")


def test_status_after_the_last_turn(tmp_path):
    path = write_route(tmp_path, L_ROUTE)
    source = make_source(parse_args(["--latlon", str(path), "--speed-kmh", "180"]), random.Random(1))
    nav = None
    for _ in range(110):  # 1100 m at 50 m/s: past the turn at 1000 m, 400 m before the end
        nav = source.step(0.2)
    assert "| no turn ahead, end of route in 400 m | 400 m to go |" in source.status(nav)


def test_speed_alias_and_limits(capsys):
    assert parse_args(["--latlon", "--speed", "25"]).speed == 25.0
    for bad in (["--latlon", "--speed-kmh", "-1"], ["--latlon", "--speed-kmh", "300"],
                ["--synthetic", "--maneuvers", "ors"]):
        with pytest.raises(SystemExit):
            parse_args(bad)


@pytest.mark.parametrize("typed,speed,paused", [
    ("10\n", 10.0, False),
    ("7.5 km/h\n", 7.5, False),
    ("0\n", 0.0, False),
    ("\n", None, True),
    ("fast\n", None, False),
    ("999\n", None, False),
])
def test_keyboard(monkeypatch, capsys, typed, speed, paused):
    monkeypatch.setattr(ble_sender.sys, "stdin", io.StringIO(typed))
    controls = Controls(None)
    controls.on_stdin()
    assert (controls.speed_kmh, controls.paused(), controls.quit) == (speed, paused, False)
    if speed is None and not paused:
        assert "keys:" in capsys.readouterr().out


def test_q_quits(monkeypatch):
    monkeypatch.setattr(ble_sender.sys, "stdin", io.StringIO("q\n"))
    controls = Controls(None)
    controls.on_stdin()
    assert controls.quit
