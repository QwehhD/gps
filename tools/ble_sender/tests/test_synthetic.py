import math
import random

from navproto import MSG_MAX_SIZE, decode, encode
from synthetic import SyntheticRide


def test_synthetic_ride_produces_valid_messages():
    ride = SyntheticRide(random.Random(3))
    sizes = []
    for i in range(5000):  # 1000 s at 5 Hz
        nav = ride.step(0.2)
        assert 2 <= len(nav.route) <= 32
        assert len(nav.side_roads) <= 8
        assert all(len(road) == 4 for road in nav.side_roads)
        assert nav.distance_to_turn_m >= 0
        # The rider sits on the route line.
        assert min(math.hypot(x, y) for x, y in nav.route) < 30
        msg = encode(nav, i)
        sizes.append(len(msg))
        out, seq = decode(msg)
        assert seq == i & 0xFF and len(out.route) == len(nav.route)
    assert max(sizes) <= MSG_MAX_SIZE
    assert 120 < sum(sizes) / len(sizes) < 230
