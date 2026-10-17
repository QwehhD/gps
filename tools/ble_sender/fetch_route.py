#!/usr/bin/env python3
"""Fetches a driving route from OpenRouteService (Directions V2, profile
driving-car) once and saves the GeoJSON response unchanged to
routes/<name>.geojson, so --latlon runs replay it without internet or quota:

    python fetch_route.py --from "-7.9666,112.6326" --to "-7.9826,112.6308" --name kampus
    python ble_sender.py --latlon routes/kampus.geojson

Points are written LAT,LON: latitude first, the order Google Maps shows when
you right-click a spot (e.g. "-7.966620, 112.632632"). ORS wants lon,lat;
this script swaps them for you, so do not swap them yourself.

The API key is read from the ORS_API_KEY environment variable, or from an
ORS_API_KEY=... line in tools/ble_sender/.env. It is never printed.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from collections.abc import Mapping
from pathlib import Path

from navgeo import LatLon

ORS_URL = "https://api.openrouteservice.org/v2/directions/driving-car/geojson"
KEY_VAR = "ORS_API_KEY"
HERE = Path(__file__).resolve().parent
ENV_FILE = HERE / ".env"
ROUTES_DIR = HERE / "routes"
TIMEOUT_S = 30.0
NAME_RE = re.compile(r"[A-Za-z0-9_-]+")


class RouteError(Exception):
    """A failure explained in words the user can act on."""


def parse_point(text: str) -> LatLon:
    """Parses "lat,lon" (spaces allowed, as Google Maps copies it). Rejects a
    point that looks like lon,lat with the corrected order in the message."""
    parts = [part.strip() for part in text.split(",")]
    try:
        lat, lon = (float(part) for part in parts)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r}: expected LAT,LON, e.g. -7.97,112.63") from None
    if not -90.0 <= lat <= 90.0:
        if -90.0 <= lon <= 90.0 and -180.0 <= lat <= 180.0:
            raise argparse.ArgumentTypeError(
                f"{text!r} looks like LON,LAT (latitude {parts[0]} is outside -90..90); "
                f"write latitude first: {parts[1]},{parts[0]}")
        raise argparse.ArgumentTypeError(f"{text!r}: latitude {parts[0]} is outside -90..90")
    if not -180.0 <= lon <= 180.0:
        raise argparse.ArgumentTypeError(f"{text!r}: longitude {parts[1]} is outside -180..180")
    return lat, lon


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--from", dest="start", required=True, type=parse_point, metavar="LAT,LON",
                   help="start point, latitude first (Google Maps order), e.g. \"-7.97,112.63\"")
    p.add_argument("--to", required=True, type=parse_point, metavar="LAT,LON",
                   help="destination, latitude first (Google Maps order)")
    p.add_argument("--name", required=True,
                   help="file name without extension: saves routes/NAME.geojson")
    p.add_argument("--force", action="store_true", help="fetch again even if routes/NAME.geojson exists")
    args = p.parse_args(argv)

    if args.start == args.to:
        p.error("--from and --to are the same point")
    if not NAME_RE.fullmatch(args.name):
        p.error("--name may only contain letters, digits, - and _ (no folder, no extension)")
    return args


def read_api_key(environ: Mapping[str, str], env_file: Path) -> str | None:
    """ORS_API_KEY from the environment, else from env_file (KEY=value lines)."""
    key = environ.get(KEY_VAR, "").strip()
    if key:
        return key
    try:
        lines = env_file.read_text().splitlines()
    except FileNotFoundError:
        return None
    for line in lines:
        line = line.strip().removeprefix("export ").strip()
        name, sep, value = line.partition("=")
        if sep and name.strip() == KEY_VAR:
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            return value or None
    return None


def ors_error(body: bytes) -> tuple[int | None, str | None]:
    """(code, message) from an ORS error body: {"error": {"code", "message"}}
    from the routing engine, {"error": "..."} from the API gateway."""
    try:
        error = json.loads(body).get("error")
    except (ValueError, AttributeError):
        return None, None
    if isinstance(error, dict):
        return error.get("code"), error.get("message")
    if isinstance(error, str):
        return None, error
    return None, None


def explain_http_error(status: int, body: bytes) -> str:
    code, message = ors_error(body)
    detail = f" (ORS: {message})" if message else ""
    if status in (401, 403):
        return f"OpenRouteService rejected the API key (HTTP {status}){detail}; check {KEY_VAR}"
    if status == 429:
        return (f"OpenRouteService quota or rate limit reached (HTTP 429){detail}; wait a minute "
                "for the per-minute limit, or until the daily quota resets (see the ORS dashboard)")
    if code == 2010:
        return (f"no road found near one of the points{detail}; check that both points are on or "
                "next to a road and are written LAT,LON")
    if code == 2009:
        return f"no driving route between the points{detail}"
    if code == 2004:
        return f"the request exceeds ORS limits, e.g. the route is too long{detail}"
    if status >= 500:
        return f"OpenRouteService server error (HTTP {status}){detail}; try again later"
    return f"OpenRouteService refused the request (HTTP {status}){detail}"


def request_route(start: LatLon, end: LatLon, key: str, urlopen=urllib.request.urlopen) -> bytes:
    """POSTs the two points (as lon,lat) and returns the raw GeoJSON response.
    The key goes in a header, never in the URL, so it cannot leak into errors."""
    body = json.dumps({"coordinates": [[start[1], start[0]], [end[1], end[0]]]}).encode()
    request = urllib.request.Request(ORS_URL, data=body, method="POST", headers={
        "Authorization": key,
        "Content-Type": "application/json; charset=utf-8",
        "Accept": "application/geo+json, application/json",
    })
    try:
        with urlopen(request, timeout=TIMEOUT_S) as response:
            return response.read()
    except urllib.error.HTTPError as e:
        raise RouteError(explain_http_error(e.code, e.read())) from None
    except urllib.error.URLError as e:
        raise RouteError(f"cannot reach OpenRouteService ({e.reason}); check the internet connection") from None
    except TimeoutError:
        raise RouteError(f"OpenRouteService did not answer within {TIMEOUT_S:g} s; try again later") from None


def describe_route(body: bytes) -> str:
    """Checks the response holds a route line and summarises it."""
    try:
        feature = json.loads(body)["features"][0]
        geometry = feature["geometry"]
        points = len(geometry["coordinates"])
        summary = feature.get("properties", {}).get("summary", {})
    except (ValueError, KeyError, IndexError, TypeError, AttributeError):
        raise RouteError("OpenRouteService answered with something that is not a GeoJSON route") from None
    if geometry.get("type") != "LineString" or points < 2:
        raise RouteError("OpenRouteService answered without a route line")
    return (f"{points} points, {summary.get('distance', 0.0) / 1000:.1f} km, "
            f"about {summary.get('duration', 0.0) / 60:.0f} min")


def main(argv: list[str] | None = None) -> None:
    args = parse_args(argv)
    path = ROUTES_DIR / f"{args.name}.geojson"
    shown = f"routes/{path.name}"
    if path.exists() and not args.force:
        sys.exit(f"error: {shown} already exists and tests replay it; "
                 "pick another --name, or --force to fetch it again")

    key = read_api_key(os.environ, ENV_FILE)
    if not key:
        sys.exit(f"error: no API key: set {KEY_VAR}, or put {KEY_VAR}=<key> in tools/ble_sender/.env")

    print("fetching the route from OpenRouteService (driving-car)...", flush=True)
    try:
        body = request_route(args.start, args.to, key)
        description = describe_route(body)
    except RouteError as e:
        sys.exit(f"error: {str(e).replace(key, '***')}")

    ROUTES_DIR.mkdir(exist_ok=True)
    path.write_bytes(body)
    print(f"saved {shown}: {description}")
    print(f"replay it with: python ble_sender.py --latlon {shown}")


if __name__ == "__main__":
    main()
