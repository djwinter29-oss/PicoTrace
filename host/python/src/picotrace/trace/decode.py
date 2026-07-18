from __future__ import annotations

"""Pure PicoTrace packet framing and protocol decode helpers.

This module is transport-agnostic on purpose so the same decode path can be
fed from USB bulk, CDC, files, or any other byte stream source.
"""

from dataclasses import dataclass
from enum import IntEnum, IntFlag
import struct


TRACE_PACKET_VERSION = 1
TRACE_PACKET_BYTES = 256
TRACE_PACKET_HEADER_BYTES = 16
TRACE_PACKET_PAYLOAD_BYTES = TRACE_PACKET_BYTES - TRACE_PACKET_HEADER_BYTES

_TRACE_PACKET_HEADER = struct.Struct("<BBBBHHII")


class TraceType(IntEnum):
    I2C = 1
    SPI = 2


class TraceFlags(IntFlag):
    END = 1 << 0
    CONTINUED = 1 << 1
    OVERFLOW = 1 << 2
    TRUNCATED = 1 << 3
    ERROR = 1 << 4


class I2cEventType(IntEnum):
    START = 1
    DATA = 2
    ACK = 3
    STOP = 4
    ERROR = 128
    OVERFLOW = 129
    CONTROL_RECONFIG = 130
    CONTROL_STOP = 131


class SpiCaptureMode(IntEnum):
    DISABLED = 0
    MOSI = 1
    MOSI_MISO = 2


class TraceDecodeError(ValueError):
    pass


@dataclass(frozen=True)
class TracePacketHeader:
    version: int
    type: int
    channel: int
    flags: int
    payload_len: int
    meta: int
    sequence: int
    timestamp_us: int

    @property
    def trace_type(self) -> TraceType:
        try:
            return TraceType(self.type)
        except ValueError as exc:
            raise TraceDecodeError(f"unknown trace type: {self.type}") from exc

    @property
    def flag_bits(self) -> TraceFlags:
        return TraceFlags(self.flags)


@dataclass(frozen=True)
class TracePacket:
    header: TracePacketHeader
    payload: bytes


@dataclass(frozen=True)
class I2cEvent:
    type: int
    value: int

    @property
    def event_type(self) -> I2cEventType:
        try:
            return I2cEventType(self.type)
        except ValueError as exc:
            raise TraceDecodeError(f"unknown I2C event type: {self.type}") from exc


@dataclass(frozen=True)
class SpiSamples:
    capture_mode: SpiCaptureMode
    mosi: bytes
    miso: bytes | None


def decode_trace_packet(packet_bytes: bytes | bytearray | memoryview) -> TracePacket:
    raw = bytes(packet_bytes)
    if len(raw) < TRACE_PACKET_HEADER_BYTES:
        raise TraceDecodeError("trace packet is shorter than the fixed header")

    header = TracePacketHeader(*_TRACE_PACKET_HEADER.unpack(raw[:TRACE_PACKET_HEADER_BYTES]))
    if header.version != TRACE_PACKET_VERSION:
        raise TraceDecodeError(f"unsupported trace packet version: {header.version}")
    if header.payload_len > TRACE_PACKET_PAYLOAD_BYTES:
        raise TraceDecodeError(
            f"payload_len {header.payload_len} exceeds maximum {TRACE_PACKET_PAYLOAD_BYTES}"
        )

    expected_size = TRACE_PACKET_HEADER_BYTES + header.payload_len
    if len(raw) != expected_size:
        raise TraceDecodeError(
            f"trace packet length {len(raw)} does not match header-declared size {expected_size}"
        )

    return TracePacket(header=header, payload=raw[TRACE_PACKET_HEADER_BYTES:expected_size])


def decode_i2c_events(packet: TracePacket) -> tuple[I2cEvent, ...]:
    if packet.header.trace_type is not TraceType.I2C:
        raise TraceDecodeError("trace packet does not carry I2C data")
    if (packet.header.payload_len % 2) != 0:
        raise TraceDecodeError("I2C payload length must be an even number of bytes")

    events = tuple(
        I2cEvent(type=packet.payload[index], value=packet.payload[index + 1])
        for index in range(0, len(packet.payload), 2)
    )
    if packet.header.meta != len(events):
        raise TraceDecodeError(
            f"I2C event count mismatch: header meta {packet.header.meta}, decoded {len(events)}"
        )
    return events


def decode_spi_samples(packet: TracePacket) -> SpiSamples:
    if packet.header.trace_type is not TraceType.SPI:
        raise TraceDecodeError("trace packet does not carry SPI data")

    try:
        capture_mode = SpiCaptureMode(packet.header.meta)
    except ValueError as exc:
        raise TraceDecodeError(f"unknown SPI capture mode: {packet.header.meta}") from exc

    if capture_mode is SpiCaptureMode.MOSI:
        return SpiSamples(capture_mode=capture_mode, mosi=packet.payload, miso=None)

    if capture_mode is SpiCaptureMode.MOSI_MISO:
        if (packet.header.payload_len % 2) != 0:
            raise TraceDecodeError("SPI MOSI+MISO payload length must be even")

        mosi = bytearray()
        miso = bytearray()
        for index in range(0, len(packet.payload), 2):
            mosi.append(packet.payload[index])
            miso.append(packet.payload[index + 1])
        return SpiSamples(capture_mode=capture_mode, mosi=bytes(mosi), miso=bytes(miso))

    raise TraceDecodeError("SPI capture mode DISABLED is not valid in emitted trace packets")


class TraceStreamDecoder:
    def __init__(self) -> None:
        self._buffer = bytearray()

    def append(self, data: bytes | bytearray | memoryview) -> list[TracePacket]:
        self._buffer.extend(data)
        packets: list[TracePacket] = []

        while True:
            if len(self._buffer) < TRACE_PACKET_HEADER_BYTES:
                return packets

            header_tuple = _TRACE_PACKET_HEADER.unpack(self._buffer[:TRACE_PACKET_HEADER_BYTES])
            header = TracePacketHeader(*header_tuple)
            if header.version != TRACE_PACKET_VERSION:
                del self._buffer[0]
                continue
            if header.payload_len > TRACE_PACKET_PAYLOAD_BYTES:
                del self._buffer[0]
                continue

            packet_size = TRACE_PACKET_HEADER_BYTES + header.payload_len
            if len(self._buffer) < packet_size:
                return packets

            payload = bytes(self._buffer[TRACE_PACKET_HEADER_BYTES:packet_size])
            del self._buffer[:packet_size]
            packets.append(TracePacket(header=header, payload=payload))

    @property
    def buffered_byte_count(self) -> int:
        return len(self._buffer)