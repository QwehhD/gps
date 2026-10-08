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
    """Reads a route from JSON: either {"points": [[lat, lon], ...]} or a
    GeoJSON LineString (geometry, Feature, or the first LineString in a
    FeatureCollection; GeoJSON coordinates are [lon, lat])."""
    data = json.loads(Path(path).read_text())
    if isinstance(data, dict) and "points" in data:
        return [(float(lat), float(lon)) for lat, lon in data["points"]]

    def find_line(obj):
        kind = obj.get("type")
        if kind == "LineString":
            return obj["coordinates"]
        if kind == "Feature":
            return find_line(obj["geometry"])
        if kind == "FeatureCollection":
            for feature in obj["features"]:
                line = find_line(feature)
                if line:
                    return line
        return None

    line = find_line(data) if isinstance(data, dict) else None
    if not line:
        raise ValueError(f"{path}: expected {{'points': [[lat, lon], ...]}} or a GeoJSON LineString")
    return [(float(c[1]), float(c[0])) for c in line]


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
