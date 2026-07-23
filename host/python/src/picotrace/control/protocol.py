from __future__ import annotations

"""Pure PicoTrace HID command framing and status payload decoding helpers."""

from dataclasses import dataclass
from enum import IntEnum

from ..trace.decode import SpiCaptureMode


__all__ = [
    "DEFAULT_HID_REPORT_SIZE",
    "HidCommand",
    "HidControlError",
    "HidDeviceStatus",
    "HidOpcode",
    "HidProtocolError",
    "HidResponse",
    "HidStatus",
    "I2cMonitorAllStatus",
    "I2cMonitorStatus",
    "SpiBusConfig",
    "SpiMonitorAllStatus",
    "SpiMonitorStatus",
    "build_i2c_set_rate_payload",
    "build_spi_set_config_payload",
    "decode_hid_response",
    "decode_i2c_monitor_all_status_payload",
    "decode_i2c_monitor_status_payload",
    "decode_spi_monitor_all_status_payload",
    "decode_spi_monitor_status_payload",
]


DEFAULT_HID_REPORT_SIZE = 64
_HID_I2C_STATUS_BYTES = 18
_HID_I2C_ALL_STATUS_CHANNEL_BYTES = 14
_HID_SPI_STATUS_BYTES = 54
_HID_SPI_ALL_STATUS_CHANNEL_BYTES = 10


class HidOpcode(IntEnum):
    NOP = 0x00
    GET_STATUS = 0x01
    STREAM_ENABLE = 0x02
    STREAM_DISABLE = 0x03
    I2C_MONITOR_SET_RATE = 0x04
    I2C_MONITOR_GET_STATUS = 0x05
    I2C_MONITOR_GET_ALL_STATUS = 0x06
    SPI_MONITOR_SET_CONFIG = 0x07
    SPI_MONITOR_GET_STATUS = 0x08
    SPI_MONITOR_GET_ALL_STATUS = 0x09
    LED_ON = 0x80
    LED_OFF = 0x81
    REBOOT = 0x82


class HidStatus(IntEnum):
    OK = 0x00
    UNKNOWN_COMMAND = 0x01
    BAD_LENGTH = 0x02
    PENDING = 0x03
    REJECTED = 0x04
    BUSY = 0x05


class HidControlError(RuntimeError):
    pass


class HidProtocolError(HidControlError):
    pass


@dataclass(frozen=True)
class HidCommand:
    opcode: int
    sequence: int = 0
    payload: bytes = b""

    def to_report_bytes(self) -> bytes:
        if len(self.payload) > (DEFAULT_HID_REPORT_SIZE - 4):
            raise ValueError("HID payload exceeds the fixed PicoTrace report size")

        report = bytearray(DEFAULT_HID_REPORT_SIZE)
        report[0] = int(self.opcode) & 0xFF
        report[1] = self.sequence & 0xFF
        report[2] = 0
        report[3] = len(self.payload)
        report[4 : 4 + len(self.payload)] = self.payload
        return bytes(report)


@dataclass(frozen=True)
class HidResponse:
    opcode: int
    sequence: int
    status: HidStatus
    payload: bytes

    @property
    def ok(self) -> bool:
        return self.status is HidStatus.OK


@dataclass(frozen=True)
class HidDeviceStatus:
    stream_enabled: bool
    firmware_version: str = ""


@dataclass(frozen=True)
class I2cMonitorStatus:
    channel: int
    initialized: bool
    running: bool
    overrun: bool
    sample_hz: int
    completed_buffers: int
    overrun_count: int
    transition_pending: bool
    transition_reason: int


@dataclass(frozen=True)
class I2cMonitorAllStatus:
    channel: int
    initialized: bool
    running: bool
    overrun: bool
    sample_hz: int
    overrun_count: int
    transition_pending: bool
    transition_reason: int


@dataclass(frozen=True)
class SpiBusConfig:
    capture: SpiCaptureMode
    spi_mode: int
    channel_select_mask: int
    timeout_us: int


@dataclass(frozen=True)
class SpiMonitorStatus:
    bus: int
    initialized: bool
    running: bool
    capture: SpiCaptureMode
    spi_mode: int
    channel_select_mask: int
    timeout_us: int
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


@dataclass(frozen=True)
class SpiMonitorAllStatus:
    bus: int
    initialized: bool
    running: bool
    capture: SpiCaptureMode
    spi_mode: int
    timeout_us: int
    overrun: bool


def _read_u32_le(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], byteorder="little", signed=False)


def decode_hid_response(report_bytes: bytes | bytearray | memoryview) -> HidResponse:
    raw = bytes(report_bytes)
    if len(raw) != DEFAULT_HID_REPORT_SIZE:
        raise HidProtocolError("HID response must match the fixed 64-byte PicoTrace report size")

    payload_length = raw[3]
    if payload_length > (DEFAULT_HID_REPORT_SIZE - 4):
        raise HidProtocolError("HID response payload length exceeds the fixed PicoTrace report size")

    try:
        status = HidStatus(raw[2])
    except ValueError as exc:
        raise HidProtocolError(f"unknown HID status: {raw[2]}") from exc

    return HidResponse(
        opcode=raw[0],
        sequence=raw[1],
        status=status,
        payload=raw[4 : 4 + payload_length],
    )


def decode_device_status_payload(payload: bytes) -> HidDeviceStatus:
    if len(payload) == 1:
        return HidDeviceStatus(stream_enabled=(payload[0] != 0), firmware_version="")
    if len(payload) < 2:
        raise HidProtocolError("GET_STATUS response must contain at least one payload byte")

    version_length = payload[1]
    if len(payload) != (2 + version_length):
        raise HidProtocolError("GET_STATUS response has an unexpected firmware version payload size")

    return HidDeviceStatus(
        stream_enabled=(payload[0] != 0),
        firmware_version=payload[2 : 2 + version_length].decode("ascii"),
    )


def build_i2c_set_rate_payload(channel: int, sample_hz: int) -> bytes:
    return bytes([channel & 0xFF]) + int(sample_hz).to_bytes(4, byteorder="little", signed=False)


def decode_i2c_monitor_status_payload(payload: bytes) -> I2cMonitorStatus:
    if len(payload) != _HID_I2C_STATUS_BYTES:
        raise HidProtocolError("I2C monitor status response has an unexpected payload size")

    return I2cMonitorStatus(
        channel=payload[0],
        initialized=(payload[1] != 0),
        running=(payload[2] != 0),
        overrun=(payload[3] != 0),
        sample_hz=_read_u32_le(payload, 4),
        completed_buffers=_read_u32_le(payload, 8),
        overrun_count=_read_u32_le(payload, 12),
        transition_pending=(payload[16] != 0),
        transition_reason=payload[17],
    )


def decode_i2c_monitor_all_status_payload(payload: bytes) -> tuple[I2cMonitorAllStatus, ...]:
    if (len(payload) % _HID_I2C_ALL_STATUS_CHANNEL_BYTES) != 0:
        raise HidProtocolError("I2C all-status response has an unexpected payload size")

    statuses: list[I2cMonitorAllStatus] = []
    for offset in range(0, len(payload), _HID_I2C_ALL_STATUS_CHANNEL_BYTES):
        channel_payload = payload[offset : offset + _HID_I2C_ALL_STATUS_CHANNEL_BYTES]
        statuses.append(
            I2cMonitorAllStatus(
                channel=channel_payload[0],
                initialized=(channel_payload[1] != 0),
                running=(channel_payload[2] != 0),
                overrun=(channel_payload[3] != 0),
                sample_hz=_read_u32_le(channel_payload, 4),
                overrun_count=_read_u32_le(channel_payload, 8),
                transition_pending=(channel_payload[12] != 0),
                transition_reason=channel_payload[13],
            )
        )
    return tuple(statuses)


def build_spi_set_config_payload(
    bus: int,
    *,
    capture: SpiCaptureMode,
    spi_mode: int,
    channel_select_mask: int,
    timeout_us: int,
) -> bytes:
    return bytes(
        [bus & 0xFF, int(capture) & 0xFF, spi_mode & 0xFF, channel_select_mask & 0xFF]
    ) + int(timeout_us).to_bytes(4, byteorder="little", signed=False)


def decode_spi_monitor_status_payload(payload: bytes) -> SpiMonitorStatus:
    if len(payload) != _HID_SPI_STATUS_BYTES:
        raise HidProtocolError("SPI monitor status response has an unexpected payload size")

    return SpiMonitorStatus(
        bus=payload[0],
        initialized=(payload[1] != 0),
        running=(payload[2] != 0),
        capture=SpiCaptureMode(payload[3]),
        spi_mode=payload[4],
        channel_select_mask=payload[5],
        timeout_us=_read_u32_le(payload, 6),
        packets_emitted=_read_u32_le(payload, 10),
        transactions_emitted=_read_u32_le(payload, 14),
        overrun_count=_read_u32_le(payload, 18),
        sink_overrun_count=_read_u32_le(payload, 22),
        sampler_overrun_count=_read_u32_le(payload, 26),
        ring_drop_count=_read_u32_le(payload, 30),
        usb_host_backpressure_stall_count=_read_u32_le(payload, 34),
        dma_words_consumed=_read_u32_le(payload, 38),
        fragment_push_attempt_count=_read_u32_le(payload, 42),
        peak_ring_depth_packets=_read_u32_le(payload, 46),
        timeout_close_count=_read_u32_le(payload, 50),
    )


def decode_spi_monitor_all_status_payload(payload: bytes) -> tuple[SpiMonitorAllStatus, ...]:
    if (len(payload) % _HID_SPI_ALL_STATUS_CHANNEL_BYTES) != 0:
        raise HidProtocolError("SPI all-status response has an unexpected payload size")

    statuses: list[SpiMonitorAllStatus] = []
    for offset in range(0, len(payload), _HID_SPI_ALL_STATUS_CHANNEL_BYTES):
        channel_payload = payload[offset : offset + _HID_SPI_ALL_STATUS_CHANNEL_BYTES]
        statuses.append(
            SpiMonitorAllStatus(
                bus=channel_payload[0],
                initialized=(channel_payload[1] != 0),
                running=(channel_payload[2] != 0),
                capture=SpiCaptureMode(channel_payload[3]),
                spi_mode=channel_payload[4],
                timeout_us=_read_u32_le(channel_payload, 5),
                overrun=(channel_payload[9] != 0),
            )
        )
    return tuple(statuses)