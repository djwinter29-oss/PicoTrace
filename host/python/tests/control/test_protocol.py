from __future__ import annotations

import unittest

from picotrace.control.protocol import (
    HidDeviceStatus,
    SpiCaptureMode,
    build_i2c_set_rate_payload,
    build_spi_set_config_payload,
    decode_device_status_payload,
    decode_i2c_monitor_all_status_payload,
    decode_i2c_monitor_status_payload,
    decode_spi_monitor_all_status_payload,
    decode_spi_monitor_status_payload,
)


class HidProtocolTests(unittest.TestCase):
    def test_decode_device_status_payload_accepts_legacy_stream_only_status(self) -> None:
        status = decode_device_status_payload(b"\x01")

        self.assertEqual(status, HidDeviceStatus(stream_enabled=True, firmware_version=""))

    def test_decode_device_status_payload_decodes_firmware_version(self) -> None:
        status = decode_device_status_payload(b"\x01\x06v1.2.3")

        self.assertEqual(status, HidDeviceStatus(stream_enabled=True, firmware_version="v1.2.3"))

    def test_build_i2c_set_rate_payload_uses_little_endian_sample_rate(self) -> None:
        payload = build_i2c_set_rate_payload(2, 1_000_000)

        self.assertEqual(payload, b"\x02@B\x0F\x00")

    def test_decode_i2c_monitor_status_payload(self) -> None:
        payload = (
            b"\x01\x01\x01\x00"
            b"\x40\x42\x0F\x00"
            b"\x03\x00\x00\x00"
            b"\x07\x00\x00\x00"
            b"\x01\x82"
        )

        status = decode_i2c_monitor_status_payload(payload)

        self.assertEqual(status.channel, 1)
        self.assertTrue(status.initialized)
        self.assertTrue(status.running)
        self.assertFalse(status.overrun)
        self.assertEqual(status.sample_hz, 1_000_000)
        self.assertEqual(status.completed_buffers, 3)
        self.assertEqual(status.overrun_count, 7)
        self.assertTrue(status.transition_pending)
        self.assertEqual(status.transition_reason, 0x82)

    def test_decode_i2c_monitor_all_status_payload(self) -> None:
        payload = (
            b"\x00\x01\x00\x00\x80\x96\x98\x00\x02\x00\x00\x00\x00\x00"
            b"\x01\x01\x01\x01\x40\x42\x0F\x00\x03\x00\x00\x00\x01\x81"
        )

        statuses = decode_i2c_monitor_all_status_payload(payload)

        self.assertEqual(len(statuses), 2)
        self.assertEqual(statuses[0].sample_hz, 10_000_000)
        self.assertTrue(statuses[1].overrun)
        self.assertEqual(statuses[1].transition_reason, 0x81)

    def test_build_spi_set_config_payload_uses_expected_layout(self) -> None:
        payload = build_spi_set_config_payload(
            1,
            capture=SpiCaptureMode.MOSI_MISO,
            spi_mode=3,
            channel_select_mask=0x0F,
            timeout_us=250,
        )

        self.assertEqual(payload, b"\x01\x02\x03\x0F\xFA\x00\x00\x00")

    def test_decode_spi_monitor_status_payload(self) -> None:
        payload = (
            b"\x01\x01\x01\x02\x03\x0F"
            b"\xFA\x00\x00\x00"
            b"\x11\x00\x00\x00"
            b"\x02\x00\x00\x00"
            b"\x03\x00\x00\x00"
            b"\x04\x00\x00\x00"
            b"\x05\x00\x00\x00"
            b"\x06\x00\x00\x00"
            b"\x07\x00\x00\x00"
        )

        status = decode_spi_monitor_status_payload(payload)

        self.assertEqual(status.bus, 1)
        self.assertEqual(status.capture, SpiCaptureMode.MOSI_MISO)
        self.assertEqual(status.spi_mode, 3)
        self.assertEqual(status.channel_select_mask, 0x0F)
        self.assertEqual(status.timeout_us, 250)
        self.assertEqual(status.packets_emitted, 17)
        self.assertEqual(status.overrun_count, 2)
        self.assertEqual(status.sink_overrun_count, 3)
        self.assertEqual(status.sampler_overrun_count, 4)
        self.assertEqual(status.ring_drop_count, 5)
        self.assertEqual(status.usb_stall_count, 6)
        self.assertEqual(status.peak_ring_depth_packets, 7)

    def test_decode_spi_monitor_all_status_payload(self) -> None:
        payload = (
            b"\x00\x01\x01\x01\x00\x64\x00\x00\x00\x00"
            b"\x01\x01\x00\x02\x03\xFA\x00\x00\x00\x01"
        )

        statuses = decode_spi_monitor_all_status_payload(payload)

        self.assertEqual(len(statuses), 2)
        self.assertEqual(statuses[0].capture, SpiCaptureMode.MOSI)
        self.assertEqual(statuses[1].capture, SpiCaptureMode.MOSI_MISO)
        self.assertEqual(statuses[1].timeout_us, 250)
        self.assertTrue(statuses[1].overrun)