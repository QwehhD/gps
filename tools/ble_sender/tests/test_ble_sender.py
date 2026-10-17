import json
import random

import pytest

from ble_sender import make_source, parse_args, route_summary
from navgeo import Route, from_local_m

LAT0, LON0 = -7.97, 112.63


def offset(east: float, north: float) -> tuple[float, float]:
    return from_local_m(east, north, LAT0, LON0)


def test_summary_lists_each_turn_with_direction_angle_and_position():
    # East 1000 m, left (north) for 500 m, right (east) again.
    route = Route([offset(0, 0), offset(1000, 0), offset(1000, 500), offset(1600, 500)])
    lines = route_summary(route, "test route").splitlines()
    assert lines[0] == "route: test route"
    assert lines[1] == "  4 points, 2100 m"
    assert lines[2].startswith("  2 turns")
    left_lat, left_lon = offset(1000, 0)
    right_lat, right_lon = offset(1000, 500)
    assert lines[4].split() == ["1", "left", "90°", "1000", "m", f"{left_lat:.6f},{left_lon:.6f}"]
    assert lines[5].split() == ["2", "right", "90°", "1500", "m", f"{right_lat:.6f},{right_lon:.6f}"]


def test_summary_of_a_straight_route():
    summary = route_summary(Route([offset(0, 0), offset(0, 800)]), "straight")
    assert summary.splitlines()[1:] == ["  2 points, 800 m", "  no turns sharper than 35°"]


def test_latlon_file_prints_the_summary_before_riding(tmp_path, capsys):
    path = tmp_path / "route.geojson"
    coords = [[lon, lat] for lat, lon in (offset(0, 0), offset(1000, 0), offset(1000, 500))]
    path.write_text(json.dumps({"type": "Feature", "properties": {},
                                "geometry": {"type": "LineString", "coordinates": coords}}))
    step = make_source(parse_args(["--latlon", str(path), "--dry-run"]), random.Random(1))
    out = capsys.readouterr().out
    assert f"route: {path} (GeoJSON Feature)" in out
    assert "  1 turn (" in out
    assert step(0.2).route  # rides it


def test_unusable_latlon_file_exits_with_the_reason(tmp_path):
    path = tmp_path / "swapped.geojson"
    path.write_text(json.dumps({"type": "LineString", "coordinates": [[-7.97, 112.63], [-7.98, 112.64]]}))
    with pytest.raises(SystemExit, match=r"^error: .*probably swapped"):
        make_source(parse_args(["--latlon", str(path), "--dry-run"]), random.Random(1))


def test_missing_latlon_file_exits_with_the_reason(tmp_path):
    with pytest.raises(SystemExit, match=r"^error: .*No such file"):
        make_source(parse_args(["--latlon", str(tmp_path / "none.geojson")]), random.Random(1))
