#!/usr/bin/env python3
from __future__ import annotations

import argparse
import importlib.util
import re
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
HOST_SRC = REPO_ROOT / "host" / "python" / "src"

if str(HOST_SRC) not in sys.path:
    sys.path.insert(0, str(HOST_SRC))

from picotrace.control import HidControlClient
from picotrace.trace import (
    SpiCaptureMode,
    TraceChannelRegistry,
    TraceType,
    decode_spi_samples,
    iter_trace_packets,
)


SPI_TRANSFER_SPEC = importlib.util.spec_from_file_location(
    "picotrace_spi_transfer",
    REPO_ROOT / "tools" / "linux" / "spi_transfer.py",
)
if SPI_TRANSFER_SPEC is None or SPI_TRANSFER_SPEC.loader is None:
    raise RuntimeError("failed to load tools/linux/spi_transfer.py")
SPI_TRANSFER_MODULE = importlib.util.module_from_spec(SPI_TRANSFER_SPEC)
SPI_TRANSFER_SPEC.loader.exec_module(SPI_TRANSFER_MODULE)
transfer_once = SPI_TRANSFER_MODULE.transfer_once

DEFAULT_MARKER = bytes([
    0xF3, 0x19, 0xA7, 0x4C, 0xD2, 0x68, 0x0E, 0xB5,
    0x5A, 0xC1, 0x37, 0x8D, 0xE4, 0x2B, 0x91, 0x76,
    0xCC, 0x03, 0xB8, 0x5F, 0x14, 0xEA, 0x61, 0x97,
    0x2D, 0xC4, 0x7B, 0x10, 0x86, 0xFD, 0x52, 0x39,
    0xA0, 0x17, 0xED, 0x64, 0x9B, 0x21, 0xC8, 0x7F,
    0x06, 0xBC, 0x43, 0xD9, 0x70, 0x15, 0xAB, 0x32,
    0xE8, 0x4F, 0x94, 0x29, 0xCF, 0x56, 0x0B, 0xB1,
    0x68, 0xDE, 0x25, 0x9A, 0x40, 0xF7, 0x1C, 0x83,
])


@dataclass(frozen=True)
class BenchmarkConfig:
    device: str
    capture_mode: SpiCaptureMode
    speed_hz: int
    spi_mode: int
    bits_per_word: int
    delay_usecs: int
    bus: int
    channel: int
    channel_select_mask: int
    timeout_us: int
    chunk_bytes: int
    repeat_count: int
    warmup_bytes: int
    settle_seconds: float
    drain_seconds: float
    trials: int

    @property
    def total_bytes(self) -> int:
        return self.chunk_bytes * self.repeat_count


@dataclass(frozen=True)
class TrialResult:
    ok: bool
    mismatch: int | str | None
    mosi_bytes: int
    miso_bytes: int | None
    packets_emitted: int
    transactions_emitted: int
    overrun_count: int
    sink_overrun_count: int
    sampler_overrun_count: int
    ring_drop_count: int
    usb_host_backpressure_stall_count: int
    dma_words_consumed: int
    fragment_push_attempt_count: int
    peak_ring_depth_packets: int
    timeout_close_count: int
    throughput_mbps: float
    window_seconds: float


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run repeatable PicoTrace SPI throughput benchmarks from Linux.",
    )
    parser.add_argument(
        "--capture",
        choices=("mosi", "mosi-miso"),
        default="mosi",
        help="capture mode to benchmark",
    )
    parser.add_argument(
        "--speed-hz",
        nargs="+",
        type=int,
        required=True,
        help="one or more SPI clock rates in Hz to test",
    )
    parser.add_argument("--device", default="/dev/spidev0.0", help="Linux SPI device path")
    parser.add_argument(
        "--firmware-build-dir",
        default=None,
        help="firmware build directory to inspect for the configured system clock",
    )
    parser.add_argument("--spi-mode", type=int, choices=range(4), default=0, help="SPI mode 0-3")
    parser.add_argument("--bits-per-word", type=int, default=8, help="SPI bits per word")
    parser.add_argument("--delay-usecs", type=int, default=0, help="SPI inter-transfer delay in microseconds")
    parser.add_argument("--bus", type=int, default=0, help="PicoTrace SPI bus index")
    parser.add_argument("--board", choices=("pico", "pico2"), default="pico", help="target PicoTrace board family for reporting the configured firmware clock")
    parser.add_argument("--channel", type=int, default=0, help="logical trace channel to read back")
    parser.add_argument(
        "--channel-select-mask",
        type=lambda value: int(value, 0),
        default=0x01,
        help="PicoTrace channel select mask, accepts decimal or 0x-prefixed values",
    )
    parser.add_argument("--timeout-us", type=int, default=20000, help="SPI idle timeout in microseconds")
    parser.add_argument("--chunk-bytes", type=int, default=3968, help="bytes per SPI transfer chunk")
    parser.add_argument("--repeat-count", type=int, default=240, help="number of transfer chunks per trial")
    parser.add_argument("--warmup-bytes", type=int, default=256, help="warmup transfer byte count")
    parser.add_argument("--trials", type=int, default=3, help="trials per speed")
    parser.add_argument(
        "--settle-seconds",
        type=float,
        default=0.2,
        help="delay after reader open before warmup transfer",
    )
    parser.add_argument(
        "--drain-seconds",
        type=float,
        default=1.5,
        help="delay after the transfer window to let host draining finish",
    )
    return parser


def first_mismatch(expected: bytes, actual: bytes) -> int | None:
    limit = min(len(expected), len(actual))
    for index in range(limit):
        if expected[index] != actual[index]:
            return index
    if len(expected) != len(actual):
        return limit
    return None


def decode_capture_mode(name: str) -> SpiCaptureMode:
    if name == "mosi":
        return SpiCaptureMode.MOSI
    return SpiCaptureMode.MOSI_MISO


def collect_stream(config: BenchmarkConfig, marker: bytes, expected: bytes) -> TrialResult:
    with HidControlClient.open() as control:
        control.set_stream_enabled(True)
        control.spi_set_config(
            config.bus,
            capture=config.capture_mode,
            spi_mode=config.spi_mode,
            channel_select_mask=config.channel_select_mask,
            timeout_us=config.timeout_us,
        )

    captured_mosi = bytearray()
    captured_miso = bytearray()
    stop_event = threading.Event()
    opened_event = threading.Event()
    registry = TraceChannelRegistry([config.channel])
    error_box: list[BaseException] = []

    def reader() -> None:
        try:
            for packet in iter_trace_packets(
                duration_seconds=None,
                channel_registry=registry,
                keep_running=lambda: not stop_event.is_set(),
                on_opened=opened_event.set,
            ):
                if packet.header.trace_type is not TraceType.SPI:
                    continue
                samples = decode_spi_samples(packet)
                captured_mosi.extend(samples.mosi)
                if samples.miso is not None:
                    captured_miso.extend(samples.miso)
        except BaseException as exc:
            error_box.append(exc)

    reader_thread = threading.Thread(target=reader, daemon=True)
    reader_thread.start()

    try:
        if not opened_event.wait(5.0):
            raise RuntimeError("threaded reader did not open USB bulk stream in time")

        warmup = bytes([0x55]) * config.warmup_bytes
        data_chunk = expected[:config.chunk_bytes]

        time.sleep(config.settle_seconds)
        transfer_once(
            config.device,
            warmup,
            speed_hz=config.speed_hz,
            spi_mode=config.spi_mode,
            bits_per_word=config.bits_per_word,
            delay_usecs=config.delay_usecs,
        )
        time.sleep(0.05)
        transfer_once(
            config.device,
            marker,
            speed_hz=config.speed_hz,
            spi_mode=config.spi_mode,
            bits_per_word=config.bits_per_word,
            delay_usecs=config.delay_usecs,
        )

        tx_start = time.perf_counter()
        for _ in range(config.repeat_count):
            transfer_once(
                config.device,
                data_chunk,
                speed_hz=config.speed_hz,
                spi_mode=config.spi_mode,
                bits_per_word=config.bits_per_word,
                delay_usecs=config.delay_usecs,
            )
        tx_seconds = time.perf_counter() - tx_start

        time.sleep(config.drain_seconds)
    finally:
        stop_event.set()
        reader_thread.join(timeout=5.0)

    if error_box:
        raise RuntimeError(str(error_box[0])) from error_box[0]

    with HidControlClient.open() as control:
        status = control.spi_get_status(config.bus)
        control.spi_set_config(
            config.bus,
            capture=SpiCaptureMode.DISABLED,
            spi_mode=config.spi_mode,
            channel_select_mask=0,
            timeout_us=0,
        )
        control.set_stream_enabled(False)

    raw_mosi = bytes(captured_mosi)
    raw_miso = bytes(captured_miso)
    throughput_mbps = (config.total_bytes * 8.0) / tx_seconds / 1000000.0

    marker_index = raw_mosi.find(marker)

    if marker_index < 0:
        return TrialResult(
            ok=False,
            mismatch="marker-not-found",
            mosi_bytes=len(raw_mosi),
            miso_bytes=len(raw_miso) if config.capture_mode is SpiCaptureMode.MOSI_MISO else None,
            packets_emitted=status.packets_emitted,
            transactions_emitted=status.transactions_emitted,
            overrun_count=status.overrun_count,
            sink_overrun_count=status.sink_overrun_count,
            sampler_overrun_count=status.sampler_overrun_count,
            ring_drop_count=status.ring_drop_count,
            usb_host_backpressure_stall_count=status.usb_host_backpressure_stall_count,
            dma_words_consumed=status.dma_words_consumed,
            fragment_push_attempt_count=status.fragment_push_attempt_count,
            peak_ring_depth_packets=status.peak_ring_depth_packets,
            timeout_close_count=status.timeout_close_count,
            throughput_mbps=throughput_mbps,
            window_seconds=tx_seconds,
        )

    stress_mosi = raw_mosi[marker_index + len(marker):marker_index + len(marker) + config.total_bytes]
    stress_miso = raw_miso[marker_index + len(marker):marker_index + len(marker) + config.total_bytes]
    mismatch = first_mismatch(expected, stress_mosi)
    ok = mismatch is None
    if config.capture_mode is SpiCaptureMode.MOSI_MISO:
        ok = ok and (len(stress_miso) == config.total_bytes)

    return TrialResult(
        ok=ok,
        mismatch=mismatch,
        mosi_bytes=len(stress_mosi),
        miso_bytes=len(stress_miso) if config.capture_mode is SpiCaptureMode.MOSI_MISO else None,
        packets_emitted=status.packets_emitted,
        transactions_emitted=status.transactions_emitted,
        overrun_count=status.overrun_count,
        sink_overrun_count=status.sink_overrun_count,
        sampler_overrun_count=status.sampler_overrun_count,
        ring_drop_count=status.ring_drop_count,
        usb_host_backpressure_stall_count=status.usb_host_backpressure_stall_count,
        dma_words_consumed=status.dma_words_consumed,
        fragment_push_attempt_count=status.fragment_push_attempt_count,
        peak_ring_depth_packets=status.peak_ring_depth_packets,
        timeout_close_count=status.timeout_close_count,
        throughput_mbps=throughput_mbps,
        window_seconds=tx_seconds,
    )


def format_trial(index: int, total_bytes: int, capture_mode: SpiCaptureMode, result: TrialResult) -> str:
    miso_text = ""
    if capture_mode is SpiCaptureMode.MOSI_MISO:
        miso_text = f" miso={result.miso_bytes}/{total_bytes}"

    return (
        f"trial{index}:{'PASS' if result.ok else 'FAIL'} "
        f"mosi={result.mosi_bytes}/{total_bytes}{miso_text} "
        f"mismatch={result.mismatch} tx={result.throughput_mbps:.2f}Mb/s "
        f"window={result.window_seconds:.2f}s packets={result.packets_emitted} txns={result.transactions_emitted} "
        f"overruns={result.overrun_count} sink={result.sink_overrun_count} "
        f"sampler={result.sampler_overrun_count} ring={result.ring_drop_count} "
        f"host_stalls={result.usb_host_backpressure_stall_count} "
        f"dma_words={result.dma_words_consumed} fragment_attempts={result.fragment_push_attempt_count} "
        f"peak={result.peak_ring_depth_packets} "
        f"timeout_closes={result.timeout_close_count}"
    )


def current_clock_khz(board: str, firmware_build_dir: str | None) -> int | None:
    if firmware_build_dir is not None:
        cache_path = REPO_ROOT / firmware_build_dir / "CMakeCache.txt"
        if cache_path.is_file():
            text = cache_path.read_text(encoding="utf-8")
            match = re.search(r"^PICOTRACE_SYSTEM_CLOCK_KHZ:STRING=(\d+)\s*$", text, re.MULTILINE)
            if match is not None:
                return int(match.group(1))

    system_h = REPO_ROOT / "firmware" / "src" / "driver" / "system.h"
    text = system_h.read_text(encoding="utf-8")
    match = re.search(
        r"#if defined\(PICO_RP2350A\) \|\| defined\(PICO_RP2350B\)\s*"
        r"#define SYSTEM_CLOCK (\d+)u\s*#else\s*#define SYSTEM_CLOCK (\d+)u",
        text,
        re.MULTILINE,
    )

    if match is None:
        return None

    return int(match.group(1 if board.startswith("pico2") else 2))


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)

    if args.chunk_bytes <= 0 or args.repeat_count <= 0 or args.warmup_bytes <= 0 or args.trials <= 0:
        parser.error("chunk sizes, repeat count, warmup bytes, and trials must be positive")

    capture_mode = decode_capture_mode(args.capture)
    marker = DEFAULT_MARKER
    data_chunk = bytes((index & 0xFF) for index in range(args.chunk_bytes))
    expected = data_chunk * args.repeat_count
    total_bytes = args.chunk_bytes * args.repeat_count

    clock_khz = current_clock_khz(args.board, args.firmware_build_dir)
    prefix = f"firmware_clock={clock_khz}kHz " if clock_khz is not None else ""
    print(
        f"{prefix}capture={args.capture} chunk_bytes={args.chunk_bytes} "
        f"repeat_count={args.repeat_count} total_bytes={total_bytes} trials={args.trials}"
    )

    for speed_hz in args.speed_hz:
        config = BenchmarkConfig(
            device=args.device,
            capture_mode=capture_mode,
            speed_hz=speed_hz,
            spi_mode=args.spi_mode,
            bits_per_word=args.bits_per_word,
            delay_usecs=args.delay_usecs,
            bus=args.bus,
            channel=args.channel,
            channel_select_mask=args.channel_select_mask,
            timeout_us=args.timeout_us,
            chunk_bytes=args.chunk_bytes,
            repeat_count=args.repeat_count,
            warmup_bytes=args.warmup_bytes,
            settle_seconds=args.settle_seconds,
            drain_seconds=args.drain_seconds,
            trials=args.trials,
        )

        results: list[TrialResult] = []
        passes = 0
        for _ in range(config.trials):
            result = collect_stream(config, marker, expected)
            results.append(result)
            if result.ok:
                passes += 1

        detail_text = " | ".join(
            format_trial(index + 1, config.total_bytes, config.capture_mode, result)
            for index, result in enumerate(results)
        )
        print(f"requested={config.speed_hz / 1000000.0:.3f}MHz pass_rate={passes}/{config.trials} {detail_text}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())