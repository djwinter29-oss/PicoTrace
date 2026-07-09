from __future__ import annotations

from collections.abc import Callable

from ..trace import (
    TraceChannelRegistry,
    TracePacket,
    TraceType,
    decode_i2c_events,
    decode_spi_samples,
    iter_trace_packets,
)


_STREAM_CHUNK_SECONDS = 24.0 * 60.0 * 60.0


def _format_trace_packet(packet: TracePacket) -> str:
    header = packet.header
    prefix = f"[{header.timestamp_us:>10}] seq={header.sequence:>6} ch={header.channel} {header.trace_type.name} "
    if header.trace_type is TraceType.I2C:
        events = decode_i2c_events(packet)
        payload = " ".join(f"{event.event_type.name}:{event.value:02X}" for event in events)
        return f"{prefix}{payload}"

    samples = decode_spi_samples(packet)
    if samples.miso is None:
        payload = " ".join(f"{byte:02X}" for byte in samples.mosi)
        return f"{prefix}MOSI {payload}"

    pairs = " ".join(f"{mosi:02X}/{miso:02X}" for mosi, miso in zip(samples.mosi, samples.miso))
    return f"{prefix}{samples.capture_mode.name} {pairs}"


def _stream_channel(channel: int) -> int:
    return _stream_channel_with_hooks(channel)


def _stream_channel_with_hooks(channel: int, *, on_started: Callable[[], None] | None = None) -> int:
    registry = TraceChannelRegistry([channel])
    print(f"streaming channel {channel}; press Ctrl+C to stop")
    try:
        for packet in iter_trace_packets(
            duration_seconds=_STREAM_CHUNK_SECONDS,
            channel_registry=registry,
            on_opened=on_started,
        ):
            print(_format_trace_packet(packet), flush=True)
    except KeyboardInterrupt:
        print("stream stopped")
    return 0
