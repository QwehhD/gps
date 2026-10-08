"""Wire format of Butamap navigation data: the 'N' message and its BLE frames.

Mirrors src/nav_codec.c (the message) and src/nav_frag.c (the frames) in the
firmware. tests/test_navproto.py checks the same golden byte vectors as the C
unit tests, so both ends agree on the layout.

'N' message, little-endian (one complete route window):

    0     type 'N' (0x4E)
    1     sequence number, wraps 255 -> 0
    2     maneuver (Maneuver)
    3     flags: bit 0 = route valid (set exactly when N >= 2)
    4-5   distance to the maneuver, uint16, metres
    6-7   remaining route distance, uint16, tens of metres
    8     speed, uint8, km/h
    9     route point count N (0-32)
    10    N x (int16 x, int16 y), 1/16 unit
    ...   side road count M (0-8), then per road: K (2-4), K x (int16 x, int16 y)

Frame (one BLE write): 0xCE, message id, chunk index, chunk count (1-32),
then the next bytes of the message.
"""

from __future__ import annotations

import enum
import math
import struct
from dataclasses import dataclass, field

MSG_TYPE_NAV = 0x4E
MSG_HEADER_SIZE = 10
MSG_MAX_SIZE = 275
FLAG_ROUTE_VALID = 0x01
COORD_SCALE = 16
MAX_ROUTE_POINTS = 32
MAX_SIDE_ROADS = 8
MAX_SIDE_ROAD_POINTS = 4

FRAME_TYPE = 0xCE
FRAME_HEADER_SIZE = 4
FRAME_MAX_CHUNKS = 32
MIN_FRAME = 20  # ATT MTU 23 - 3: the write size every BLE link supports

Point = tuple[float, float]


class Maneuver(enum.IntEnum):
    STRAIGHT = 0
    TURN_LEFT = 1
    TURN_RIGHT = 2
    U_TURN = 3


@dataclass
class NavData:
    """Everything the display needs; same fields as nav_data_t.

    Coordinates are schematic units (4 px each on screen), heading-up: the
    rider at (0, 0), +y ahead, +x to the right.
    """

    maneuver: Maneuver = Maneuver.STRAIGHT
    distance_to_turn_m: float = 0.0
    total_distance_m: float = 0.0
    speed_kmh: float = 0.0
    route: list[Point] = field(default_factory=list)
    side_roads: list[list[Point]] = field(default_factory=list)


def _quantize(value: float, scale: float, lo: int, hi: int) -> int:
    """Rounds value * scale to the nearest integer, halves away from zero
    (like C roundf), clamped to lo..hi. NaN becomes 0."""
    if math.isnan(value):
        value = 0.0
    scaled = value * scale
    if scaled <= lo:
        return lo
    if scaled >= hi:
        return hi
    return int(math.copysign(math.floor(abs(scaled) + 0.5), scaled))


def _pack_point(point: Point) -> bytes:
    return struct.pack(
        "<hh",
        _quantize(point[0], COORD_SCALE, -32768, 32767),
        _quantize(point[1], COORD_SCALE, -32768, 32767),
    )


def encode(nav: NavData, seq: int) -> bytes:
    """Packs nav into one 'N' message, clamping like the firmware's
    nav_encode(): counts to their maximum, values to their field, and side
    roads with fewer than 2 points left out."""
    maneuver = Maneuver(nav.maneuver)
    route = list(nav.route[:MAX_ROUTE_POINTS])
    sides = [road[:MAX_SIDE_ROAD_POINTS] for road in nav.side_roads[:MAX_SIDE_ROADS]]
    sides = [road for road in sides if len(road) >= 2]

    out = bytearray(
        [MSG_TYPE_NAV, seq & 0xFF, maneuver, FLAG_ROUTE_VALID if len(route) >= 2 else 0]
    )
    out += struct.pack(
        "<HHB",
        _quantize(nav.distance_to_turn_m, 1.0, 0, 0xFFFF),
        _quantize(nav.total_distance_m, 0.1, 0, 0xFFFF),
        _quantize(nav.speed_kmh, 1.0, 0, 0xFF),
    )
    out.append(len(route))
    for point in route:
        out += _pack_point(point)
    out.append(len(sides))
    for road in sides:
        out.append(len(road))
        for point in road:
            out += _pack_point(point)
    return bytes(out)


def decode(msg: bytes) -> tuple[NavData, int]:
    """Unpacks one 'N' message into (NavData, seq), validating it as strictly
    as the firmware's nav_decode(). Raises ValueError if it is malformed."""

    def need(pos: int, count: int) -> None:
        if len(msg) < pos + count:
            raise ValueError("truncated")

    need(0, MSG_HEADER_SIZE)
    if msg[0] != MSG_TYPE_NAV:
        raise ValueError("unknown message type")
    if msg[2] >= len(Maneuver):
        raise ValueError("bad maneuver")
    n = msg[9]
    if n > MAX_ROUTE_POINTS:
        raise ValueError("too many route points")
    if bool(msg[3] & FLAG_ROUTE_VALID) != (n >= 2):
        raise ValueError("route flag disagrees with point count")
    distance, total, speed = struct.unpack_from("<HHB", msg, 4)

    def points(pos: int, count: int) -> list[Point]:
        need(pos, 4 * count)
        return [
            (x / COORD_SCALE, y / COORD_SCALE)
            for x, y in struct.iter_unpack("<hh", msg[pos : pos + 4 * count])
        ]

    pos = MSG_HEADER_SIZE
    route = points(pos, n)
    pos += 4 * n
    need(pos, 1)
    m = msg[pos]
    pos += 1
    if m > MAX_SIDE_ROADS:
        raise ValueError("too many side roads")
    sides = []
    for _ in range(m):
        need(pos, 1)
        k = msg[pos]
        pos += 1
        if not 2 <= k <= MAX_SIDE_ROAD_POINTS:
            raise ValueError("bad side road point count")
        sides.append(points(pos, k))
        pos += 4 * k
    if pos != len(msg):
        raise ValueError("trailing bytes")
    nav = NavData(Maneuver(msg[2]), float(distance), total * 10.0, float(speed), route, sides)
    return nav, msg[1]


def frame_count(msg_len: int, frame_size: int) -> int:
    """Frames needed to send msg_len bytes in writes of at most frame_size
    bytes, or 0 if it cannot be done within FRAME_MAX_CHUNKS frames."""
    if frame_size <= FRAME_HEADER_SIZE or not 0 < msg_len <= MSG_MAX_SIZE:
        return 0
    count = math.ceil(msg_len / (frame_size - FRAME_HEADER_SIZE))
    return count if count <= FRAME_MAX_CHUNKS else 0


def split_frames(msg: bytes, msg_id: int, frame_size: int) -> list[bytes]:
    """Splits a message into frames of at most frame_size bytes."""
    count = frame_count(len(msg), frame_size)
    if count == 0:
        raise ValueError(
            f"cannot send a {len(msg)} B message in frames of {frame_size} B "
            f"(at most {FRAME_MAX_CHUNKS} frames of {FRAME_HEADER_SIZE} B header + payload)"
        )
    per_frame = frame_size - FRAME_HEADER_SIZE
    return [
        bytes([FRAME_TYPE, msg_id & 0xFF, i, count]) + msg[i * per_frame : (i + 1) * per_frame]
        for i in range(count)
    ]


def smallest_frame_size() -> int:
    """Smallest frame size that still carries the largest message."""
    return FRAME_HEADER_SIZE + math.ceil(MSG_MAX_SIZE / FRAME_MAX_CHUNKS)
