import json
import math

import pytest

import make_test_vectors
from make_test_vectors import CASES, ORIGIN, OUT_DIR, build, render
from navgeo import M_PER_DEG_LAT
from navproto import Maneuver, decode

NAMES = sorted(CASES)


def load(name: str) -> dict:
    return json.loads((OUT_DIR / f"{name}.json").read_text())


def test_the_files_are_up_to_date():
    for name in NAMES:
        assert (OUT_DIR / f"{name}.json").read_text() == render(build(name)), (
            f"{name}.json is out of date: run python make_test_vectors.py")
    assert sorted(p.stem for p in OUT_DIR.glob("*.json")) == NAMES
    make_test_vectors.main(["--check"])  # the same check, as the script runs it


def test_the_requested_cases_are_there():
    maneuvers = {name: load(name)["expected"]["maneuver"] for name in NAMES}
    assert maneuvers["right_turn"] == "TURN_RIGHT"
    assert maneuvers["left_turn"] == "TURN_LEFT"
    assert maneuvers["u_turn"] == "U_TURN"
    assert maneuvers["straight"] == "STRAIGHT"
    near_end = load("near_end")["expected"]
    assert (near_end["maneuver"], round(near_end["total_distance_m"])) == ("STRAIGHT", 30)
    assert len(load("curve_simplified")["input"]["route_latlon"]) > 32


@pytest.mark.parametrize("name", NAMES)
def test_bytes_agree_with_the_expected_values(name):
    vector = load(name)
    expected = vector["expected"]
    nav, seq = decode(bytes.fromhex(expected["message_hex"]))
    assert seq == vector["input"]["seq"]
    assert nav.maneuver == Maneuver[expected["maneuver"]]
    assert nav.distance_to_turn_m == round(expected["distance_to_turn_m"])
    assert nav.total_distance_m == 10 * round(expected["total_distance_m"] / 10)
    assert nav.route == pytest.approx(
        [(round(x * 16) / 16, round(y * 16) / 16) for x, y in expected["window_units"]], abs=1e-9)
    for size, frames in expected["frames_hex"].items():
        frames = [bytes.fromhex(f) for f in frames]
        assert all(len(f) <= int(size) for f in frames)
        assert b"".join(f[4:] for f in frames).hex() == expected["message_hex"]


@pytest.mark.parametrize("name", NAMES)
def test_only_synthetic_coordinates(name):
    """Every point stays within 5 km of the synthetic origin, so a personal
    route cannot slip into these committed files."""
    vector = load(name)["input"]
    for lat, lon in vector["route_latlon"] + [vector["fix_latlon"]]:
        north = (lat - ORIGIN[0]) * M_PER_DEG_LAT
        east = (lon - ORIGIN[1]) * 111320.0 * math.cos(math.radians(ORIGIN[0]))
        assert math.hypot(east, north) < 5000.0


def test_a_rounding_tie_is_refused(monkeypatch):
    # The left turn is exactly 120 m ahead; a fix 0.5 m further back would
    # put the distance on a tie (120.5 m).
    description, legs, turns, fix_s, right, previous, speed, seq = CASES["left_turn"]
    monkeypatch.setitem(CASES, "left_turn", (description, legs, turns, fix_s - 0.5, right, previous, speed, seq))
    with pytest.raises(ValueError, match="rounding tie"):
        build("left_turn")
