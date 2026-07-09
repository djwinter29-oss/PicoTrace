from __future__ import annotations

from collections.abc import Callable

from ..control import HidControlClient
from ..trace import SpiCaptureMode


def _with_control(operation: Callable[[HidControlClient], None]) -> int:
    with HidControlClient.open() as control:
        operation(control)
    return 0


def _print_device_status(control: HidControlClient) -> None:
    status = control.get_status()
    print(f"stream_enabled={status.stream_enabled}")

    print("i2c_status:")
    for channel_status in control.i2c_get_all_status():
        print(
            "  "
            f"channel={channel_status.channel} initialized={channel_status.initialized} "
            f"running={channel_status.running} overrun={channel_status.overrun} "
            f"sample_hz={channel_status.sample_hz} overrun_count={channel_status.overrun_count} "
            f"transition_pending={channel_status.transition_pending} transition_reason={channel_status.transition_reason}"
        )

    print("spi_status:")
    for bus_status in control.spi_get_all_status():
        print(
            "  "
            f"bus={bus_status.bus} initialized={bus_status.initialized} running={bus_status.running} "
            f"capture={bus_status.capture.name} spi_mode={bus_status.spi_mode} "
            f"timeout_us={bus_status.timeout_us} overrun={bus_status.overrun}"
        )


def _disable_stream_best_effort() -> None:
    try:
        with HidControlClient.open() as control:
            control.set_stream_enabled(False)
    except Exception:
        pass


def _stop_i2c_channel(channel: int) -> None:
    with HidControlClient.open() as control:
        control.i2c_set_rate(channel, 0)


def _stop_spi_logical_channel(logical_channel: int) -> None:
    bus = logical_channel // 3
    with HidControlClient.open() as control:
        control.spi_set_config(
            bus,
            capture=SpiCaptureMode.DISABLED,
            spi_mode=0,
            channel_select_mask=0,
            timeout_us=0,
        )


def _configure_i2c_channel(channel: int, sample_hz: int) -> None:
    def action(control: HidControlClient) -> None:
        control.i2c_set_rate(channel, sample_hz)
        control.set_stream_enabled(True)

    _with_control(action)


def _print_configured_i2c_channel(channel: int, sample_hz: int) -> None:
    _configure_i2c_channel(channel, sample_hz)
    print(f"configured i2c channel {channel} at {sample_hz} Hz")


def _configure_spi_channel(
    logical_channel: int,
    *,
    capture: SpiCaptureMode,
    spi_mode: int,
    timeout_us: int,
) -> tuple[int, int]:
    bus = logical_channel // 3
    slot = logical_channel % 3
    channel_select_mask = 1 << slot

    def action(control: HidControlClient) -> None:
        control.spi_set_config(
            bus,
            capture=capture,
            spi_mode=spi_mode,
            channel_select_mask=channel_select_mask,
            timeout_us=timeout_us,
        )
        control.set_stream_enabled(True)

    _with_control(action)
    return bus, channel_select_mask


def _print_configured_spi_channel(
    logical_channel: int,
    *,
    capture: SpiCaptureMode,
    spi_mode: int,
    timeout_us: int,
) -> None:
    bus, channel_select_mask = _configure_spi_channel(
        logical_channel,
        capture=capture,
        spi_mode=spi_mode,
        timeout_us=timeout_us,
    )
    print(
        f"configured spi logical channel {logical_channel} on bus {bus} "
        f"capture={capture.name} mode={spi_mode} mask=0x{channel_select_mask:02X} timeout_us={timeout_us}"
    )
