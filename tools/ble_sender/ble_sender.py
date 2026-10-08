#!/usr/bin/env python3
"""Sends navigation data to the Butamap ESP32 over BLE, standing in for the
phone app while testing the firmware's real-data path (NAV_SOURCE_BLE).

Each update is one 'N' message (navproto.encode) split into frames of at
most --chunk-size bytes, each written to the Nordic UART RX characteristic
without response.

    python ble_sender.py --synthetic
    python ble_sender.py --latlon                 # built-in route around Monas
    python ble_sender.py --latlon my_route.json --noise 5 --chunk-size 20

While it runs: Enter pauses/resumes sending (the link stays up, so the
display's "no signal" state can be tested), q + Enter quits.
"""

from __future__ import annotations

import argparse
import asyncio
import random
import sys
import time
import warnings

from navproto import MIN_FRAME, MSG_MAX_SIZE, encode, frame_count, smallest_frame_size, split_frames

DEVICE_NAME = "GPS_Tracker_BLE"
RX_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
STATS_PERIOD_S = 5.0
MTU_WAIT_S = 3.0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--synthetic", action="store_true",
                      help="random route in local coordinates, like the firmware simulator")
    mode.add_argument("--latlon", nargs="?", const="", metavar="ROUTE_JSON",
                      help="ride a lat/lon route through the phone-side pipeline "
                           "(built-in sample route if no file is given)")
    p.add_argument("--name", default=DEVICE_NAME, help="BLE name to scan for (default %(default)s)")
    p.add_argument("--address", help="connect to this address instead of scanning by name")
    p.add_argument("--rate", type=float, default=5.0, help="messages per second (default %(default)s)")
    p.add_argument("--chunk-size", type=int,
                   help="bytes per BLE write, frame header included "
                        "(default: the link's max write-without-response size)")
    p.add_argument("--drop", type=float, default=0.0, metavar="P",
                   help="skip whole messages with probability P (the firmware sees sequence gaps)")
    p.add_argument("--drop-frames", type=float, default=0.0, metavar="P",
                   help="skip single frames with probability P (the firmware drops those messages)")
    p.add_argument("--speed", type=float, default=40.0, help="--latlon riding speed, km/h (default %(default)s)")
    p.add_argument("--noise", type=float, default=0.0, metavar="M",
                   help="--latlon GPS noise, standard deviation in metres")
    p.add_argument("--pause-cycle", metavar="RUN,PAUSE",
                   help="send for RUN seconds, then pause for PAUSE seconds, repeatedly")
    p.add_argument("--duration", type=float, help="stop after this many seconds")
    p.add_argument("--seed", type=int, help="random seed, for repeatable runs")
    p.add_argument("--dry-run", action="store_true", help="no BLE: build and count the frames only")
    p.add_argument("--scan-timeout", type=float, default=10.0)
    args = p.parse_args(argv)

    if args.rate <= 0:
        p.error("--rate must be positive")
    for name in ("drop", "drop_frames"):
        if not 0.0 <= getattr(args, name) <= 1.0:
            p.error(f"--{name.replace('_', '-')} must be between 0 and 1")
    if args.pause_cycle:
        try:
            run_s, pause_s = (float(v) for v in args.pause_cycle.split(","))
        except ValueError:
            p.error("--pause-cycle expects RUN,PAUSE in seconds, e.g. 10,5")
        if run_s <= 0 or pause_s <= 0:
            p.error("--pause-cycle times must be positive")
        args.pause_cycle = (run_s, pause_s)
    return args


def check_chunk_size(chunk_size: int, max_write: int | None) -> None:
    """Exits with a clear message if frames of chunk_size bytes cannot work."""
    if max_write is not None and chunk_size > max_write:
        sys.exit(f"error: --chunk-size {chunk_size} is larger than this link's maximum "
                 f"write without response ({max_write} B)")
    if frame_count(MSG_MAX_SIZE, chunk_size) == 0:
        sys.exit(f"error: --chunk-size {chunk_size} is too small: the largest message "
                 f"({MSG_MAX_SIZE} B) would need more frames than allowed; use at least "
                 f"{smallest_frame_size()} (BLE guarantees {MIN_FRAME})")


def make_source(args: argparse.Namespace, rng: random.Random):
    """Returns step(dt) -> NavData."""
    if args.synthetic:
        from synthetic import SyntheticRide

        return SyntheticRide(rng).step

    from gps_sim import SAMPLE_ROUTE, FakeGps, load_route
    from navgeo import Navigator, Route

    route = Route(load_route(args.latlon) if args.latlon else SAMPLE_ROUTE)
    print(f"route: {len(route.latlon)} points, {route.length:.0f} m, "
          f"turns: {', '.join(f'{t.maneuver.name} at {t.s:.0f} m' for t in route.turns) or 'none'}")
    gps = FakeGps(route, args.speed, args.noise, rng)
    navigator = Navigator(route)

    def step(dt: float):
        lat, lon = gps.step(dt)
        return navigator.update(lat, lon, args.speed)

    return step


class Pauser:
    """Manual pause (Enter) and the optional --pause-cycle."""

    def __init__(self, cycle: tuple[float, float] | None):
        self.manual = False
        self.quit = False
        self.cycle = cycle
        self.start = time.monotonic()

    def paused(self) -> bool:
        if self.manual:
            return True
        if self.cycle:
            run_s, pause_s = self.cycle
            return (time.monotonic() - self.start) % (run_s + pause_s) >= run_s
        return False

    def on_stdin(self) -> None:
        line = sys.stdin.readline()
        if line == "":  # EOF: stop listening
            asyncio.get_running_loop().remove_reader(sys.stdin)
        elif line.strip().lower() == "q":
            self.quit = True
        else:
            self.manual = not self.manual
            print("paused (link kept up)" if self.manual else "resumed", flush=True)


async def connect(args: argparse.Namespace):
    from bleak import BleakClient, BleakScanner

    if args.address:
        target = args.address
    else:
        print(f"scanning for {args.name!r}...", flush=True)
        target = await BleakScanner.find_device_by_name(args.name, timeout=args.scan_timeout)
        if target is None:
            sys.exit(f"error: no device named {args.name!r} found")
    client = BleakClient(target, disconnected_callback=lambda _: print("disconnected", flush=True))
    await client.connect()
    char = client.services.get_characteristic(RX_CHAR_UUID)
    if char is None:
        await client.disconnect()
        sys.exit(f"error: the device has no RX characteristic {RX_CHAR_UUID}")

    # The MTU exchange may still be running; bleak reports 20 until it is done.
    deadline = time.monotonic() + MTU_WAIT_S
    while char.max_write_without_response_size <= MIN_FRAME and time.monotonic() < deadline:
        await asyncio.sleep(0.2)
    max_write = char.max_write_without_response_size
    if max_write <= MIN_FRAME:
        # BlueZ quirk: on a reconnect (services from its cache) the property
        # stays at 20 although the link negotiated a larger MTU. Ask BlueZ
        # for the link's MTU instead (bleak's documented workaround).
        acquire = getattr(client._backend, "_acquire_mtu", None)
        if acquire is not None:
            try:
                await acquire()
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore")
                    max_write = max(max_write, client.mtu_size - 3)
            except Exception as e:  # keep the safe 20 B
                print(f"could not read the link MTU ({e}); assuming {max_write} B writes", flush=True)
    print(f"connected to {client.address}, max write without response {max_write} B", flush=True)
    return client, char, max_write


async def run(args: argparse.Namespace) -> None:
    rng = random.Random(args.seed)
    step = make_source(args, rng)

    client = char = None
    if args.dry_run:
        max_write = None
        chunk_size = args.chunk_size or 514
    else:
        client, char, max_write = await connect(args)
        chunk_size = args.chunk_size or max_write
    check_chunk_size(chunk_size, max_write)
    print(f"sending at {args.rate:g} Hz in frames of up to {chunk_size} B; "
          "Enter = pause/resume, q + Enter = quit", flush=True)

    pauser = Pauser(args.pause_cycle)
    loop = asyncio.get_running_loop()
    if sys.stdin.isatty():
        loop.add_reader(sys.stdin, pauser.on_stdin)

    period = 1.0 / args.rate
    seq = 0
    sent_msgs = sent_frames = sent_bytes = skipped_msgs = skipped_frames = 0
    max_msg = max_frames = 0
    was_paused = False
    started = last_stats = time.monotonic()
    next_tick = loop.time()
    try:
        while not pauser.quit:
            now = time.monotonic()
            if args.duration and now - started >= args.duration:
                break
            if client is not None and not client.is_connected:
                sys.exit("error: link lost")

            nav = step(period)  # the ride goes on while paused
            paused = pauser.paused()
            if paused != was_paused and args.pause_cycle:
                print("paused (link kept up)" if paused else "resumed", flush=True)
            was_paused = paused

            if not paused:
                msg = encode(nav, seq)
                frames = split_frames(msg, seq, chunk_size)
                max_msg, max_frames = max(max_msg, len(msg)), max(max_frames, len(frames))
                if rng.random() < args.drop:
                    skipped_msgs += 1
                else:
                    for frame in frames:
                        if rng.random() < args.drop_frames:
                            skipped_frames += 1
                            continue
                        if client is not None:
                            await client.write_gatt_char(char, frame, response=False)
                        sent_frames += 1
                        sent_bytes += len(frame)
                    sent_msgs += 1
                seq = (seq + 1) & 0xFF

            if now - last_stats >= STATS_PERIOD_S:
                print(f"sent {sent_msgs} msgs, {sent_frames} frames, {sent_bytes} B "
                      f"(largest msg {max_msg} B = {max_frames} frames); "
                      f"skipped {skipped_msgs} msgs, {skipped_frames} frames"
                      f"{' [paused]' if paused else ''}", flush=True)
                last_stats = now

            next_tick += period
            await asyncio.sleep(max(0.0, next_tick - loop.time()))
    finally:
        if sys.stdin.isatty():
            loop.remove_reader(sys.stdin)
        print(f"total: sent {sent_msgs} msgs, {sent_frames} frames; "
              f"skipped {skipped_msgs} msgs, {skipped_frames} frames", flush=True)
        if client is not None and client.is_connected:
            await client.disconnect()


def main() -> None:
    try:
        asyncio.run(run(parse_args()))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
