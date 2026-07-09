from __future__ import annotations

import unittest
from unittest import mock

from picotrace.app import trace_stream
from picotrace.trace import TracePacket, TracePacketHeader, TraceType


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

        self.assertIn("ch=2 I2C START:55", rendered)

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