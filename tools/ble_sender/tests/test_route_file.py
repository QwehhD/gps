import json

import pytest

from gps_sim import load_route, read_route_file

# [lon, lat] as GeoJSON writes them, and the (lat, lon) points they mean.
COORDS = [[112.6326, -7.9666], [112.632, -7.975], [112.6308, -7.9826]]
POINTS = [(-7.9666, 112.6326), (-7.975, 112.632), (-7.9826, 112.6308)]

LINE = {"type": "LineString", "coordinates": COORDS}


def write(tmp_path, data, name="route.geojson"):
    path = tmp_path / name
    path.write_text(json.dumps(data) if not isinstance(data, str) else data)
    return path


def feature(geometry):
    return {"type": "Feature", "properties": {}, "geometry": geometry}


# ---- Accepted shapes --------------------------------------------------------

def test_bare_linestring(tmp_path):
    assert read_route_file(write(tmp_path, LINE)) == (POINTS, "GeoJSON LineString")


def test_feature(tmp_path):
    assert read_route_file(write(tmp_path, feature(LINE))) == (POINTS, "GeoJSON Feature")


def test_openrouteservice_answer(tmp_path):
    ors = {
        "type": "FeatureCollection",
        "bbox": [112.6308, -7.9826, 112.6326, -7.9666],
        "features": [{
            "bbox": [112.6308, -7.9826, 112.6326, -7.9666],
            "type": "Feature",
            "properties": {"segments": [{"distance": 2345.6, "duration": 312.4, "steps": []}],
                           "way_points": [0, 2], "summary": {"distance": 2345.6, "duration": 312.4}},
            "geometry": {"coordinates": COORDS, "type": "LineString"},
        }],
        "metadata": {"service": "routing"},
    }
    assert read_route_file(write(tmp_path, ors)) == (POINTS, "GeoJSON FeatureCollection")


def test_geojson_io_drawing_uses_the_first_line_and_skips_markers(tmp_path):
    drawing = {"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {"marker-color": "#7e7e7e"},
         "geometry": {"type": "Point", "coordinates": COORDS[0]}, "id": 0},
        {"type": "Feature", "properties": {}, "geometry": None, "id": 1},
        {**feature(LINE), "id": 2},
        {**feature({"type": "LineString", "coordinates": [[106.8, -6.1], [106.9, -6.2]]}), "id": 3},
    ]}
    points, kind = read_route_file(write(tmp_path, drawing))
    assert points == POINTS
    assert kind == "GeoJSON FeatureCollection, first of 2 LineStrings"


def test_elevation_is_ignored(tmp_path):
    line = {"type": "LineString", "coordinates": [[*c, 450.5] for c in COORDS]}
    assert load_route(write(tmp_path, line)) == POINTS


def test_points_list_is_latitude_first(tmp_path):
    path = write(tmp_path, {"points": [[lat, lon] for lat, lon in POINTS]}, "route.json")
    assert read_route_file(path) == (POINTS, "points list")


# ---- Refused files ----------------------------------------------------------

def test_swapped_geojson_order_is_refused(tmp_path):
    swapped = {"type": "LineString", "coordinates": [[lat, lon] for lon, lat in COORDS]}
    with pytest.raises(ValueError, match=r"3 of 3 points have a latitude outside -90\.\.90 "
                                         r"\(first: point 0, \[-7\.9666, 112\.6326\]\).*"
                                         r"\[longitude, latitude\].*probably swapped"):
        load_route(write(tmp_path, swapped))


def test_one_bad_latitude_is_enough(tmp_path):
    line = {"type": "LineString", "coordinates": [COORDS[0], [-7.975, 112.632], COORDS[2]]}
    with pytest.raises(ValueError, match=r"1 of 3 points .*first: point 1"):
        load_route(write(tmp_path, line))


def test_swapped_points_list_is_refused(tmp_path):
    path = write(tmp_path, {"points": COORDS}, "route.json")
    with pytest.raises(ValueError, match=r'"points" are \[latitude, longitude\].*probably swapped'):
        load_route(path)


def test_longitude_out_of_range_is_refused(tmp_path):
    line = {"type": "LineString", "coordinates": [[190.0, -7.9], [112.6, -7.9]]}
    with pytest.raises(ValueError, match=r"longitude outside -180\.\.180"):
        load_route(write(tmp_path, line))


@pytest.mark.parametrize("data,message", [
    ({"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": COORDS[0]}},
     r"no GeoJSON LineString found \(found: Point\)"),
    ({"type": "MultiLineString", "coordinates": [COORDS, COORDS]},
     r"no GeoJSON LineString found \(found: MultiLineString\)"),
    ({"type": "FeatureCollection", "features": []}, r"no GeoJSON LineString found \(found: nothing\)"),
    ([1, 2], r"no GeoJSON LineString found"),
    ({"type": "LineString", "coordinates": [COORDS[0]]}, r"at least 2 points, the file has 1"),
    ({"type": "LineString", "coordinates": [[112.6, "-7.9"], [112.7, -7.9]]}, r"\[number, number\] pairs"),
    ({"type": "LineString"}, r"\[number, number\] pairs"),
    ("{\"type\": \"LineString\", \"coordinates\": [[", r"not valid JSON"),
])
def test_unusable_files_are_explained(tmp_path, data, message):
    path = write(tmp_path, data)
    with pytest.raises(ValueError, match=message) as error:
        load_route(path)
    assert str(error.value).startswith(f"{path}: ")
