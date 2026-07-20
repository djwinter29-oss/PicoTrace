#!/usr/bin/env python3
"""Run a repeatable PicoTrace I2C trace validation against Linux I2C traffic.

This script configures one PicoTrace I2C monitor channel, opens the USB bulk
trace stream, runs one host-side traffic command such as ``i2cdetect -y 1``,
and then verifies the observed I2C address-probe count.
"""

from __future__ import annotations

import argparse
from collections import Counter
import shlex
import subprocess
import sys
import time
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
HOST_PYTHON_SRC = REPO_ROOT / "host" / "python" / "src"
if str(HOST_PYTHON_SRC) not in sys.path:
    sys.path.insert(0, str(HOST_PYTHON_SRC))

from picotrace.control.hid import HidControlClient  # noqa: E402
from picotrace.trace import TraceChannelRegistry, decode_i2c_events, iter_trace_packets  # noqa: E402
from picotrace.trace.decode import I2cEventType  # noqa: E402


DEFAULT_TRAFFIC_COMMAND = "i2cdetect -y 1"
VALID_SAMPLE_RATES = (4_000_000, 8_000_000, 12_000_000)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run a PicoTrace I2C trace validation against host-generated traffic.")
    parser.add_argument("--channel", type=int, default=0, help="logical PicoTrace I2C trace channel to capture")
    parser.add_argument(
        "--sample-hz",
        type=int,
        choices=VALID_SAMPLE_RATES,
        default=4_000_000,
        help="validated PicoTrace I2C monitor preset in Hz",
    )
    parser.add_argument(
        "--traffic-command",
        default=DEFAULT_TRAFFIC_COMMAND,
        help="host command that generates I2C traffic, quoted as one shell-style string",
    )
    parser.add_argument(
        "--expected-address-queries",
        type=int,
        default=112,
        help="expected count of decoded address-query bytes, or 0 to skip the count check",
    )
    parser.add_argument(
        "--duration-seconds",
        type=float,
        default=6.0,
        help="maximum bulk-capture duration in seconds",
    )
    parser.add_argument(
        "--settle-seconds",
        type=float,
        default=0.2,
        help="delay after opening the bulk stream before launching the traffic command",
    )
    return parser.parse_args()


def _configure_capture(channel: int, sample_hz: int) -> None:
    with HidControlClient.open() as control:
        control.set_stream_enabled(False)
        control.i2c_set_rate(channel, 0)
        control.set_stream_enabled(True)
        control.i2c_set_rate(channel, sample_hz)


def _teardown_capture(channel: int) -> None:
    with HidControlClient.open() as control:
        control.set_stream_enabled(False)
        try:
            control.i2c_set_rate(channel, 0)
        except Exception:
            pass


def main() -> int:
    args = _parse_args()
    traffic_command = shlex.split(args.traffic_command)
    registry = TraceChannelRegistry([args.channel])
    probe_box: list[subprocess.CompletedProcess[str] | None] = [None]
    counter: Counter[str] = Counter()
    address_bytes: list[int] = []
    awaiting_address = False
    packet_count = 0

    print(f"channel={args.channel} sample_hz={args.sample_hz} traffic={' '.join(traffic_command)}")

    _configure_capture(args.channel, args.sample_hz)

    def on_opened() -> None:
        time.sleep(args.settle_seconds)
        probe_box[0] = subprocess.run(traffic_command, capture_output=True, text=True, check=False)

    try:
        for packet in iter_trace_packets(
            duration_seconds=args.duration_seconds,
            channel_registry=registry,
            on_opened=on_opened,
        ):
            packet_count += 1
            for event in decode_i2c_events(packet):
                event_name = event.event_type.name
                counter[event_name] += 1

                if event.event_type is I2cEventType.START:
                    awaiting_address = True
                    continue

                if awaiting_address and event.event_type is I2cEventType.DATA:
                    address_bytes.append(event.value)
                    awaiting_address = False
                    continue

                if event.event_type in {
                    I2cEventType.STOP,
                    I2cEventType.ERROR,
                    I2cEventType.OVERFLOW,
                    I2cEventType.CONTROL_RECONFIG,
                    I2cEventType.CONTROL_STOP,
                }:
                    awaiting_address = False
    finally:
        _teardown_capture(args.channel)

    probe = probe_box[0]
    if probe is None:
        print("traffic command did not run", file=sys.stderr)
        return 1

    query_count = len(address_bytes)
    write_queries = sum(1 for value in address_bytes if (value & 1) == 0)
    read_queries = query_count - write_queries
    unique_addresses = len({value >> 1 for value in address_bytes})
    overflow_count = counter.get("OVERFLOW", 0)

    print(f"traffic_exit={probe.returncode}")
    print(f"packet_count={packet_count}")
    print(f"event_counts={dict(counter)}")
    print(f"address_query_count={query_count}")
    print(f"write_queries={write_queries}")
    print(f"read_queries={read_queries}")
    print(f"unique_7bit_addresses={unique_addresses}")
    print("first_16_address_bytes=" + " ".join(f"{value:02X}" for value in address_bytes[:16]))
    if probe.stdout:
        print("traffic_stdout:")
        print(probe.stdout.rstrip())
    if probe.stderr:
        print("traffic_stderr:", file=sys.stderr)
        print(probe.stderr.rstrip(), file=sys.stderr)

    if probe.returncode != 0:
        return probe.returncode
    if overflow_count != 0:
        print(f"FAIL: observed {overflow_count} overflow boundary events", file=sys.stderr)
        return 1
    if (args.expected_address_queries > 0) and (query_count != args.expected_address_queries):
        print(
            f"FAIL: expected {args.expected_address_queries} address queries but decoded {query_count}",
            file=sys.stderr,
        )
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())