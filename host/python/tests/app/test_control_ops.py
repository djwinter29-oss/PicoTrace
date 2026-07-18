from __future__ import annotations

import unittest
from unittest import mock

from picotrace.app import control_ops
from picotrace.trace import SpiCaptureMode


class ControlOpsTests(unittest.TestCase):
    def test_disable_stream_best_effort_swallows_control_open_failure(self) -> None:
        with mock.patch("picotrace.app.control_ops.HidControlClient.open", side_effect=RuntimeError("usb missing")):
            control_ops._disable_stream_best_effort()

    def test_stop_i2c_channel_sends_zero_sample_rate(self) -> None:
        control = mock.Mock()
        control_context = mock.MagicMock()
        control_context.__enter__.return_value = control
        control_context.__exit__.return_value = None

        with mock.patch("picotrace.app.control_ops.HidControlClient.open", return_value=control_context):
            control_ops._stop_i2c_channel(2)

        control.i2c_set_rate.assert_called_once_with(2, 0)

    def test_stop_spi_logical_channel_disables_owning_bus(self) -> None:
        control = mock.Mock()
        control_context = mock.MagicMock()
        control_context.__enter__.return_value = control
        control_context.__exit__.return_value = None

        with mock.patch("picotrace.app.control_ops.HidControlClient.open", return_value=control_context):
            control_ops._stop_spi_logical_channel(4)

        control.spi_set_config.assert_called_once_with(
            1,
            capture=SpiCaptureMode.DISABLED,
            spi_mode=0,
            channel_select_mask=0,
            timeout_us=0,
        )

    def test_configure_spi_channel_sets_stream_enabled(self) -> None:
        control = mock.Mock()

        with mock.patch("picotrace.app.control_ops._with_control", side_effect=lambda operation: operation(control) or 0):
            bus, channel_select_mask = control_ops._configure_spi_channel(
                5,
                capture=SpiCaptureMode.MOSI_MISO,
                spi_mode=3,
                timeout_us=250,
            )

        self.assertEqual(bus, 1)
        self.assertEqual(channel_select_mask, 0x04)
        control.spi_set_config.assert_called_once_with(
            1,
            capture=SpiCaptureMode.MOSI_MISO,
            spi_mode=3,
            channel_select_mask=0x04,
            timeout_us=250,
        )
        control.set_stream_enabled.assert_called_once_with(True)
        self.assertLess(
            control.mock_calls.index(mock.call.set_stream_enabled(True)),
            control.mock_calls.index(
                mock.call.spi_set_config(
                    1,
                    capture=SpiCaptureMode.MOSI_MISO,
                    spi_mode=3,
                    channel_select_mask=0x04,
                    timeout_us=250,
                )
            ),
        )