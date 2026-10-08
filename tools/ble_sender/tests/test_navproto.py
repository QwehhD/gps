import math

import pytest

from navproto import (
    FRAME_TYPE,
    MIN_FRAME,
    MSG_MAX_SIZE,
    Maneuver,
    NavData,
    decode,
    encode,
    frame_count,
    smallest_frame_size,
    split_frames,
)

# Same bytes as test_encode_layout_is_little_endian in
# test/test_nav_codec/test_nav_codec.c and the golden frames in
# test/test_nav_frag/test_nav_frag.c.
GOLDEN_MESSAGE = bytes([0x4E, 0xAB, 0x03, 0x01, 0x34, 0x12, 0x02, 0x01, 99, 2,
                        0xF0, 0xFF, 0x28, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00])
GOLDEN_FRAMES = [
    bytes([0xCE, 0xAB, 0, 5, 0x4E, 0xAB, 0x03, 0x01]),
    bytes([0xCE, 0xAB, 1, 5, 0x34, 0x12, 0x02, 0x01]),
    bytes([0xCE, 0xAB, 2, 5, 99, 2, 0xF0, 0xFF]),
    bytes([0xCE, 0xAB, 3, 5, 0x28, 0x00, 0x00, 0x00]),
    bytes([0xCE, 0xAB, 4, 5, 0x00, 0x05, 0x00]),
]


def golden_nav():
    return NavData(Maneuver.U_TURN, 0x1234, 0x0102 * 10.0, 99.0, [(-1.0, 2.5), (0.0, 80.0)])


def largest_nav():
    return NavData(
        Maneuver.TURN_RIGHT, 120, 2500, 50,
        route=[(0.37 * i - 3, 2.9 * i - 12) for i in range(32)],
        side_roads=[[(-20 + 5.3 * i + 3.1 * v, 7.7 * i - 2.2 * v) for v in range(4)] for i in range(8)],
    )


def test_golden_message_matches_the_firmware():
    assert encode(golden_nav(), 0xAB) == GOLDEN_MESSAGE


def test_golden_frames_match_the_firmware():
    assert split_frames(GOLDEN_MESSAGE, 0xAB, 8) == GOLDEN_FRAMES


def test_round_trip_within_quantization():
    nav = largest_nav()
    msg = encode(nav, 7)
    assert len(msg) == MSG_MAX_SIZE
    out, seq = decode(msg)
    assert seq == 7
    assert out.maneuver == nav.maneuver
    for a, b in zip(nav.route + sum(nav.side_roads, []), out.route + sum(out.side_roads, [])):
        assert abs(a[0] - b[0]) <= 1 / 32 and abs(a[1] - b[1]) <= 1 / 32


def test_no_route_is_eleven_bytes():
    msg = encode(NavData(), 0)
    assert len(msg) == 11
    assert msg[3] == 0
    assert decode(msg)[0].route == []


def test_values_are_clamped_and_rounded_like_c():
    out, _ = decode(encode(NavData(Maneuver.STRAIGHT, 1e9, -5, math.nan), 0))
    assert (out.distance_to_turn_m, out.total_distance_m, out.speed_kmh) == (65535, 0, 0)
    out, _ = decode(encode(NavData(route=[(-0.03125, 0.03125), (5000, -5000)]), 0))
    # roundf: halves away from zero, symmetric for negatives.
    assert out.route[0] == (-1 / 16, 1 / 16)
    assert out.route[1] == (32767 / 16, -2048)


def test_counts_are_clamped_and_short_side_roads_skipped():
    nav = largest_nav()
    nav.route *= 2
    nav.side_roads = [[(0, 0)]] + nav.side_roads * 2
    out, _ = decode(encode(nav, 0))
    assert len(out.route) == 32
    assert len(out.side_roads) == 7  # the first 8 roads minus the 1-point one


def test_decode_rejects_malformed():
    with pytest.raises(ValueError):
        decode(GOLDEN_MESSAGE[:-1])
    with pytest.raises(ValueError):
        decode(GOLDEN_MESSAGE + b"\0")
    with pytest.raises(ValueError):
        decode(b"G" + GOLDEN_MESSAGE[1:])
    with pytest.raises(ValueError):
        encode(NavData(maneuver=7), 0)


@pytest.mark.parametrize("frame_size,count", [(MIN_FRAME, 18), (182, 2), (514, 1)])
def test_largest_message_frame_counts(frame_size, count):
    msg = encode(largest_nav(), 1)
    frames = split_frames(msg, 1, frame_size)
    assert len(frames) == count == frame_count(len(msg), frame_size)
    assert all(len(f) <= frame_size and f[0] == FRAME_TYPE for f in frames)
    assert [f[2] for f in frames] == list(range(count))
    assert b"".join(f[4:] for f in frames) == msg


def test_frames_that_cannot_work_are_refused():
    assert smallest_frame_size() == 13
    assert frame_count(MSG_MAX_SIZE, 13) == 31
    assert frame_count(MSG_MAX_SIZE, 12) == 0
    with pytest.raises(ValueError, match="cannot send"):
        split_frames(bytes(MSG_MAX_SIZE), 0, 12)
