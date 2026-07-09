from __future__ import annotations

"""USB HID control client for the PicoTrace command interface."""

from dataclasses import dataclass

import usb.core
import usb.util

from ..trace.usb_bulk import find_usb_device
from ..trace.decode import SpiCaptureMode
from .protocol import (
    DEFAULT_HID_REPORT_SIZE,
    HidCommand,
    HidControlError,
    HidDeviceStatus,
    HidOpcode,
    HidProtocolError,
    HidResponse,
    HidStatus,
    I2cMonitorAllStatus,
    I2cMonitorStatus,
    SpiBusConfig,
    SpiMonitorAllStatus,
    SpiMonitorStatus,
    build_i2c_set_rate_payload,
    build_spi_set_config_payload,
    decode_device_status_payload,
    decode_hid_response,
    decode_i2c_monitor_all_status_payload,
    decode_i2c_monitor_status_payload,
    decode_spi_monitor_all_status_payload,
    decode_spi_monitor_status_payload,
)


__all__ = [
    "DEFAULT_HID_CONTROL_IN_ENDPOINT",
    "DEFAULT_HID_REPORT_SIZE",
    "HidCommand",
    "HidControlClient",
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
    "close_hid_control_device",
    "decode_hid_response",
    "open_hid_control_device",
]


DEFAULT_VID = 0xCAFE
DEFAULT_PID = 0x4003
DEFAULT_HID_CONTROL_IN_ENDPOINT = 0x84
_HID_INPUT_REPORT = 0x01
_HID_OUTPUT_REPORT = 0x02
_HID_REQUEST_GET_REPORT = 0x01
_HID_REQUEST_SET_REPORT = 0x09
_HID_REQUEST_TIMEOUT_MS = 250
_HID_SET_REQUEST = usb.util.build_request_type(
    usb.util.CTRL_OUT,
    usb.util.CTRL_TYPE_CLASS,
    usb.util.CTRL_RECIPIENT_INTERFACE,
)
_HID_GET_REQUEST = usb.util.build_request_type(
    usb.util.CTRL_IN,
    usb.util.CTRL_TYPE_CLASS,
    usb.util.CTRL_RECIPIENT_INTERFACE,
)


@dataclass(frozen=True)
class _HidControlHandle:
    device: usb.core.Device
    interface_number: int


def _read_hid_response_report(
    device: usb.core.Device,
    interface_number: int,
    timeout_ms: int,
) -> bytes:
    report = device.ctrl_transfer(
        _HID_GET_REQUEST,
        _HID_REQUEST_GET_REPORT,
        (_HID_INPUT_REPORT << 8),
        interface_number,
        DEFAULT_HID_REPORT_SIZE,
        timeout=timeout_ms,
    )
    return bytes(report)


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


def open_hid_control_device(
    vid: int = DEFAULT_VID,
    pid: int = DEFAULT_PID,
    hid_in_endpoint: int = DEFAULT_HID_CONTROL_IN_ENDPOINT,
) -> tuple[usb.core.Device, int]:
    device = find_usb_device(vid, pid)

    try:
        device.set_configuration()
    except usb.core.USBError as exc:
        if exc.errno is None:
            error_text = str(exc).lower()
            if "busy" not in error_text and "access" not in error_text:
                raise

    configuration = device.get_active_configuration()
    interface = None
    for candidate_interface in configuration:
        if int(candidate_interface.bInterfaceClass) != 0x03:
            continue
        for endpoint in candidate_interface:
            if int(endpoint.bEndpointAddress) == hid_in_endpoint:
                interface = candidate_interface
                break
        if interface is not None:
            break

    if interface is None:
        raise RuntimeError(f"HID control endpoint 0x{hid_in_endpoint:02x} not found on the active USB configuration")

    interface_number = int(interface.bInterfaceNumber)

    try:
        if device.is_kernel_driver_active(interface_number):
            device.detach_kernel_driver(interface_number)
    except (NotImplementedError, usb.core.USBError):
        pass

    usb.util.claim_interface(device, interface_number)
    return device, interface_number


def close_hid_control_device(device: usb.core.Device, interface_number: int | None) -> None:
    try:
        if interface_number is not None:
            usb.util.release_interface(device, interface_number)
        usb.util.dispose_resources(device)
    except usb.core.USBError:
        pass


class HidControlClient:
    def __init__(self, device: usb.core.Device, interface_number: int, timeout_ms: int = _HID_REQUEST_TIMEOUT_MS) -> None:
        self._handle = _HidControlHandle(device=device, interface_number=interface_number)
        self._timeout_ms = timeout_ms
        self._next_sequence = 0

    @classmethod
    def open(
        cls,
        vid: int = DEFAULT_VID,
        pid: int = DEFAULT_PID,
        hid_in_endpoint: int = DEFAULT_HID_CONTROL_IN_ENDPOINT,
        timeout_ms: int = _HID_REQUEST_TIMEOUT_MS,
    ) -> HidControlClient:
        device, interface_number = open_hid_control_device(vid=vid, pid=pid, hid_in_endpoint=hid_in_endpoint)
        return cls(device=device, interface_number=interface_number, timeout_ms=timeout_ms)

    def close(self) -> None:
        close_hid_control_device(self._handle.device, self._handle.interface_number)

    def __enter__(self) -> HidControlClient:
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    def transact(self, command: HidCommand) -> HidResponse:
        device = self._handle.device
        interface_number = self._handle.interface_number

        written = device.ctrl_transfer(
            _HID_SET_REQUEST,
            _HID_REQUEST_SET_REPORT,
            (_HID_OUTPUT_REPORT << 8),
            interface_number,
            command.to_report_bytes(),
            timeout=self._timeout_ms,
        )
        if written != DEFAULT_HID_REPORT_SIZE:
            raise HidProtocolError(f"short HID write: expected {DEFAULT_HID_REPORT_SIZE}, wrote {written}")

        response = decode_hid_response(_read_hid_response_report(device, interface_number, self._timeout_ms))
        if response.sequence != command.sequence:
            raise HidProtocolError(
                f"HID response sequence mismatch: expected {command.sequence}, got {response.sequence}"
            )
        if response.opcode != int(command.opcode):
            raise HidProtocolError(f"HID response opcode mismatch: expected {int(command.opcode)}, got {response.opcode}")
        return response

    def request(self, opcode: HidOpcode, payload: bytes = b"", sequence: int | None = None) -> HidResponse:
        if sequence is None:
            sequence = self._next_sequence
            self._next_sequence = (self._next_sequence + 1) & 0xFF
        return self.transact(HidCommand(opcode=opcode, sequence=sequence, payload=payload))

    def _require_ok(self, response: HidResponse) -> HidResponse:
        if response.ok:
            return response
        raise HidControlError(f"HID command failed with status {response.status.name}")

    def get_status(self) -> HidDeviceStatus:
        response = self._require_ok(self.request(HidOpcode.GET_STATUS))
        return decode_device_status_payload(response.payload)

    def set_stream_enabled(self, enabled: bool) -> None:
        opcode = HidOpcode.STREAM_ENABLE if enabled else HidOpcode.STREAM_DISABLE
        self._require_ok(self.request(opcode))

    def set_led(self, on: bool) -> None:
        opcode = HidOpcode.LED_ON if on else HidOpcode.LED_OFF
        self._require_ok(self.request(opcode))

    def reboot(self) -> None:
        self._require_ok(self.request(HidOpcode.REBOOT))

    def i2c_set_rate(self, channel: int, sample_hz: int) -> None:
        self._require_ok(self.request(HidOpcode.I2C_MONITOR_SET_RATE, build_i2c_set_rate_payload(channel, sample_hz)))

    def i2c_get_status(self, channel: int) -> I2cMonitorStatus:
        response = self._require_ok(self.request(HidOpcode.I2C_MONITOR_GET_STATUS, bytes([channel & 0xFF])))
        return decode_i2c_monitor_status_payload(response.payload)

    def i2c_get_all_status(self) -> tuple[I2cMonitorAllStatus, ...]:
        response = self._require_ok(self.request(HidOpcode.I2C_MONITOR_GET_ALL_STATUS))
        return decode_i2c_monitor_all_status_payload(response.payload)

    def spi_set_config(
        self,
        bus: int,
        *,
        capture: SpiCaptureMode,
        spi_mode: int,
        channel_select_mask: int,
        timeout_us: int,
    ) -> None:
        self._require_ok(
            self.request(
                HidOpcode.SPI_MONITOR_SET_CONFIG,
                build_spi_set_config_payload(
                    bus,
                    capture=capture,
                    spi_mode=spi_mode,
                    channel_select_mask=channel_select_mask,
                    timeout_us=timeout_us,
                ),
            )
        )

    def spi_get_status(self, bus: int) -> SpiMonitorStatus:
        response = self._require_ok(self.request(HidOpcode.SPI_MONITOR_GET_STATUS, bytes([bus & 0xFF])))
        return decode_spi_monitor_status_payload(response.payload)

    def spi_get_all_status(self) -> tuple[SpiMonitorAllStatus, ...]:
        response = self._require_ok(self.request(HidOpcode.SPI_MONITOR_GET_ALL_STATUS))
        return decode_spi_monitor_all_status_payload(response.payload)