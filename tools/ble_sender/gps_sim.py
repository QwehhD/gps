"""A pretend GPS riding along a lat/lon route, plus the sample route and a
loader for route files. Stands in for the phone's GPS while testing."""

from __future__ import annotations

import json
import random
from pathlib import Path

from navgeo import LatLon, Route, from_local_m

# Around Monas, Jakarta: north on Jl. Medan Merdeka Barat, three right turns
# around the park (Utara, Timur, Selatan), then left onto Jl. M.H. Thamrin
# going south, with a gentle bend. About 4.5 km; points every ~100 m on the
# straights so simplification has something to do.
SAMPLE_ROUTE: list[LatLon] = [
    (-6.1790, 106.8225), (-6.1780, 106.8225), (-6.1770, 106.8225), (-6.1760, 106.8225),
    (-6.1750, 106.8225), (-6.1740, 106.8225), (-6.1730, 106.8225), (-6.1720, 106.8225),
    (-6.1710, 106.8225), (-6.1700, 106.8225),
    (-6.1700, 106.8235), (-6.1700, 106.8245), (-6.1700, 106.8255), (-6.1700, 106.8265),
    (-6.1700, 106.8275), (-6.1700, 106.8285), (-6.1700, 106.8295), (-6.1700, 106.8305),
    (-6.1710, 106.8305), (-6.1720, 106.8305), (-6.1730, 106.8305), (-6.1740, 106.8305),
    (-6.1750, 106.8305), (-6.1760, 106.8305), (-6.1770, 106.8305), (-6.1780, 106.8305),
    (-6.1790, 106.8305), (-6.1800, 106.8305), (-6.1810, 106.8305),
    (-6.1810, 106.8295), (-6.1810, 106.8285), (-6.1810, 106.8275), (-6.1810, 106.8265),
    (-6.1810, 106.8255), (-6.1810, 106.8245), (-6.1810, 106.8235), (-6.1810, 106.8225),
    (-6.1820, 106.8226), (-6.1830, 106.8227), (-6.1840, 106.8229), (-6.1850, 106.8232),
    (-6.1860, 106.8236), (-6.1870, 106.8239), (-6.1880, 106.8241), (-6.1890, 106.8242),
    (-6.1900, 106.8242),
]


def load_route(path: str | Path) -> list[LatLon]:
    """The points of a route file; see read_route_file()."""
    return read_route_file(path)[0]


def read_route_file(path: str | Path) -> tuple[list[LatLon], str]:
    """Reads a route from JSON and returns (points, what was read).

    Accepted: {"points": [[lat, lon], ...]}, or GeoJSON: a LineString
    geometry, a Feature holding one, or a FeatureCollection, of which the
    first LineString is used. That covers the OpenRouteService answers saved
    by fetch_route.py and lines drawn on geojson.io (whose Point markers are
    skipped). GeoJSON coordinates are [lon, lat]; a latitude outside -90..90
    means the order is swapped, and the file is refused."""
    try:
        data = json.loads(Path(path).read_bytes())
    except json.JSONDecodeError as e:
        raise ValueError(f"{path}: not valid JSON ({e})") from None

    if isinstance(data, dict) and "points" in data:
        return _positions(path, data["points"], lat_first=True), "points list"

    geometries = _geometries(data)
    lines = [g for g in geometries if g.get("type") == "LineString"]
    if not lines:
        found = ", ".join(sorted({str(g.get("type")) for g in geometries})) or "nothing"
        raise ValueError(f"{path}: no GeoJSON LineString found (found: {found}); expected a LineString, "
                         "a Feature or FeatureCollection holding one, or {\"points\": [[lat, lon], ...]}")
    source = f"GeoJSON {data['type']}"
    if len(lines) > 1:
        source += f", first of {len(lines)} LineStrings"
    return _positions(path, lines[0].get("coordinates"), lat_first=False), source


def _geometries(obj) -> list[dict]:
    """The geometries in a GeoJSON object, in file order: the object itself,
    a Feature's geometry, or those of every feature in a FeatureCollection."""
    if not isinstance(obj, dict):
        return []
    kind = obj.get("type")
    if kind == "FeatureCollection":
        features = obj.get("features")
        return [g for f in features for g in _geometries(f)] if isinstance(features, list) else []
    if kind == "Feature":
        return _geometries(obj.get("geometry"))
    return [obj] if kind else []


def _positions(path: str | Path, coords, lat_first: bool) -> list[LatLon]:
    """(lat, lon) points from [lat, lon] pairs, or from GeoJSON [lon, lat]
    positions (a third value, the elevation, is ignored)."""
    def is_number(v) -> bool:
        return isinstance(v, (int, float)) and not isinstance(v, bool)

    if not isinstance(coords, list) or not all(
            isinstance(c, list) and len(c) >= 2 and is_number(c[0]) and is_number(c[1]) for c in coords):
        raise ValueError(f"{path}: the coordinates must be a list of [number, number] pairs")
    if len(coords) < 2:
        raise ValueError(f"{path}: a route needs at least 2 points, the file has {len(coords)}")
    points = [(float(c[0]), float(c[1])) if lat_first else (float(c[1]), float(c[0])) for c in coords]

    if lat_first:
        order = '"points" are [latitude, longitude]'
    else:
        order = "GeoJSON coordinates are [longitude, latitude]"
    bad = [i for i, (lat, _) in enumerate(points) if not -90.0 <= lat <= 90.0]
    if bad:
        raise ValueError(f"{path}: {len(bad)} of {len(points)} points have a latitude outside -90..90 "
                         f"(first: point {bad[0]}, {coords[bad[0]][:2]}); {order}, so the order in "
                         "this file is probably swapped")
    bad = [i for i, (_, lon) in enumerate(points) if not -180.0 <= lon <= 180.0]
    if bad:
        raise ValueError(f"{path}: {len(bad)} of {len(points)} points have a longitude outside -180..180 "
                         f"(first: point {bad[0]}, {coords[bad[0]][:2]}); {order}")
    return points


class FakeGps:
    """Moves along the route at a fixed speed and reports noisy fixes.
    Starts over from the beginning after the end."""

    def __init__(self, route: Route, speed_kmh: float, noise_m: float = 0.0,
                 rng: random.Random | None = None):
        self.route = route
        self.speed_kmh = speed_kmh
        self.noise_m = noise_m
        self.rng = rng or random.Random()
        self.s = 0.0

    def step(self, dt: float) -> LatLon:
        self.s += self.speed_kmh / 3.6 * dt
        if self.s > self.route.length:
            self.s = 0.0
        x, y = self.route.xy_at(self.s)
        if self.noise_m > 0.0:
            x += self.rng.gauss(0.0, self.noise_m)
            y += self.rng.gauss(0.0, self.noise_m)
        return from_local_m(x, y, self.route.lat0, self.route.lon0)
