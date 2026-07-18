from __future__ import annotations

import unittest
from unittest import mock

from picotrace.app import trace_stream
from picotrace.trace import TraceFlags, TracePacket, TracePacketHeader, TraceType


class TraceStreamTests(unittest.TestCase):
    def test_format_trace_packet_formats_i2c_events(self) -> None:
        packet = TracePacket(
            header=TracePacketHeader(
                version=1,
                type=int(TraceType.I2C),
                channel=2,
                flags=0,
                payload_len=0,
                meta=0,
                sequence=7,
                timestamp_us=123,
            ),
            payload=b"",
        )
        event = mock.Mock()
        event.event_type.name = "START"
        event.value = 0x55

        with mock.patch("picotrace.app.trace_stream.decode_i2c_events", return_value=(event,)):
            rendered = trace_stream._format_trace_packet(packet)

        self.assertIn("seq=     7 I2C CH2: START", rendered)

    def test_format_trace_packet_formats_spi_bytes(self) -> None:
        packet = TracePacket(
            header=TracePacketHeader(
                version=1,
                type=int(TraceType.SPI),
                channel=0,
                flags=0,
                payload_len=2,
                meta=1,
                sequence=5,
                timestamp_us=123,
            ),
            payload=bytes([0x01, 0x02]),
        )

        rendered = trace_stream._format_trace_packet(packet)

        self.assertIn("seq=     5 SPI CH0 MOSI: 01 02", rendered)

    def test_stream_channel_with_hooks_passes_open_callback_and_prints_packets(self) -> None:
        packet = mock.Mock()
        packet_render = "packet-line"
        started = mock.Mock()

        def iter_packets(*, on_opened=None, duration_seconds=None, **_kwargs):
            self.assertEqual(duration_seconds, 24.0 * 60.0 * 60.0)
            if on_opened is not None:
                on_opened()
            return [packet]

        with mock.patch("picotrace.app.trace_stream.iter_trace_packets", side_effect=iter_packets), mock.patch(
            "picotrace.app.trace_stream._format_trace_packet", return_value=packet_render
        ), mock.patch("builtins.print") as print_mock:
            exit_code = trace_stream._stream_channel_with_hooks(4, on_started=started)

        self.assertEqual(exit_code, 0)
        started.assert_called_once_with()
        print_mock.assert_any_call("streaming channel 4; press Ctrl+C to stop")
        print_mock.assert_any_call(packet_render, flush=True)

    def test_stream_channel_with_hooks_handles_keyboard_interrupt(self) -> None:
        with mock.patch("picotrace.app.trace_stream.iter_trace_packets", side_effect=KeyboardInterrupt), mock.patch(
            "builtins.print"
        ) as print_mock:
            exit_code = trace_stream._stream_channel_with_hooks(6)

        self.assertEqual(exit_code, 0)
        print_mock.assert_any_call("stream stopped")

    def test_stream_all_with_hooks_passes_open_callback_and_prints_packets(self) -> None:
        packet = mock.Mock()
        packet_render = "packet-line"
        started = mock.Mock()

        def iter_packets(*, on_opened=None, channel_registry=None, duration_seconds=None, **_kwargs):
            self.assertEqual(channel_registry.registered_channels, frozenset())
            self.assertIsNone(duration_seconds)
            if on_opened is not None:
                on_opened()
            return [packet]

        with mock.patch("picotrace.app.trace_stream.iter_trace_packets", side_effect=iter_packets), mock.patch(
            "picotrace.app.trace_stream._format_trace_packet", return_value=packet_render
        ), mock.patch("builtins.print") as print_mock:
            exit_code = trace_stream._stream_all_with_hooks(on_started=started)

        self.assertEqual(exit_code, 0)
        started.assert_called_once_with()
        print_mock.assert_any_call("streaming all trace traffic; press Ctrl+C to stop")
        print_mock.assert_any_call(packet_render, flush=True)

    def test_stream_channel_with_hooks_prints_start_message_only_after_open(self) -> None:
        packet = mock.Mock()

        def iter_packets(*, on_opened=None, **_kwargs):
            self.assertEqual(print_mock.mock_calls, [])
            if on_opened is not None:
                on_opened()
            return [packet]

        with mock.patch("picotrace.app.trace_stream.iter_trace_packets", side_effect=iter_packets), mock.patch(
            "picotrace.app.trace_stream._format_trace_packet", return_value="packet-line"
        ), mock.patch("builtins.print") as print_mock:
            exit_code = trace_stream._stream_channel_with_hooks(4)

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            print_mock.mock_calls,
            [mock.call("streaming channel 4; press Ctrl+C to stop"), mock.call("packet-line", flush=True)],
        )

    def test_stream_channel_with_hooks_flushes_end_packet_without_waiting_for_next_packet(self) -> None:
        packet = TracePacket(
            header=TracePacketHeader(
                version=1,
                type=int(TraceType.SPI),
                channel=0,
                flags=int(TraceFlags.END),
                payload_len=1,
                meta=1,
                sequence=1,
                timestamp_us=100,
            ),
            payload=bytes([0xAA]),
        )

        def iter_packets(*, on_opened=None, **_kwargs):
            if on_opened is not None:
                on_opened()
            return [packet]

        with mock.patch("picotrace.app.trace_stream.iter_trace_packets", side_effect=iter_packets), mock.patch(
            "picotrace.app.trace_stream._format_trace_packet", return_value="packet-line"
        ) as format_mock, mock.patch("builtins.print") as print_mock:
            exit_code = trace_stream._stream_channel_with_hooks(0)

        self.assertEqual(exit_code, 0)
        format_mock.assert_called_once_with(packet)
        self.assertEqual(
            print_mock.mock_calls,
            [mock.call("streaming channel 0; press Ctrl+C to stop"), mock.call("packet-line", flush=True)],
        )