from __future__ import annotations

import argparse
import array
import os
import platform
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import usb.core
import usb.backend.libusb1
import usb.util


DEFAULT_VID = 0xCAFE
DEFAULT_PID = 0x4003
DEFAULT_VENDOR_IN_ENDPOINT = 0x83
DEFAULT_DURATION_SECONDS = 20.0
DEFAULT_READ_SIZE = 16384
DEFAULT_TIMEOUT_MS = 250


@dataclass(frozen=True)
class CaptureStats:
    total_bytes: int
    elapsed_seconds: float

    @property
    def bytes_per_second(self) -> float:
        if self.elapsed_seconds <= 0.0:
            return 0.0
        return self.total_bytes / self.elapsed_seconds


def _candidate_libusb_paths() -> list[Path]:
    candidates: list[Path] = []

    if sys.platform.startswith("win"):
        machine = platform.machine().lower()
        if machine in {"amd64", "x86_64"}:
            runtime = "win-x64"
        elif machine in {"arm64", "aarch64"}:
            runtime = "win-arm64"
        else:
            runtime = "win-x86"

        user_profile = os.environ.get("USERPROFILE")
        if user_profile:
            candidates.append(
                Path(user_profile)
                / ".nuget"
                / "packages"
                / "libusbdotnet"
                / "3.0.102-alpha"
                / "runtimes"
                / runtime
                / "native"
                / "libusb-1.0.dll"
            )

        python_dir = Path(sys.executable).resolve().parent
        candidates.append(python_dir / "libusb-1.0.dll")
        candidates.append(Path.cwd() / "libusb-1.0.dll")

    return [candidate for candidate in candidates if candidate.is_file()]


def _get_libusb_backend() -> usb.backend.libusb1._LibUSB:  # type: ignore[attr-defined]
    backend = usb.backend.libusb1.get_backend()
    if backend is not None:
        return backend

    for candidate in _candidate_libusb_paths():
        if sys.platform.startswith("win") and hasattr(os, "add_dll_directory"):
            os.add_dll_directory(str(candidate.parent))

        backend = usb.backend.libusb1.get_backend(find_library=lambda _name, path=str(candidate): path)
        if backend is not None:
            return backend

    raise RuntimeError("No libusb backend available. Install libusb-1.0 and ensure your user can access the device.")


def _find_device(vid: int, pid: int) -> usb.core.Device:
    backend = _get_libusb_backend()
    device = usb.core.find(idVendor=vid, idProduct=pid, backend=backend)
    if device is None:
        raise RuntimeError(f"Device {vid:04x}:{pid:04x} not found")
    return device


def _find_vendor_interface(device: usb.core.Device, endpoint_address: int) -> usb.core.Interface:
    configuration = device.get_active_configuration()

    for interface in configuration:
        for endpoint in interface:
            if endpoint.bEndpointAddress == endpoint_address:
                return interface

    raise RuntimeError(f"Endpoint 0x{endpoint_address:02x} not found on the active USB configuration")


def _prepare_device(device: usb.core.Device, endpoint_address: int) -> usb.core.Interface:
    try:
        device.set_configuration()
    except usb.core.USBError as exc:
        if exc.errno is None:
            error_text = str(exc).lower()
            if "busy" not in error_text and "access" not in error_text:
                raise

    interface = _find_vendor_interface(device, endpoint_address)
    interface_number = int(interface.bInterfaceNumber)
    alternate_setting = int(interface.bAlternateSetting)

    if sys.platform.startswith("linux"):
        try:
            if device.is_kernel_driver_active(interface_number):
                device.detach_kernel_driver(interface_number)
        except (NotImplementedError, usb.core.USBError):
            pass

    usb.util.claim_interface(device, interface_number)
    device.set_interface_altsetting(interface=interface_number, alternate_setting=alternate_setting)
    return interface_number


def _release_device(device: usb.core.Device, interface_number: int | None) -> None:
    try:
        if interface_number is not None:
            usb.util.release_interface(device, interface_number)
        usb.util.dispose_resources(device)
    except usb.core.USBError:
        pass


def capture_bulk_stream(
    duration_seconds: float = DEFAULT_DURATION_SECONDS,
    vid: int = DEFAULT_VID,
    pid: int = DEFAULT_PID,
    endpoint_address: int = DEFAULT_VENDOR_IN_ENDPOINT,
    read_size: int = DEFAULT_READ_SIZE,
    timeout_ms: int = DEFAULT_TIMEOUT_MS,
) -> CaptureStats:
    if duration_seconds <= 0.0:
        raise ValueError("duration_seconds must be positive")

    if read_size <= 0:
        raise ValueError("read_size must be positive")

    device = _find_device(vid, pid)
    interface_number = _prepare_device(device, endpoint_address)
    backend = device._ctx.backend
    handle = device._ctx.handle
    read_buffer = array.array("B", [0]) * read_size

    total_bytes = 0
    start = time.perf_counter()
    deadline = start + duration_seconds

    try:
        while True:
            now = time.perf_counter()
            if now >= deadline:
                break

            try:
                transferred = backend.bulk_read(handle, endpoint_address, interface_number, read_buffer, timeout_ms)
            except usb.core.USBTimeoutError:
                continue

            total_bytes += transferred
    finally:
        elapsed = time.perf_counter() - start
        _release_device(device, interface_number)

    return CaptureStats(total_bytes=total_bytes, elapsed_seconds=elapsed)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Capture PicoTrace vendor bulk data and report total bytes plus average speed.")
    parser.add_argument("--duration", type=float, default=DEFAULT_DURATION_SECONDS, help="Capture duration in seconds")
    parser.add_argument("--vid", type=lambda value: int(value, 0), default=DEFAULT_VID, help="USB vendor ID, for example 0xCAFE")
    parser.add_argument("--pid", type=lambda value: int(value, 0), default=DEFAULT_PID, help="USB product ID, for example 0x4003")
    parser.add_argument("--endpoint", type=lambda value: int(value, 0), default=DEFAULT_VENDOR_IN_ENDPOINT, help="Bulk IN endpoint address")
    parser.add_argument("--read-size", type=int, default=DEFAULT_READ_SIZE, help="Bytes to request per bulk read")
    parser.add_argument("--timeout-ms", type=int, default=DEFAULT_TIMEOUT_MS, help="Per-read timeout in milliseconds")
    return parser


def main() -> int:
    parser = _build_parser()
    args = parser.parse_args()

    try:
        stats = capture_bulk_stream(
            duration_seconds=args.duration,
            vid=args.vid,
            pid=args.pid,
            endpoint_address=args.endpoint,
            read_size=args.read_size,
            timeout_ms=args.timeout_ms,
        )
    except (RuntimeError, ValueError, usb.core.USBError) as exc:
        print(f"capture failed: {exc}", file=sys.stderr)
        return 1

    print(f"total bytes read: {stats.total_bytes}")
    print(f"average speed: {stats.bytes_per_second:.2f} B/s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())