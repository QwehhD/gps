#!/usr/bin/env python3
"""Sends navigation data to the Butamap ESP32 over BLE, standing in for the
phone app while testing the firmware's real-data path (NAV_SOURCE_BLE).

Each update is one 'N' message (navproto.encode) split into frames of at
most --chunk-size bytes, each written to the Nordic UART RX characteristic
without response.

    python ble_sender.py --synthetic
    python ble_sender.py --latlon                 # built-in route around Monas
    python ble_sender.py --latlon routes/kampus.geojson --noise 5 --chunk-size 20

--latlon reads GeoJSON (a LineString, bare or in a Feature or
FeatureCollection, as fetch_route.py saves it or geojson.io draws it) or
{"points": [[lat, lon], ...]}, and prints the route's turns before riding.
The maneuvers come from the OpenRouteService steps when the file has them,
else from the shape of the line (--maneuvers); with steps, both are listed
side by side. While riding it prints, once a second, where the rider is and
the maneuver the display should show.

While it runs: Enter pauses/resumes sending (the link stays up, so the
display's "no signal" state can be tested), a number + Enter sets the
--latlon speed in km/h (e.g. 10 to slow down for a turn), q + Enter quits.
"""

from __future__ import annotations

import argparse
import asyncio
import random
import sys
import time
import warnings

from navproto import MIN_FRAME, MSG_MAX_SIZE, Maneuver, encode, frame_count, smallest_frame_size, split_frames

DEVICE_NAME = "GPS_Tracker_BLE"
RX_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
STATS_PERIOD_S = 5.0
STATUS_PERIOD_S = 1.0
MTU_WAIT_S = 3.0
MAX_SPEED_KMH = 255.0  # the message's speed field is one byte

DIRECTION = {Maneuver.STRAIGHT: "straight", Maneuver.TURN_LEFT: "left", Maneuver.TURN_RIGHT: "right",
             Maneuver.U_TURN: "u-turn"}


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--synthetic", action="store_true",
                      help="random route in local coordinates, like the firmware simulator")
    mode.add_argument("--latlon", nargs="?", const="", metavar="ROUTE_FILE",
                      help="ride a lat/lon route (GeoJSON or points JSON) through the phone-side "
                           "pipeline (built-in sample route if no file is given)")
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
    p.add_argument("--maneuvers", choices=("ors", "geometry"),
                   help="--latlon maneuver source: the OpenRouteService steps in the file, or the "
                        "turns in the line's shape (default: ors when the file has steps)")
    p.add_argument("--speed-kmh", "--speed", dest="speed", type=float, default=40.0, metavar="KMH",
                   help="--latlon riding speed (default %(default)s); type a number + Enter while "
                        "running to change it")
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
    if not 0.0 <= args.speed <= MAX_SPEED_KMH:
        p.error(f"--speed-kmh must be between 0 and {MAX_SPEED_KMH:g}")
    if args.maneuvers and args.latlon is None:
        p.error("--maneuvers only applies to --latlon")
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


def route_summary(route, source: str) -> str:
    """What --latlon prints before riding: the turns the route geometry
    gives, to check against what the display shows."""
    from navgeo import TURN_CHORD_M, TURN_MIN_DEG, UTURN_MIN_DEG, UTURN_SPAN_M

    lines = [f"route: {source}", f"  {len(route.latlon)} points, {route.length:.0f} m"]
    if not route.turns:
        lines.append(f"  no turns sharper than {TURN_MIN_DEG:g}° in the line")
        return "\n".join(lines)
    count = len(route.turns)
    lines.append(f"  {count} turn{'s' if count > 1 else ''} in the line (direction change over "
                 f"{TURN_CHORD_M:g} m either side, sharper than {TURN_MIN_DEG:g}°; "
                 f"u-turn: {UTURN_MIN_DEG:g}° or more within {UTURN_SPAN_M:g} m):")
    lines.append("      #  direction  angle  from start  lat,lon")
    for n, turn in enumerate(route.turns, 1):
        lat, lon = route.latlon_at(turn.s)
        lines.append(f"  {n:5d}  {DIRECTION[turn.maneuver]:<9}  {abs(turn.angle_deg):4.0f}°  "
                     f"{turn.s:8.0f} m  {lat:.6f},{lon:.6f}")
    return "\n".join(lines)


def _clip(text: str, width: int) -> str:
    return text if len(text) <= width else text[: width - 1] + "…"


def steps_table(pairs) -> str:
    """The OpenRouteService steps next to the turns found in the line
    (maneuvers.pair_up), and every place where the two disagree."""
    from maneuvers import PAIR_M

    count = sum(1 for p in pairs if p.step is not None)
    lines = [f"  {count} ORS steps next to the turns in the line (same place: within {PAIR_M:g} m):",
             "      #  from start  ORS step                        display   line                  check"]
    differences = []
    n = 0
    for p in pairs:
        if p.step is not None:
            n += 1
            step = p.step
            what = step.name + (f", exit {step.exit_number}" if step.exit_number else "")
            what += f", {step.street}" if step.street else ""
            shown = DIRECTION[step.maneuver] if step.maneuver is not None else "-"
            head = f"  {n:5d}  {step.s:8.0f} m  {_clip(what, 30):<30}  {shown:<8}"
        else:
            head = f"  {'':5}  {p.turn.s:8.0f} m  {'(no step)':<30}  {'':<8}"
        line = f"{DIRECTION[p.turn.maneuver]} {abs(p.turn.angle_deg):.0f}° at {p.turn.s:.0f} m" if p.turn else ""
        lines.append(f"{head}  {line:<20}  {p.check}".rstrip())
        if p.check == "different":
            differences.append(f"at {p.s:.0f} m ORS says {p.step.name} (shown as {shown}), the line makes "
                               f"a {line}")
        elif p.check == "ORS only":
            differences.append(f"at {p.s:.0f} m ORS says {p.step.name} (shown as {shown}), the line turns "
                               f"only {abs(p.step.angle_deg):.0f}° there")
        elif p.check == "geometry only":
            differences.append(f"at {p.s:.0f} m the line makes a {line}, ORS has no step there")
    if differences:
        lines.append(f"  {len(differences)} difference{'s' if len(differences) > 1 else ''}:")
        lines += [f"    - {d}" for d in differences]
    else:
        lines.append("  no differences")
    return "\n".join(lines)


class LatLonRide:
    """The --latlon source: a pretend GPS riding the route at speed_kmh, fed
    through the phone-side pipeline (navgeo.Navigator)."""

    def __init__(self, route, turns, speed_kmh: float, noise_m: float, rng: random.Random):
        from gps_sim import FakeGps
        from navgeo import Navigator

        self.gps = FakeGps(route, speed_kmh, noise_m, rng)
        self.navigator = Navigator(route, turns)

    @property
    def speed_kmh(self) -> float:
        return self.gps.speed_kmh

    @speed_kmh.setter
    def speed_kmh(self, value: float) -> None:
        self.gps.speed_kmh = value

    def step(self, dt: float):
        lat, lon = self.gps.step(dt)
        return self.navigator.update(lat, lon, self.gps.speed_kmh)

    def status(self, nav) -> str:
        """Where the rider is and what the display should show, for checking
        the screen against the terminal."""
        if nav.maneuver == Maneuver.STRAIGHT:
            ahead = f"no turn ahead, end of route in {nav.distance_to_turn_m:.0f} m"
        else:
            ahead = f"next: {DIRECTION[nav.maneuver]} in {nav.distance_to_turn_m:.0f} m"
        return (f"at {self.navigator.s:5.0f} m | {ahead} | {nav.total_distance_m:.0f} m to go | "
                f"{self.gps.speed_kmh:g} km/h")


def make_source(args: argparse.Namespace, rng: random.Random):
    """The source of NavData: an object with step(dt) -> NavData. The
    --latlon one also has status(nav) and a settable speed_kmh."""
    if args.synthetic:
        from synthetic import SyntheticRide

        return SyntheticRide(rng)

    from gps_sim import SAMPLE_ROUTE, read_route_file
    from maneuvers import ors_steps, ors_turns, pair_up, read_ors_steps
    from navgeo import Route

    raw_steps = None
    if args.latlon:
        try:
            points, kind = read_route_file(args.latlon)
            raw_steps = read_ors_steps(args.latlon)
        except (OSError, ValueError) as e:
            sys.exit(f"error: {e}")
        route, source = Route(points), f"{args.latlon} ({kind})"
    else:
        route, source = Route(SAMPLE_ROUTE), "built-in sample route around Monas"
    try:
        steps = ors_steps(route, raw_steps) if raw_steps else None
    except ValueError as e:
        sys.exit(f"error: {args.latlon}: {e}")
    use = args.maneuvers or ("ors" if steps else "geometry")
    if use == "ors" and not steps:
        sys.exit(f"error: {args.latlon or 'the built-in route'} has no OpenRouteService steps "
                 "(properties.segments[].steps); use --maneuvers geometry")

    print(route_summary(route, source), flush=True)
    if steps:
        print(steps_table(pair_up(steps, route.turns)), flush=True)
    if use == "ors":
        print("  the display gets the ORS maneuvers (--maneuvers geometry: the turns in the line)", flush=True)
    else:
        print("  the display gets the turns in the line"
              + (" (--maneuvers ors: the ORS steps)" if steps else ""), flush=True)
    return LatLonRide(route, ors_turns(steps) if use == "ors" else route.turns, args.speed, args.noise, rng)


class Controls:
    """The keyboard (Enter: pause/resume, a number + Enter: --latlon speed,
    q + Enter: quit) and the optional --pause-cycle."""

    def __init__(self, cycle: tuple[float, float] | None):
        self.manual = False
        self.quit = False
        self.speed_kmh: float | None = None  # a new speed typed in, not yet applied
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
        text = line.strip().lower()
        if line == "":  # EOF: stop listening
            asyncio.get_running_loop().remove_reader(sys.stdin)
        elif text == "q":
            self.quit = True
        elif not text:
            self.manual = not self.manual  # run() reports the change
        else:
            try:
                speed = float(text.removesuffix("km/h").removesuffix("kmh"))
            except ValueError:
                speed = -1.0
            if 0.0 <= speed <= MAX_SPEED_KMH:
                self.speed_kmh = speed
            else:
                print(f"keys: Enter = pause/resume, a speed in km/h (0-{MAX_SPEED_KMH:g}) + Enter, "
                      "q + Enter = quit", flush=True)


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
    source = make_source(args, rng)
    status = getattr(source, "status", None)

    client = char = None
    if args.dry_run:
        max_write = None
        chunk_size = args.chunk_size or 514
    else:
        client, char, max_write = await connect(args)
        chunk_size = args.chunk_size or max_write
    check_chunk_size(chunk_size, max_write)
    print(f"sending at {args.rate:g} Hz in frames of up to {chunk_size} B; Enter = pause/resume, "
          f"{'a number + Enter = speed in km/h, ' if status else ''}q + Enter = quit", flush=True)

    controls = Controls(args.pause_cycle)
    loop = asyncio.get_running_loop()
    if sys.stdin.isatty():
        loop.add_reader(sys.stdin, controls.on_stdin)

    period = 1.0 / args.rate
    seq = 0
    sent_msgs = sent_frames = sent_bytes = skipped_msgs = skipped_frames = 0
    max_msg = max_frames = 0
    was_paused = False
    started = last_stats = time.monotonic()
    last_status = started - STATUS_PERIOD_S  # the first status line comes right away
    last_sent = None  # when the last message went out, for the pause report
    next_tick = loop.time()
    try:
        while not controls.quit:
            now = time.monotonic()
            if args.duration and now - started >= args.duration:
                break
            if client is not None and not client.is_connected:
                sys.exit("error: link lost")

            if controls.speed_kmh is not None:
                if status:
                    source.speed_kmh = controls.speed_kmh
                    print(f"{now - started:6.1f} s | speed {controls.speed_kmh:g} km/h", flush=True)
                else:
                    print("the speed only applies to --latlon", flush=True)
                controls.speed_kmh = None

            nav = source.step(period)  # the ride goes on while paused
            paused = controls.paused()
            if paused != was_paused:
                if paused:
                    since = f", last message {now - last_sent:.2f} s ago" if last_sent is not None else ""
                    print(f"{now - started:6.1f} s | paused (link kept up){since}", flush=True)
                else:
                    print(f"{now - started:6.1f} s | resumed", flush=True)
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
                    last_sent = time.monotonic()
                seq = (seq + 1) & 0xFF

            if status and now - last_status >= STATUS_PERIOD_S:
                print(f"{now - started:6.1f} s | {status(nav)}{' [paused: not sent]' if paused else ''}",
                      flush=True)
                last_status = now

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
