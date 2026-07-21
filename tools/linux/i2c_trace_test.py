#!/usr/bin/env python3
from __future__ import annotations

import argparse
import glob
import os
import re
import select
import subprocess
import sys
import termios
import threading
import time
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
HOST_SRC = REPO_ROOT / "host" / "python" / "src"

if str(HOST_SRC) not in sys.path:
    sys.path.insert(0, str(HOST_SRC))

from picotrace.trace import I2cEventType, TraceChannelRegistry, TraceType, decode_i2c_events, iter_trace_packets


@dataclass(frozen=True)
class TracePacketSummary:
    sequence: int
    timestamp_us: int
    text: str
    start_count: int
    stop_count: int
    ack_count: int
    nack_count: int
    data_count: int


@dataclass(frozen=True)
class CaptureSummary:
    packets: tuple[TracePacketSummary, ...]
    status_line: str
    status_fields: dict[str, int]

    @property
    def transaction_count(self) -> int:
        return sum(packet.stop_count for packet in self.packets)

    @property
    def start_count(self) -> int:
        return sum(packet.start_count for packet in self.packets)

    @property
    def stop_count(self) -> int:
        return sum(packet.stop_count for packet in self.packets)

    @property
    def ack_count(self) -> int:
        return sum(packet.ack_count for packet in self.packets)

    @property
    def nack_count(self) -> int:
        return sum(packet.nack_count for packet in self.packets)

    @property
    def data_count(self) -> int:
        return sum(packet.data_count for packet in self.packets)


class CdcShell:
    def __init__(self, port: str) -> None:
        self._port = port
        self._fd: int | None = None

    def __enter__(self) -> CdcShell:
        self._fd = os.open(self._port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self._fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[3] = 0
        attrs[2] = attrs[2] | termios.CREAD | termios.CLOCAL
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 1
        termios.tcsetattr(self._fd, termios.TCSANOW, attrs)
        termios.tcflush(self._fd, termios.TCIOFLUSH)
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None

    @property
    def fd(self) -> int:
        if self._fd is None:
            raise RuntimeError("CDC shell is not open")
        return self._fd

    def transact(self, command: str, *, settle_seconds: float = 0.4, read_window_seconds: float = 1.5) -> list[str]:
        os.write(self.fd, command.encode("utf-8") + b"\r\n")
        time.sleep(settle_seconds)

        output = bytearray()
        end = time.time() + read_window_seconds
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.2)
            if not ready:
                continue

            chunk = os.read(self.fd, 4096)
            if chunk:
                output.extend(chunk)

        return [line.strip() for line in output.decode("utf-8", "replace").splitlines() if line.strip()]


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run a repeatable PicoTrace I2C trace test from Linux.",
    )
    parser.add_argument("--channel", type=int, default=0, help="PicoTrace logical I2C channel to monitor")
    parser.add_argument("--bus", type=int, default=1, help="Linux I2C bus number to scan with i2cdetect")
    parser.add_argument(
        "--sample-hz",
        type=int,
        default=4_000_000,
        help="PicoTrace sampler rate passed to the CDC i2cmon command; older firmware commonly accepts 4000000, 8000000, or 12000000",
    )
    parser.add_argument(
        "--expect-transactions",
        type=int,
        default=112,
        help="expected traced transaction count; set to 0 to skip the count check",
    )
    parser.add_argument(
        "--settle-seconds",
        type=float,
        default=0.2,
        help="delay after the trace reader opens before generating I2C traffic",
    )
    parser.add_argument(
        "--drain-seconds",
        type=float,
        default=0.5,
        help="delay after traffic generation to let USB bulk draining finish",
    )
    parser.add_argument(
        "--no-generate-traffic",
        action="store_true",
        help="leave traffic generation to another tool instead of running i2cdetect -y <bus>",
    )
    parser.add_argument("--verbose", action="store_true", help="print each decoded I2C transaction")
    return parser


def find_cdc_port() -> str:
    matches = sorted(glob.glob("/dev/serial/by-id/usb-PicoTrace_*if00"))
    if not matches:
        raise RuntimeError("no PicoTrace CDC control port found under /dev/serial/by-id")
    return matches[0]


def format_timestamp_us(timestamp_us: int) -> str:
    total_seconds, micros = divmod(timestamp_us, 1_000_000)
    minutes, seconds = divmod(total_seconds, 60)
    hours, minutes = divmod(minutes, 60)
    return f"{hours:02}:{minutes:02}:{seconds:02}.{micros:06}"


def format_i2c_packet(sequence: int, timestamp_us: int, event_tokens: list[str]) -> str:
    return f"[{format_timestamp_us(timestamp_us)}] seq={sequence:>6} I2C: {' '.join(event_tokens)}"


def summarize_i2c_packet(packet) -> TracePacketSummary:
    tokens: list[str] = []
    start_count = 0
    stop_count = 0
    ack_count = 0
    nack_count = 0
    data_count = 0

    for event in decode_i2c_events(packet):
        event_type = event.event_type
        if event_type is I2cEventType.START:
            start_count += 1
            tokens.append("START")
        elif event_type is I2cEventType.DATA:
            data_count += 1
            tokens.append(f"DATA:{event.value:02X}")
        elif event_type is I2cEventType.ACK:
            if event.value == 0:
                ack_count += 1
                tokens.append("ACK")
            else:
                nack_count += 1
                tokens.append("NACK")
        elif event_type is I2cEventType.STOP:
            stop_count += 1
            tokens.append("STOP")
        else:
            tokens.append(f"{event_type.name}:{event.value:02X}")

    return TracePacketSummary(
        sequence=packet.header.sequence,
        timestamp_us=packet.header.timestamp_us,
        text=format_i2c_packet(packet.header.sequence, packet.header.timestamp_us, tokens),
        start_count=start_count,
        stop_count=stop_count,
        ack_count=ack_count,
        nack_count=nack_count,
        data_count=data_count,
    )


def parse_status_fields(status_line: str) -> dict[str, int]:
    fields: dict[str, int] = {}
    for key, value in re.findall(r"([a-z_]+)=([0-9]+)", status_line):
        fields[key] = int(value)
    return fields


def require_command_ok(command: str, lines: list[str]) -> str:
    if not lines:
        raise RuntimeError(f"no response for CDC command: {command}")

    joined = "\n".join(lines)
    if "Usage:" in joined or "invalid" in joined.lower() or "failed" in joined.lower() or "busy" in joined.lower():
        raise RuntimeError(f"CDC command failed for '{command}': {joined}")
    return lines[-1]


def configure_i2c_monitor(shell: CdcShell, channel: int, sample_hz: int) -> str:
    require_command_ok("stream on", shell.transact("stream on"))
    require_command_ok(f"i2cmon {channel} {sample_hz}", shell.transact(f"i2cmon {channel} {sample_hz}"))
    status_line = require_command_ok(f"i2cmon status {channel}", shell.transact(f"i2cmon status {channel}"))
    if "running" not in status_line:
        raise RuntimeError(f"I2C monitor did not enter running state: {status_line}")
    return status_line


def read_i2c_status(shell: CdcShell, channel: int) -> tuple[str, dict[str, int]]:
    status_line = require_command_ok(f"i2cmon status {channel}", shell.transact(f"i2cmon status {channel}"))
    return status_line, parse_status_fields(status_line)


def disable_stream_best_effort(shell: CdcShell) -> None:
    try:
        shell.transact("stream off", settle_seconds=0.2, read_window_seconds=0.5)
    except Exception:
        pass


def run_i2cdetect(bus: int) -> str:
    result = subprocess.run(
        ["i2cdetect", "-y", str(bus)],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def collect_trace(channel: int, settle_seconds: float, drain_seconds: float, generate_traffic: bool, bus: int) -> tuple[list[TracePacketSummary], str | None]:
    packets: list[TracePacketSummary] = []
    traffic_output: str | None = None
    stop_event = threading.Event()
    opened_event = threading.Event()
    registry = TraceChannelRegistry([channel])
    error_box: list[BaseException] = []

    def reader() -> None:
        try:
            for packet in iter_trace_packets(
                duration_seconds=None,
                channel_registry=registry,
                keep_running=lambda: not stop_event.is_set(),
                on_opened=opened_event.set,
            ):
                if packet.header.trace_type is not TraceType.I2C:
                    continue
                packets.append(summarize_i2c_packet(packet))
        except BaseException as exc:
            error_box.append(exc)

    reader_thread = threading.Thread(target=reader, daemon=True)
    reader_thread.start()

    try:
        if not opened_event.wait(5.0):
            raise RuntimeError("trace reader did not open the USB bulk stream in time")
        time.sleep(settle_seconds)
        if generate_traffic:
            traffic_output = run_i2cdetect(bus)
        time.sleep(drain_seconds)
    finally:
        stop_event.set()
        reader_thread.join(timeout=5.0)

    if error_box:
        raise RuntimeError(str(error_box[0])) from error_box[0]

    return packets, traffic_output


def main() -> int:
    parser = build_argument_parser()
    args = parser.parse_args()

    if args.sample_hz <= 0:
        parser.error("--sample-hz must be positive")
    if args.expect_transactions < 0:
        parser.error("--expect-transactions must be zero or positive")

    cdc_port = find_cdc_port()
    print(f"cdc_port={cdc_port}")

    with CdcShell(cdc_port) as shell:
        configure_i2c_monitor(shell, args.channel, args.sample_hz)
        try:
            packets, traffic_output = collect_trace(
                args.channel,
                args.settle_seconds,
                args.drain_seconds,
                not args.no_generate_traffic,
                args.bus,
            )
            status_line, status_fields = read_i2c_status(shell, args.channel)
        finally:
            disable_stream_best_effort(shell)

    summary = CaptureSummary(
        packets=tuple(packets),
        status_line=status_line,
        status_fields=status_fields,
    )

    if traffic_output:
        print(traffic_output)

    print(
        "summary "
        f"packets={len(summary.packets)} transactions={summary.transaction_count} "
        f"starts={summary.start_count} stops={summary.stop_count} "
        f"data={summary.data_count} ack={summary.ack_count} nack={summary.nack_count}"
    )
    print(f"device_status {summary.status_line}")

    if args.verbose:
        for packet in summary.packets:
            print(packet.text)

    ok = True
    if args.expect_transactions > 0 and summary.transaction_count != args.expect_transactions:
        print(
            f"error expected_transactions={args.expect_transactions} actual_transactions={summary.transaction_count}",
            file=sys.stderr,
        )
        ok = False

    if summary.start_count != summary.stop_count:
        print(
            f"error unbalanced_transactions starts={summary.start_count} stops={summary.stop_count}",
            file=sys.stderr,
        )
        ok = False

    if summary.status_fields.get("overruns", 0) != 0 or summary.status_fields.get("sticky", 0) != 0:
        print(
            f"error monitor_overrun overruns={summary.status_fields.get('overruns', 0)} sticky={summary.status_fields.get('sticky', 0)}",
            file=sys.stderr,
        )
        ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())