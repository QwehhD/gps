import argparse
import io
import json
import re
import urllib.error

import pytest

import fetch_route
from fetch_route import (
    ORS_URL,
    RouteError,
    describe_route,
    parse_args,
    parse_point,
    read_api_key,
    request_route,
)
from gps_sim import load_route

KEY = "secret-test-key-123"

# Trimmed ORS Directions V2 GeoJSON answer; coordinates are [lon, lat].
ORS_ROUTE = json.dumps({
    "type": "FeatureCollection",
    "bbox": [112.6308, -7.9826, 112.6326, -7.9666],
    "features": [{
        "type": "Feature",
        "bbox": [112.6308, -7.9826, 112.6326, -7.9666],
        "properties": {"summary": {"distance": 2345.6, "duration": 312.4}},
        "geometry": {"type": "LineString",
                     "coordinates": [[112.6326, -7.9666], [112.6320, -7.9750], [112.6308, -7.9826]]},
    }],
}).encode()


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def http_error(status: int, body: bytes):
    def urlopen(request, timeout):
        raise urllib.error.HTTPError(request.full_url, status, "error", {}, io.BytesIO(body))
    return urlopen


# ---- Points -----------------------------------------------------------------

@pytest.mark.parametrize("text", ["-7.966620,112.632632", "-7.966620, 112.632632", " -7.966620 ,112.632632 "])
def test_point_is_read_latitude_first(text):
    assert parse_point(text) == (-7.96662, 112.632632)


def test_point_written_lon_lat_is_rejected_with_the_fix():
    with pytest.raises(argparse.ArgumentTypeError, match=r"looks like LON,LAT.*-7\.97,112\.63"):
        parse_point("112.63,-7.97")


@pytest.mark.parametrize("text,message", [
    ("95,200", "latitude 95 is outside"),
    ("-7.97,190", "longitude 190 is outside"),
    ("-7.97", "expected LAT,LON"),
    ("-7.97,112.63,5", "expected LAT,LON"),
    ("south,east", "expected LAT,LON"),
])
def test_bad_points_are_rejected(text, message):
    with pytest.raises(argparse.ArgumentTypeError, match=message):
        parse_point(text)


def test_negative_latitude_is_not_taken_for_an_option():
    args = parse_args(["--from", "-7.97,112.63", "--to", "-7.98,112.64", "--name", "kampus"])
    assert (args.start, args.to, args.name) == ((-7.97, 112.63), (-7.98, 112.64), "kampus")


def test_help_states_the_point_order(capsys):
    with pytest.raises(SystemExit):
        parse_args(["--help"])
    out = capsys.readouterr().out
    assert "LAT,LON" in out
    assert "latitude first" in out


@pytest.mark.parametrize("argv,message", [
    (["--from", "-7.97,112.63", "--to", "-7.97, 112.63", "--name", "x"], "same point"),
    (["--from", "-7.97,112.63", "--to", "-7.98,112.64", "--name", "../x"], "--name may only"),
    (["--from", "-7.97,112.63", "--to", "-7.98,112.64", "--name", "x.geojson"], "--name may only"),
    (["--from", "112.63,-7.97", "--to", "-7.98,112.64", "--name", "x"], "looks like LON,LAT"),
])
def test_bad_arguments_are_refused(argv, message, capsys):
    with pytest.raises(SystemExit):
        parse_args(argv)
    assert message in capsys.readouterr().err


# ---- API key ----------------------------------------------------------------

def test_key_from_the_environment_wins(tmp_path):
    env_file = tmp_path / ".env"
    env_file.write_text("ORS_API_KEY=from-file\n")
    assert read_api_key({"ORS_API_KEY": "from-env"}, env_file) == "from-env"


def test_key_from_the_env_file(tmp_path):
    env_file = tmp_path / ".env"
    env_file.write_text('# OpenRouteService\nOTHER=1\n\nexport ORS_API_KEY = "from-file"\n')
    assert read_api_key({}, env_file) == "from-file"


def test_no_key(tmp_path):
    assert read_api_key({}, tmp_path / ".env") is None
    (tmp_path / ".env").write_text("ORS_API_KEY=\n")
    assert read_api_key({"ORS_API_KEY": "  "}, tmp_path / ".env") is None


# ---- Request ----------------------------------------------------------------

def test_request_sends_lon_lat_and_the_key_in_a_header():
    seen = {}

    def urlopen(request, timeout):
        seen["request"] = request
        return FakeResponse(ORS_ROUTE)

    assert request_route((-7.97, 112.63), (-7.98, 112.64), KEY, urlopen) == ORS_ROUTE
    request = seen["request"]
    assert request.full_url == ORS_URL
    assert KEY not in request.full_url
    assert request.get_method() == "POST"
    assert request.get_header("Authorization") == KEY
    assert json.loads(request.data) == {"coordinates": [[112.63, -7.97], [112.64, -7.98]]}


@pytest.mark.parametrize("status,body,message", [
    (403, b'{"error": "Access to this API has been disallowed"}', "rejected the API key"),
    (401, b'{"error": "Authorization field missing"}', "rejected the API key"),
    (429, b'{"error": "Rate Limit Exceeded"}', "quota or rate limit"),
    (404, b'{"error": {"code": 2010, "message": "Could not find routable point within a radius '
          b'of 350.0 meters of specified coordinate 0: 112.6300000 -7.9700000."}}', "no road found"),
    (404, b'{"error": {"code": 2009, "message": "Route could not be found"}}', "no driving route"),
    (400, b'{"error": {"code": 2004, "message": "Request parameters exceed the server limits"}}',
     "exceeds ORS limits"),
    (503, b"<html>Service Unavailable</html>", "server error (HTTP 503)"),
    (400, b'{"error": {"code": 2003, "message": "Parameter coordinates has incorrect value"}}',
     "refused the request (HTTP 400) (ORS: Parameter coordinates"),
])
def test_api_errors_are_explained(status, body, message):
    with pytest.raises(RouteError, match=re.escape(message)):
        request_route((-7.97, 112.63), (-7.98, 112.64), KEY, http_error(status, body))


def test_network_error_is_explained():
    def urlopen(request, timeout):
        raise urllib.error.URLError("Name or service not known")

    with pytest.raises(RouteError, match="cannot reach OpenRouteService"):
        request_route((-7.97, 112.63), (-7.98, 112.64), KEY, urlopen)


@pytest.mark.parametrize("body", [b"not json", b'{"type": "FeatureCollection", "features": []}',
                                  b'{"features": [{"geometry": {"type": "Point", "coordinates": [1, 2]}}]}'])
def test_a_response_without_a_route_is_not_saved(body):
    with pytest.raises(RouteError):
        describe_route(body)


# ---- Whole script -----------------------------------------------------------

ARGV = ["--from", "-7.9666, 112.6326", "--to", "-7.9826,112.6308", "--name", "kampus"]


@pytest.fixture
def isolated(tmp_path, monkeypatch):
    """Routes and .env in tmp_path, the key in the environment, no network."""
    monkeypatch.setattr(fetch_route, "ROUTES_DIR", tmp_path / "routes")
    monkeypatch.setattr(fetch_route, "ENV_FILE", tmp_path / ".env")
    monkeypatch.setenv("ORS_API_KEY", KEY)
    calls = []

    def fake_request(start, end, key):
        calls.append((start, end, key))
        return ORS_ROUTE

    monkeypatch.setattr(fetch_route, "request_route", fake_request)
    return tmp_path, calls


def test_saves_the_response_unchanged_for_latlon_mode(isolated, capsys):
    tmp_path, calls = isolated
    fetch_route.main(ARGV)

    saved = tmp_path / "routes" / "kampus.geojson"
    assert saved.read_bytes() == ORS_ROUTE
    assert calls == [((-7.9666, 112.6326), (-7.9826, 112.6308), KEY)]
    assert load_route(saved) == [(-7.9666, 112.6326), (-7.975, 112.632), (-7.9826, 112.6308)]
    out = capsys.readouterr().out
    assert "3 points, 2.3 km, about 5 min" in out
    assert KEY not in out


def test_an_existing_route_is_kept_unless_forced(isolated):
    tmp_path, calls = isolated
    saved = tmp_path / "routes" / "kampus.geojson"
    saved.parent.mkdir()
    saved.write_text("old")

    with pytest.raises(SystemExit, match="already exists"):
        fetch_route.main(ARGV)
    assert (saved.read_text(), calls) == ("old", [])

    fetch_route.main([*ARGV, "--force"])
    assert saved.read_bytes() == ORS_ROUTE


def test_missing_key_is_explained(isolated, monkeypatch):
    monkeypatch.delenv("ORS_API_KEY")
    with pytest.raises(SystemExit, match="no API key"):
        fetch_route.main(ARGV)


def test_the_key_never_reaches_the_output(isolated, monkeypatch, capsys):
    tmp_path, _ = isolated

    def failing_request(start, end, key):
        raise RouteError(f"something echoed {key} back")

    monkeypatch.setattr(fetch_route, "request_route", failing_request)
    with pytest.raises(SystemExit) as exit_info:
        fetch_route.main(ARGV)
    assert KEY not in str(exit_info.value)
    assert "***" in str(exit_info.value)
    assert KEY not in "".join(capsys.readouterr())
    assert not (tmp_path / "routes" / "kampus.geojson").exists()
