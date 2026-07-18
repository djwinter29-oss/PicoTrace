from __future__ import annotations

import argparse
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from ..control import HidControlClient
from ..trace import SpiCaptureMode
from .control_ops import (
    _configure_i2c_channel,
    _configure_spi_channel,
    _disable_stream_best_effort,
    _print_configured_i2c_channel,
    _print_configured_spi_channel,
    _print_device_status,
    _stop_i2c_channel,
    _stop_spi_logical_channel,
    _with_control,
)
from .trace_stream import _stream_all_with_hooks, _stream_channel, _stream_channel_with_hooks


_MONITOR_START_GATE_TIMEOUT_SECONDS = 5.0
_MONITOR_READY_TIMEOUT_SECONDS = 5.0
_ALL_TRACE_CHANNEL = -1


@dataclass(frozen=True)
class _MonitorSession:
    process: subprocess.Popen[bytes] | None
    label: str
    channel: int
    stop: Callable[[], None] | None = None
    stop_config: _MonitorStopConfig | None = None
    spi_config: _SpiMonitorConfig | None = None


@dataclass(frozen=True)
class _MonitorStopConfig:
    kind: str
    channel: int


@dataclass(frozen=True)
class _SpiMonitorConfig:
    capture: SpiCaptureMode
    spi_mode: int
    timeout_us: int


def _supports_detached_monitors() -> bool:
    return sys.platform.startswith("win")


def _default_foreground() -> bool:
    return not _supports_detached_monitors()


def _spi_bus_for_logical_channel(logical_channel: int) -> int:
    return logical_channel // 3


def _spi_channel_select_mask(logical_channel: int) -> int:
    return 1 << (logical_channel % 3)


def _clone_spi_monitor_session(session: _MonitorSession, *, spi_config: _SpiMonitorConfig) -> _MonitorSession:
    return _MonitorSession(
        process=None,
        label=session.label,
        channel=session.channel,
        stop=session.stop,
        stop_config=session.stop_config,
        spi_config=spi_config,
    )


def _stop_spi_bus_group(sessions: Sequence[_MonitorSession]) -> None:
    if not sessions:
        return

    for session in sessions:
        if session.process is not None:
            _terminate_monitor_process(session.process)
    _run_stop_callback_best_effort(lambda: _stop_spi_logical_channel(sessions[0].channel))


class _MonitorManager:
    def __init__(self) -> None:
        self._processes: dict[int, _MonitorSession] = {}

    def start_monitor(
        self,
        channel: int,
        *,
        label: str,
        configure: Callable[[], None] | None = None,
        stop: Callable[[], None] | None = None,
        stop_config: _MonitorStopConfig | None = None,
    ) -> None:
        if not _supports_detached_monitors():
            print("detached monitor windows are unavailable on this platform; using foreground streaming in this console")
            if configure is not None:
                _run_foreground_configured_monitor(channel, configure, stop=stop)
            else:
                try:
                    _stream_channel(channel)
                finally:
                    if stop is not None:
                        _run_stop_callback_best_effort(stop)
            return

        old_session = self._processes.pop(channel, None)
        if old_session is not None:
            if old_session.spi_config is not None:
                bus = _spi_bus_for_logical_channel(old_session.channel)
                remaining_sessions = [
                    candidate for candidate in self._spi_bus_sessions(bus) if candidate.channel != channel
                ]
                self._restart_spi_bus_sessions(bus, remaining_sessions, stopped_channel=channel)
                return
            else:
                _stop_monitor_session(old_session)

        new_process = _start_detached_monitor(
            channel,
            label=label,
            configure=configure,
            old_process=None,
            stop=stop,
            stop_config=stop_config,
        )
        self._processes[channel] = _MonitorSession(
            process=new_process,
            label=label,
            channel=channel,
            stop=stop,
            stop_config=stop_config,
        )

    def start_spi_monitor(
        self,
        logical_channel: int,
        *,
        label: str,
        capture: SpiCaptureMode,
        spi_mode: int,
        timeout_us: int,
    ) -> None:
        if not _supports_detached_monitors():
            _configure_and_start_monitor(
                logical_channel,
                label=label,
                foreground=True,
                configure=lambda: _print_configured_spi_channel(
                    logical_channel,
                    capture=capture,
                    spi_mode=spi_mode,
                    timeout_us=timeout_us,
                ),
                stop=lambda: _stop_spi_logical_channel(logical_channel),
                stop_config=_MonitorStopConfig(kind="spi", channel=logical_channel),
            )
            return

        bus = _spi_bus_for_logical_channel(logical_channel)
        shared_config = _SpiMonitorConfig(
            capture=capture,
            spi_mode=spi_mode,
            timeout_us=timeout_us,
        )
        affected_sessions = self._spi_bus_sessions(bus)
        desired_sessions = [
            _clone_spi_monitor_session(session, spi_config=shared_config)
            for session in affected_sessions
            if session.channel != logical_channel
        ]
        desired_sessions.append(
            _MonitorSession(
                process=None,
                label=label,
                channel=logical_channel,
                stop=lambda: _stop_spi_logical_channel(logical_channel),
                stop_config=_MonitorStopConfig(kind="spi", channel=logical_channel),
                spi_config=shared_config,
            )
        )
        self._restart_spi_bus_sessions(bus, desired_sessions)

    def stop_monitor(self, channel: int) -> bool:
        session = self._processes.pop(channel, None)
        if session is None:
            return False

        if session.spi_config is not None:
            bus = _spi_bus_for_logical_channel(session.channel)
            remaining_sessions = [
                candidate for candidate in self._spi_bus_sessions(bus) if candidate.channel != channel
            ]
            self._processes[channel] = session
            self._restart_spi_bus_sessions(bus, remaining_sessions, stopped_channel=channel)
            return True

        _stop_monitor_session(session)
        return True

    def stop_all(self) -> None:
        sessions = list(self._processes.values())
        self._processes.clear()

        handled_spi_buses: set[int] = set()
        for session in sessions:
            if session.spi_config is None:
                _stop_monitor_session(session)
                continue

            bus = _spi_bus_for_logical_channel(session.channel)
            if bus in handled_spi_buses:
                continue

            handled_spi_buses.add(bus)
            _stop_spi_bus_group(
                [candidate for candidate in sessions if candidate.spi_config is not None and _spi_bus_for_logical_channel(candidate.channel) == bus]
            )

    def active_channels(self) -> tuple[int, ...]:
        stale_channels: list[int] = []
        for channel, session in self._processes.items():
            if (session.process is not None) and (session.process.poll() is not None):
                stale_channels.append(channel)

        stale_spi_buses = {
            _spi_bus_for_logical_channel(channel)
            for channel in stale_channels
            if (self._processes.get(channel) is not None) and (self._processes[channel].spi_config is not None)
        }
        for bus in stale_spi_buses:
            stale_bus_channels = {
                session.channel
                for session in self._spi_bus_sessions(bus)
                if (session.process is not None) and (session.process.poll() is not None)
            }
            remaining_sessions = [
                session for session in self._spi_bus_sessions(bus) if session.channel not in stale_bus_channels
            ]
            stopped_channel = min(stale_bus_channels) if stale_bus_channels else None
            self._restart_spi_bus_sessions(bus, remaining_sessions, stopped_channel=stopped_channel)

        for channel in stale_channels:
            if channel in self._processes and self._processes[channel].spi_config is None:
                stale_session = self._processes.pop(channel, None)
                if stale_session is not None:
                    _stop_monitor_session(stale_session)
        return tuple(sorted(self._processes))

    def _spi_bus_sessions(self, bus: int) -> list[_MonitorSession]:
        return [
            session
            for session in self._processes.values()
            if (session.spi_config is not None) and (_spi_bus_for_logical_channel(session.channel) == bus)
        ]

    def _restart_spi_bus_sessions(
        self,
        bus: int,
        desired_sessions: Sequence[_MonitorSession],
        *,
        stopped_channel: int | None = None,
    ) -> None:
        existing_sessions = self._spi_bus_sessions(bus)
        for session in existing_sessions:
            self._processes.pop(session.channel, None)
            if session.process is not None:
                _terminate_monitor_process(session.process)

        if stopped_channel is not None:
            _run_stop_callback_best_effort(lambda: _stop_spi_logical_channel(stopped_channel))

        if not desired_sessions:
            return

        _configure_spi_bus_sessions(desired_sessions)
        started_sessions: list[_MonitorSession] = []
        try:
            for session in desired_sessions:
                process = _start_detached_monitor(
                    session.channel,
                    label=session.label,
                    configure=None,
                    old_process=None,
                    stop=session.stop,
                    stop_config=session.stop_config,
                )
                started = _MonitorSession(
                    process=process,
                    label=session.label,
                    channel=session.channel,
                    stop=session.stop,
                    stop_config=session.stop_config,
                    spi_config=session.spi_config,
                )
                self._processes[session.channel] = started
                started_sessions.append(started)
        except Exception:
            for started in started_sessions:
                self._processes.pop(started.channel, None)
                if started.process is not None:
                    _terminate_monitor_process(started.process)
            _run_stop_callback_best_effort(lambda: _stop_spi_logical_channel(desired_sessions[0].channel))
            raise


def _create_monitor_start_gate() -> Path:
    handle = tempfile.NamedTemporaryFile(prefix="picotrace-monitor-", suffix=".gate", delete=False)
    handle.close()
    return Path(handle.name)


def _create_monitor_ready_gate() -> Path:
    handle = tempfile.NamedTemporaryFile(prefix="picotrace-monitor-", suffix=".ready", delete=False)
    handle.close()
    return Path(handle.name)


def _release_monitor_start_gate(start_gate: Path | None) -> None:
    if start_gate is None:
        return
    try:
        start_gate.unlink(missing_ok=True)
    except OSError:
        pass


def _mark_monitor_ready(ready_gate: str | None) -> None:
    if not ready_gate:
        return
    _release_monitor_start_gate(Path(ready_gate))


def _wait_for_monitor_start(start_gate: str | None) -> None:
    if not start_gate:
        return

    gate_path = Path(start_gate)
    deadline = time.monotonic() + _MONITOR_START_GATE_TIMEOUT_SECONDS
    while gate_path.exists():
        if time.monotonic() >= deadline:
            raise RuntimeError("monitor start gate timed out before the parent released the trace stream")
        time.sleep(0.05)


def _wait_for_monitor_ready(ready_gate: Path, process: subprocess.Popen[bytes]) -> None:
    deadline = time.monotonic() + _MONITOR_READY_TIMEOUT_SECONDS
    while ready_gate.exists():
        if process.poll() is not None:
            raise RuntimeError("monitor session exited before it reported ready")
        if time.monotonic() >= deadline:
            raise RuntimeError("monitor session did not report ready before timeout")
        time.sleep(0.05)


def _spawn_monitor_window(
    channel: int,
    *,
    label: str,
    start_gate: Path | None = None,
    ready_gate: Path | None = None,
    stop_config: _MonitorStopConfig | None = None,
) -> subprocess.Popen[bytes]:
    if not _supports_detached_monitors():
        raise RuntimeError("detached monitor windows are currently supported only on Windows")

    command = [
        sys.executable,
        "-m",
        "picotrace.app.cli",
        "_monitor",
        "--label",
        label,
    ]
    if channel == _ALL_TRACE_CHANNEL:
        command.append("--all")
    else:
        command.extend(["--channel", str(channel)])
    if start_gate is not None:
        command.extend(["--start-gate", str(start_gate)])
    if ready_gate is not None:
        command.extend(["--ready-gate", str(ready_gate)])
    if stop_config is not None:
        command.extend(["--stop-kind", stop_config.kind, "--stop-channel", str(stop_config.channel)])
    return subprocess.Popen(command, creationflags=subprocess.CREATE_NEW_CONSOLE)


def _terminate_monitor_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2.0)


def _stop_monitor_session(session: _MonitorSession) -> None:
    if session.process is not None:
        _terminate_monitor_process(session.process)
    if session.stop is not None:
        _run_stop_callback_best_effort(session.stop)


def _run_stop_callback_best_effort(stop: Callable[[], None]) -> None:
    try:
        stop()
    except Exception:
        pass


def _start_monitor(
    channel: int,
    *,
    label: str,
    foreground: bool,
    stop: Callable[[], None] | None = None,
    stop_config: _MonitorStopConfig | None = None,
) -> int:
    if foreground:
        try:
            if channel == _ALL_TRACE_CHANNEL:
                return _stream_all_with_hooks()
            return _stream_channel_with_hooks(channel)
        finally:
            if stop is not None:
                _run_stop_callback_best_effort(stop)

    _start_detached_monitor(
        channel,
        label=label,
        configure=None,
        old_process=None,
        stop=stop,
        stop_config=stop_config,
    )
    if channel == _ALL_TRACE_CHANNEL:
        print(f"started monitor window for all trace traffic ({label})")
    else:
        print(f"started monitor window for channel {channel} ({label})")
    return 0


def _configure_spi_bus_sessions(sessions: Sequence[_MonitorSession]) -> None:
    if not sessions:
        return

    spi_sessions = [session for session in sessions if session.spi_config is not None]
    if len(spi_sessions) != len(sessions):
        raise RuntimeError("SPI bus restart expected only SPI monitor sessions")

    shared_config = spi_sessions[0].spi_config
    assert shared_config is not None
    bus = _spi_bus_for_logical_channel(spi_sessions[0].channel)
    channel_select_mask = 0
    for session in spi_sessions:
        if _spi_bus_for_logical_channel(session.channel) != bus:
            raise RuntimeError("SPI bus restart sessions must share the same owning bus")
        if session.spi_config != shared_config:
            raise RuntimeError(
                "SPI monitor sessions on the same bus must share capture, SPI mode, and timeout settings"
            )
        channel_select_mask |= _spi_channel_select_mask(session.channel)

    with HidControlClient.open() as control:
        control.spi_set_config(
            bus,
            capture=shared_config.capture,
            spi_mode=shared_config.spi_mode,
            channel_select_mask=channel_select_mask,
            timeout_us=shared_config.timeout_us,
        )
        control.set_stream_enabled(True)


def _stop_monitor_best_effort(stop_config: _MonitorStopConfig | None) -> None:
    if stop_config is None:
        return

    try:
        if stop_config.kind == "i2c":
            _stop_i2c_channel(stop_config.channel)
        elif stop_config.kind == "spi":
            _stop_spi_logical_channel(stop_config.channel)
    except Exception:
        pass


def _run_foreground_configured_monitor(
    channel: int,
    configure: Callable[[], None],
    *,
    stop: Callable[[], None] | None = None,
) -> int:
    started = False

    def mark_started() -> None:
        nonlocal started
        started = True

    try:
        configure()
        return _stream_channel_with_hooks(channel, on_started=mark_started)
    except Exception:
        if not started:
            if stop is not None:
                _run_stop_callback_best_effort(stop)
            else:
                _disable_stream_best_effort()
        raise
    finally:
        if started and stop is not None:
            _run_stop_callback_best_effort(stop)


def _start_detached_monitor(
    channel: int,
    *,
    label: str,
    configure: Callable[[], None] | None,
    old_process: subprocess.Popen[bytes] | None,
    stop: Callable[[], None] | None,
    stop_config: _MonitorStopConfig | None,
) -> subprocess.Popen[bytes]:
    start_gate = _create_monitor_start_gate()
    ready_gate = _create_monitor_ready_gate()
    process: subprocess.Popen[bytes] | None = None
    try:
        process = _spawn_monitor_window(
            channel,
            label=label,
            start_gate=start_gate,
            ready_gate=ready_gate,
            stop_config=stop_config,
        )
        if old_process is not None:
            _terminate_monitor_process(old_process)
        if configure is not None:
            configure()
        _release_monitor_start_gate(start_gate)
        _wait_for_monitor_ready(ready_gate, process)
        return process
    except Exception:
        _release_monitor_start_gate(start_gate)
        _release_monitor_start_gate(ready_gate)
        if process is not None:
            _terminate_monitor_process(process)
        if stop is not None:
            _run_stop_callback_best_effort(stop)
        raise
    finally:
        _release_monitor_start_gate(ready_gate)


def _configure_and_start_monitor(
    channel: int,
    *,
    label: str,
    foreground: bool,
    configure: Callable[[], None],
    stop: Callable[[], None] | None = None,
    stop_config: _MonitorStopConfig | None = None,
) -> int:
    if foreground:
        return _run_foreground_configured_monitor(channel, configure, stop=stop)

    _start_detached_monitor(
        channel,
        label=label,
        configure=configure,
        old_process=None,
        stop=stop,
        stop_config=stop_config,
    )
    print(f"started monitor window for channel {channel} ({label})")
    return 0


def _run_status(_: argparse.Namespace) -> int:
    return _with_control(_print_device_status)


def _run_stream(args: argparse.Namespace) -> int:
    if getattr(args, "all", False) is True:
        label = getattr(args, "label", "all trace traffic")
        return _start_monitor(_ALL_TRACE_CHANNEL, label=label, foreground=args.foreground)

    label = getattr(args, "label", f"trace channel {args.channel}")
    return _start_monitor(args.channel, label=label, foreground=args.foreground)


def _run_stream_state(args: argparse.Namespace) -> int:
    enabled = args.state == "on"
    return _with_control(lambda control: control.set_stream_enabled(enabled))


def _run_led(args: argparse.Namespace) -> int:
    on = args.state == "on"
    return _with_control(lambda control: control.set_led(on))


def _run_reboot(args: argparse.Namespace) -> int:
    if not args.yes:
        answer = input("reboot the PicoTrace device? [y/N]: ").strip().lower()
        if answer not in {"y", "yes"}:
            print("reboot cancelled")
            return 1
    return _with_control(lambda control: control.reboot())


def _run_i2c(args: argparse.Namespace) -> int:
    channel = args.channel
    sample_hz = args.sample_hz
    stop = lambda: _stop_i2c_channel(channel)
    stop_config = _MonitorStopConfig(kind="i2c", channel=channel)
    if args.no_stream:
        _print_configured_i2c_channel(channel, sample_hz)
        return 0

    def configure() -> None:
        _print_configured_i2c_channel(channel, sample_hz)

    return _configure_and_start_monitor(
        channel,
        label=f"i2c channel {channel}",
        foreground=args.foreground,
        configure=configure,
        stop=stop,
        stop_config=stop_config,
    )


def _run_spi(args: argparse.Namespace) -> int:
    logical_channel = args.channel
    capture = SpiCaptureMode[args.capture]
    stop = lambda: _stop_spi_logical_channel(logical_channel)
    stop_config = _MonitorStopConfig(kind="spi", channel=logical_channel)
    if args.no_stream:
        _print_configured_spi_channel(
            logical_channel,
            capture=capture,
            spi_mode=args.spi_mode,
            timeout_us=args.timeout_us,
        )
        return 0

    def configure() -> None:
        _print_configured_spi_channel(
            logical_channel,
            capture=capture,
            spi_mode=args.spi_mode,
            timeout_us=args.timeout_us,
        )

    return _configure_and_start_monitor(
        logical_channel,
        label=f"spi logical channel {logical_channel}",
        foreground=args.foreground,
        configure=configure,
        stop=stop,
        stop_config=stop_config,
    )


def _parse_monitor_stop_config(stop_kind: str | None, stop_channel: int | None) -> _MonitorStopConfig | None:
    if stop_kind is None:
        return None
    if stop_channel is None:
        raise RuntimeError("monitor stop channel is required when a stop kind is provided")
    return _MonitorStopConfig(kind=stop_kind, channel=stop_channel)


def _run_monitor(args: argparse.Namespace) -> int:
    _wait_for_monitor_start(args.start_gate)
    ready_gate = args.ready_gate
    started = False
    stop_config = _parse_monitor_stop_config(args.stop_kind, args.stop_channel)

    def mark_started() -> None:
        nonlocal started
        started = True
        _mark_monitor_ready(ready_gate)

    if getattr(args, "all", False) is True:
        print(f"monitor window active for all trace traffic ({args.label}); close this window or press Ctrl+C to stop")
    else:
        print(f"monitor window active for channel {args.channel} ({args.label}); close this window or press Ctrl+C to stop")
    try:
        if getattr(args, "all", False) is True:
            return _stream_all_with_hooks(on_started=mark_started)
        return _stream_channel_with_hooks(args.channel, on_started=mark_started)
    finally:
        if not started:
            _mark_monitor_ready(ready_gate)
        _stop_monitor_best_effort(stop_config)


def _prompt_int(prompt: str, *, minimum: int, maximum: int) -> int:
    while True:
        raw = input(prompt).strip()
        try:
            value = int(raw)
        except ValueError:
            print("enter a decimal integer")
            continue
        if minimum <= value <= maximum:
            return value
        print(f"enter a value between {minimum} and {maximum}")


def _interactive_mode() -> int:
    manager = _MonitorManager()
    try:
        while True:
            active_channels = manager.active_channels()
            print()
            print("PicoTrace CLI")
            if active_channels:
                print("active monitor channels:", ", ".join(str(channel) for channel in active_channels))
            else:
                print("active monitor channels: none")
            print("1. status")
            print("2. stream on")
            print("3. stream off")
            print("4. led on")
            print("5. led off")
            print("6. configure i2c channel and stream")
            print("7. configure spi channel and stream")
            print("8. reboot")
            print("9. stream existing channel")
            print("10. stop monitor channel")
            print("0. exit")

            choice = input("> ").strip()
            if choice == "0":
                return 0
            if choice == "1":
                _run_status(argparse.Namespace())
                continue
            if choice == "2":
                _run_stream_state(argparse.Namespace(state="on"))
                continue
            if choice == "3":
                _run_stream_state(argparse.Namespace(state="off"))
                continue
            if choice == "4":
                _run_led(argparse.Namespace(state="on"))
                continue
            if choice == "5":
                _run_led(argparse.Namespace(state="off"))
                continue
            if choice == "6":
                channel = _prompt_int("i2c channel [0-3]: ", minimum=0, maximum=3)
                sample_hz = _prompt_int("sample_hz: ", minimum=1, maximum=0xFFFFFFFF)
                manager.start_monitor(
                    channel,
                    label=f"i2c channel {channel}",
                    configure=lambda: _print_configured_i2c_channel(channel, sample_hz),
                    stop=lambda: _stop_i2c_channel(channel),
                    stop_config=_MonitorStopConfig(kind="i2c", channel=channel),
                )
                continue
            if choice == "7":
                logical_channel = _prompt_int("spi logical channel [0-5]: ", minimum=0, maximum=5)
                capture_index = _prompt_int("capture 1=MOSI 2=MOSI_MISO: ", minimum=1, maximum=2)
                spi_mode = _prompt_int("spi_mode [0-3]: ", minimum=0, maximum=3)
                timeout_us = _prompt_int("timeout_us: ", minimum=1, maximum=0xFFFFFFFF)
                capture = SpiCaptureMode.MOSI if capture_index == 1 else SpiCaptureMode.MOSI_MISO
                manager.start_spi_monitor(
                    logical_channel,
                    label=f"spi logical channel {logical_channel}",
                    capture=capture,
                    spi_mode=spi_mode,
                    timeout_us=timeout_us,
                )
                continue
            if choice == "8":
                _run_reboot(argparse.Namespace(yes=False))
                continue
            if choice == "9":
                channel = _prompt_int("logical trace channel: ", minimum=0, maximum=255)
                manager.start_monitor(channel, label=f"trace channel {channel}")
                continue
            if choice == "10":
                channel = _prompt_int("stop logical trace channel: ", minimum=0, maximum=255)
                if manager.stop_monitor(channel):
                    print(f"stopped monitor window for channel {channel}")
                else:
                    print(f"no monitor window for channel {channel}")
                continue
            print("unknown selection")
    finally:
        manager.stop_all()


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="PicoTrace control and trace CLI")
    subparsers = parser.add_subparsers(dest="command")

    status_parser = subparsers.add_parser("status", help="Print shared device, I2C, and SPI status")
    status_parser.set_defaults(func=_run_status)

    stream_state_parser = subparsers.add_parser("stream", help="Enable or disable device streaming")
    stream_state_parser.add_argument("state", choices=["on", "off"])
    stream_state_parser.set_defaults(func=_run_stream_state)

    led_parser = subparsers.add_parser("led", help="Turn the device LED on or off")
    led_parser.add_argument("state", choices=["on", "off"])
    led_parser.set_defaults(func=_run_led)

    reboot_parser = subparsers.add_parser("reboot", help="Reboot the device")
    reboot_parser.add_argument("--yes", action="store_true", help="Skip the reboot confirmation prompt")
    reboot_parser.set_defaults(func=_run_reboot)

    i2c_parser = subparsers.add_parser("i2c", help="Configure one I2C channel and optionally stream it")
    i2c_parser.add_argument("--channel", type=int, required=True)
    i2c_parser.add_argument("--sample-hz", type=int, required=True)
    i2c_parser.add_argument(
        "--foreground",
        action="store_true",
        default=_default_foreground(),
        help="Stream in the current console instead of a new monitor window",
    )
    i2c_parser.add_argument("--no-stream", action="store_true", help="Configure the device without starting the trace stream")
    i2c_parser.set_defaults(func=_run_i2c)

    spi_parser = subparsers.add_parser("spi", help="Configure one SPI logical channel and optionally stream it")
    spi_parser.add_argument("--channel", type=int, required=True)
    spi_parser.add_argument("--capture", choices=[mode.name for mode in SpiCaptureMode], default=SpiCaptureMode.MOSI_MISO.name)
    spi_parser.add_argument("--spi-mode", type=int, choices=[0, 1, 2, 3], default=0)
    spi_parser.add_argument("--timeout-us", type=int, default=100)
    spi_parser.add_argument(
        "--foreground",
        action="store_true",
        default=_default_foreground(),
        help="Stream in the current console instead of a new monitor window",
    )
    spi_parser.add_argument("--no-stream", action="store_true", help="Configure the device without starting the trace stream")
    spi_parser.set_defaults(func=_run_spi)

    trace_parser = subparsers.add_parser("trace", help="Stream one logical channel or all traffic without changing device configuration")
    trace_group = trace_parser.add_mutually_exclusive_group(required=True)
    trace_group.add_argument("--channel", type=int)
    trace_group.add_argument("--all", action="store_true", help="Stream all decoded trace traffic without a channel filter")
    trace_parser.add_argument(
        "--foreground",
        action="store_true",
        default=_default_foreground(),
        help="Stream in the current console instead of a new monitor window",
    )
    trace_parser.set_defaults(func=_run_stream)

    return parser


def _build_monitor_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(add_help=False)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--channel", type=int)
    group.add_argument("--all", action="store_true")
    parser.add_argument("--label", default="trace channel")
    parser.add_argument("--start-gate")
    parser.add_argument("--ready-gate")
    parser.add_argument("--stop-kind", choices=["i2c", "spi"])
    parser.add_argument("--stop-channel", type=int)
    parser.set_defaults(func=_run_monitor)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    argv_list = list(argv) if argv is not None else sys.argv[1:]
    try:
        if argv_list and argv_list[0] == "_monitor":
            parser = _build_monitor_parser()
            args = parser.parse_args(argv_list[1:])
            return args.func(args)

        parser = _build_parser()
        args = parser.parse_args(argv_list)
        if args.command is None:
            return _interactive_mode()
        return args.func(args)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())