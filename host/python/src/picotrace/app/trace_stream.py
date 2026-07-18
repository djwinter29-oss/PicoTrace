from __future__ import annotations

from collections.abc import Callable

from ..trace import (
    I2cEventType,
    TraceChannelRegistry,
    TraceFlags,
    TracePacket,
    TracePacketHeader,
    TraceType,
    decode_i2c_events,
    decode_spi_samples,
    iter_trace_packets,
)


_STREAM_CHUNK_SECONDS = 24.0 * 60.0 * 60.0
_SPI_PACKET_COALESCE_GAP_US = 1000


def _format_timestamp_us(timestamp_us: int) -> str:
    total_seconds, micros = divmod(timestamp_us, 1_000_000)
    minutes, seconds = divmod(total_seconds, 60)
    hours, minutes = divmod(minutes, 60)
    return f"{hours:02}:{minutes:02}:{seconds:02}.{micros:06}"


def _format_i2c_event(event) -> str:
    event_type = event.event_type

    if event_type is I2cEventType.START:
        return "START"
    if event_type is I2cEventType.DATA:
        return f"DATA:{event.value:02X}"
    if event_type is I2cEventType.ACK:
        return "ACK" if event.value == 0 else "NACK"
    if event_type is I2cEventType.STOP:
        return "STOP"

    return f"{event_type.name}:{event.value:02X}"


def _format_trace_packet(packet: TracePacket) -> str:
    header = packet.header
    if header.trace_type is TraceType.I2C:
        prefix = f"[{_format_timestamp_us(header.timestamp_us)}] seq={header.sequence:>6} {header.trace_type.name} CH{header.channel}: "
        events = decode_i2c_events(packet)
        payload = " ".join(_format_i2c_event(event) for event in events)
        return f"{prefix}{payload}"

    prefix = f"[{_format_timestamp_us(header.timestamp_us)}] seq={header.sequence:>6} {header.trace_type.name} CH{header.channel} "
    samples = decode_spi_samples(packet)
    if samples.miso is None:
        payload = " ".join(f"{byte:02X}" for byte in samples.mosi)
        return f"{prefix}MOSI: {payload}"

    pairs = " ".join(f"{mosi:02X}/{miso:02X}" for mosi, miso in zip(samples.mosi, samples.miso))
    return f"{prefix}{samples.capture_mode.name} {pairs}"


def _can_coalesce_spi_packets(current: TracePacket, nxt: TracePacket) -> bool:
    current_header = current.header
    next_header = nxt.header

    if (current_header.trace_type is not TraceType.SPI) or (next_header.trace_type is not TraceType.SPI):
        return False
    if current_header.channel != next_header.channel:
        return False
    if current_header.meta != next_header.meta:
        return False
    if next_header.sequence != current_header.sequence:
        return False
    if (next_header.timestamp_us - current_header.timestamp_us) > _SPI_PACKET_COALESCE_GAP_US:
        return False
    if (next_header.flag_bits & TraceFlags.CONTINUED) == 0:
        return False

    disallowed_flags = TraceFlags.OVERFLOW | TraceFlags.TRUNCATED | TraceFlags.ERROR
    if (current_header.flag_bits & disallowed_flags) or (next_header.flag_bits & disallowed_flags):
        return False

    return True


def _coalesce_spi_packets(current: TracePacket, nxt: TracePacket) -> TracePacket:
    payload = current.payload + nxt.payload
    header = TracePacketHeader(
        version=current.header.version,
        type=current.header.type,
        channel=current.header.channel,
        flags=current.header.flags | nxt.header.flags,
        payload_len=len(payload),
        meta=current.header.meta,
        sequence=current.header.sequence,
        timestamp_us=current.header.timestamp_us,
    )
    return TracePacket(header=header, payload=payload)


def _should_flush_immediately(packet: TracePacket) -> bool:
    flag_bits = getattr(packet.header, "flag_bits", TraceFlags(0))
    try:
        return (int(flag_bits) & int(TraceFlags.END)) != 0
    except (TypeError, ValueError):
        return False


def _stream_channel(channel: int) -> int:
    return _stream_channel_with_hooks(channel)


def _stream_all() -> int:
    return _stream_all_with_hooks()


def _stream_channel_with_hooks(channel: int, *, on_started: Callable[[], None] | None = None) -> int:
    registry = TraceChannelRegistry([channel])
    return _stream_with_registry(
        registry,
        f"streaming channel {channel}; press Ctrl+C to stop",
        duration_seconds=_STREAM_CHUNK_SECONDS,
        on_started=on_started,
    )


def _stream_all_with_hooks(*, on_started: Callable[[], None] | None = None) -> int:
    return _stream_with_registry(
        TraceChannelRegistry(),
        "streaming all trace traffic; press Ctrl+C to stop",
        duration_seconds=None,
        on_started=on_started,
    )


def _stream_with_registry(
    registry: TraceChannelRegistry,
    start_message: str,
    *,
    duration_seconds: float | None,
    on_started: Callable[[], None] | None = None,
) -> int:
    pending_packet: TracePacket | None = None
    stream_opened = False

    def handle_opened() -> None:
        nonlocal stream_opened
        if stream_opened:
            return

        stream_opened = True
        print(start_message)
        if on_started is not None:
            on_started()

    try:
        for packet in iter_trace_packets(
            duration_seconds=duration_seconds,
            channel_registry=registry,
            on_opened=handle_opened,
        ):
            if pending_packet is None:
                pending_packet = packet
                if _should_flush_immediately(pending_packet):
                    print(_format_trace_packet(pending_packet), flush=True)
                    pending_packet = None
                continue

            if _can_coalesce_spi_packets(pending_packet, packet):
                pending_packet = _coalesce_spi_packets(pending_packet, packet)
                if _should_flush_immediately(pending_packet):
                    print(_format_trace_packet(pending_packet), flush=True)
                    pending_packet = None
                continue

            print(_format_trace_packet(pending_packet), flush=True)
            pending_packet = packet
            if _should_flush_immediately(pending_packet):
                print(_format_trace_packet(pending_packet), flush=True)
                pending_packet = None
    except KeyboardInterrupt:
        if pending_packet is not None:
            print(_format_trace_packet(pending_packet), flush=True)
        print("stream stopped")
        return 0

    if pending_packet is not None:
        print(_format_trace_packet(pending_packet), flush=True)

    return 0
