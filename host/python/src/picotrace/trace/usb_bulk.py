from __future__ import annotations

"""PyUSB transport for reading PicoTrace packets from the current USB bulk IN path."""

import array
from collections.abc import Callable
import os
from pathlib import Path
import platform
import queue
import sys
import threading
import time

import usb.backend.libusb1
import usb.core
import usb.util

from .decode import TracePacket, TraceStreamDecoder
from .filter import TraceChannelRegistry


DEFAULT_VID = 0xCAFE
DEFAULT_PID = 0x4003
DEFAULT_VENDOR_IN_ENDPOINT = 0x83
DEFAULT_DURATION_SECONDS = 20.0
DEFAULT_READ_SIZE = 65536
DEFAULT_TIMEOUT_MS = 250
_TRACE_READER_BUFFER_COUNT = 4

_TRACE_READER_DONE = object()


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


def find_usb_device(vid: int, pid: int) -> usb.core.Device:
    device = usb.core.find(idVendor=vid, idProduct=pid, backend=_get_libusb_backend())
    if device is None:
        raise RuntimeError(f"Device {vid:04x}:{pid:04x} not found")
    return device


def open_trace_device(
    vid: int = DEFAULT_VID,
    pid: int = DEFAULT_PID,
    endpoint_address: int = DEFAULT_VENDOR_IN_ENDPOINT,
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
        for endpoint in candidate_interface:
            if endpoint.bEndpointAddress == endpoint_address:
                interface = candidate_interface
                break
        if interface is not None:
            break

    if interface is None:
        raise RuntimeError(f"Endpoint 0x{endpoint_address:02x} not found on the active USB configuration")

    interface_number = int(interface.bInterfaceNumber)
    alternate_setting = int(interface.bAlternateSetting)

    if sys.platform.startswith("linux"):
        try:
            if device.is_kernel_driver_active(interface_number):
                device.detach_kernel_driver(interface_number)
        except (NotImplementedError, usb.core.USBError):
            pass

    try:
        usb.util.claim_interface(device, interface_number)
    except usb.core.USBError as exc:
        error_text = str(exc).lower()
        if exc.errno == 16 or "busy" in error_text:
            raise RuntimeError(
                "Trace bulk interface is busy. Stop any other running 'picotrace ... trace' process and retry."
            ) from exc
        raise
    if alternate_setting != 0:
        device.set_interface_altsetting(interface=interface_number, alternate_setting=alternate_setting)
    return device, interface_number


def close_trace_device(device: usb.core.Device, interface_number: int | None) -> None:
    try:
        if interface_number is not None:
            usb.util.release_interface(device, interface_number)
        usb.util.dispose_resources(device)
    except usb.core.USBError:
        pass


def _bulk_read_chunks(
    backend,
    handle,
    endpoint_address: int,
    interface_number: int,
    read_size: int,
    timeout_ms: int,
    stop_event: threading.Event,
    free_buffers: queue.SimpleQueue[array.array],
    chunk_queue: queue.SimpleQueue[tuple[array.array, int] | object],
    error_box: list[BaseException],
) -> None:
    try:
        while not stop_event.is_set():
            try:
                read_buffer = free_buffers.get_nowait()
            except queue.Empty:
                read_buffer = array.array("B", [0]) * read_size

            try:
                transferred = backend.bulk_read(handle, endpoint_address, interface_number, read_buffer, timeout_ms)
            except usb.core.USBTimeoutError:
                free_buffers.put(read_buffer)
                continue

            if transferred <= 0:
                free_buffers.put(read_buffer)
                continue

            chunk_queue.put((read_buffer, transferred))
    except BaseException as exc:
        error_box.append(exc)
    finally:
        chunk_queue.put(_TRACE_READER_DONE)


def iter_trace_packets(
    duration_seconds: float | None = DEFAULT_DURATION_SECONDS,
    vid: int = DEFAULT_VID,
    pid: int = DEFAULT_PID,
    endpoint_address: int = DEFAULT_VENDOR_IN_ENDPOINT,
    read_size: int = DEFAULT_READ_SIZE,
    timeout_ms: int = DEFAULT_TIMEOUT_MS,
    channel_registry: TraceChannelRegistry | None = None,
    keep_running: Callable[[], bool] | None = None,
    on_opened: Callable[[], None] | None = None,
):
    if duration_seconds is not None and duration_seconds <= 0.0:
        raise ValueError("duration_seconds must be positive")

    if read_size <= 0:
        raise ValueError("read_size must be positive")

    device, interface_number = open_trace_device(vid=vid, pid=pid, endpoint_address=endpoint_address)
    backend = device._ctx.backend
    handle = device._ctx.handle
    free_buffers: queue.SimpleQueue[array.array] = queue.SimpleQueue()
    chunk_queue: queue.SimpleQueue[tuple[array.array, int] | object] = queue.SimpleQueue()
    decoder = TraceStreamDecoder()
    deadline = None if duration_seconds is None else time.perf_counter() + duration_seconds
    stop_event = threading.Event()
    reader_errors: list[BaseException] = []

    for _ in range(_TRACE_READER_BUFFER_COUNT):
        free_buffers.put(array.array("B", [0]) * read_size)

    reader_thread = threading.Thread(
        target=_bulk_read_chunks,
        args=(
            backend,
            handle,
            endpoint_address,
            interface_number,
            read_size,
            timeout_ms,
            stop_event,
            free_buffers,
            chunk_queue,
            reader_errors,
        ),
        daemon=True,
    )
    reader_thread.start()
    if on_opened is not None:
        on_opened()

    try:
        while True:
            if keep_running is not None and not keep_running():
                break
            if deadline is not None and time.perf_counter() >= deadline:
                break

            try:
                chunk = chunk_queue.get(timeout=timeout_ms / 1000.0)
            except queue.Empty:
                continue

            if chunk is _TRACE_READER_DONE:
                if reader_errors:
                    raise reader_errors[0]
                break

            read_buffer, transferred = chunk
            for packet in decoder.append(memoryview(read_buffer)[:transferred]):
                if channel_registry is not None and not channel_registry.matches_packet(packet):
                    continue
                yield packet
            free_buffers.put(read_buffer)
    finally:
        stop_event.set()
        reader_thread.join(timeout=(timeout_ms / 1000.0) + 1.0)
        close_trace_device(device, interface_number)


def read_trace_packets(
    duration_seconds: float | None = DEFAULT_DURATION_SECONDS,
    vid: int = DEFAULT_VID,
    pid: int = DEFAULT_PID,
    endpoint_address: int = DEFAULT_VENDOR_IN_ENDPOINT,
    read_size: int = DEFAULT_READ_SIZE,
    timeout_ms: int = DEFAULT_TIMEOUT_MS,
    channel_registry: TraceChannelRegistry | None = None,
    keep_running: Callable[[], bool] | None = None,
    on_opened: Callable[[], None] | None = None,
) -> list[TracePacket]:
    return list(
        iter_trace_packets(
            duration_seconds=duration_seconds,
            vid=vid,
            pid=pid,
            endpoint_address=endpoint_address,
            read_size=read_size,
            timeout_ms=timeout_ms,
            channel_registry=channel_registry,
            keep_running=keep_running,
            on_opened=on_opened,
        )
    )