from __future__ import annotations

import struct
import unittest
from unittest import mock

from picotrace.trace import TraceChannelRegistry, TraceFlags, TraceType, iter_trace_packets


def make_packet_bytes(
    *,
    trace_type: int,
    channel: int,
    flags: int,
    payload: bytes,
    meta: int,
    sequence: int,
    timestamp_us: int,
    version: int = 1,
) -> bytes:
    header = struct.pack(
        "<BBBBHHII",
        version,
        trace_type,
        channel,
        flags,
        len(payload),
        meta,
        sequence,
        timestamp_us,
    )
    return header + payload


class TraceTransportTests(unittest.TestCase):
    def test_iter_trace_packets_decodes_bulk_reads(self) -> None:
        raw = make_packet_bytes(
            trace_type=TraceType.I2C,
            channel=2,
            flags=int(TraceFlags.END),
            payload=b"\x01\x00",
            meta=1,
            sequence=9,
            timestamp_us=321,
        )

        class FakeBackend:
            def __init__(self) -> None:
                self._chunks = [raw[:3], raw[3:], b""]

            def bulk_read(self, handle, endpoint_address, interface_number, read_buffer, timeout_ms):
                chunk = self._chunks.pop(0)
                for index, value in enumerate(chunk):
                    read_buffer[index] = value
                return len(chunk)

        fake_device = mock.Mock()
        fake_device._ctx = mock.Mock(backend=FakeBackend(), handle=object())

        with mock.patch("picotrace.trace.usb_bulk.open_trace_device", return_value=(fake_device, 7)), mock.patch(
            "picotrace.trace.usb_bulk.close_trace_device"
        ) as close_trace_device_mock, mock.patch(
            "picotrace.trace.usb_bulk.time.perf_counter", side_effect=[0.0, 0.1, 0.2, 1.1]
        ):
            packets = list(iter_trace_packets(duration_seconds=1.0, read_size=64))

        self.assertEqual(len(packets), 1)
        self.assertEqual(packets[0].header.channel, 2)
        self.assertEqual(packets[0].header.sequence, 9)
        self.assertEqual(packets[0].payload, b"\x01\x00")
        close_trace_device_mock.assert_called_once_with(fake_device, 7)

    def test_iter_trace_packets_filters_to_registered_channels(self) -> None:
        first_packet = make_packet_bytes(
            trace_type=TraceType.I2C,
            channel=1,
            flags=int(TraceFlags.END),
            payload=b"\x01\x00",
            meta=1,
            sequence=1,
            timestamp_us=100,
        )
        second_packet = make_packet_bytes(
            trace_type=TraceType.I2C,
            channel=2,
            flags=int(TraceFlags.END),
            payload=b"\x01\x00",
            meta=1,
            sequence=2,
            timestamp_us=200,
        )
        raw = first_packet + second_packet

        class FakeBackend:
            def __init__(self) -> None:
                self._chunks = [raw, b""]

            def bulk_read(self, handle, endpoint_address, interface_number, read_buffer, timeout_ms):
                chunk = self._chunks.pop(0)
                for index, value in enumerate(chunk):
                    read_buffer[index] = value
                return len(chunk)

        fake_device = mock.Mock()
        fake_device._ctx = mock.Mock(backend=FakeBackend(), handle=object())
        registry = TraceChannelRegistry([2])

        with mock.patch("picotrace.trace.usb_bulk.open_trace_device", return_value=(fake_device, 7)), mock.patch(
            "picotrace.trace.usb_bulk.close_trace_device"
        ), mock.patch("picotrace.trace.usb_bulk.time.perf_counter", side_effect=[0.0, 0.1, 1.1]):
            packets = list(iter_trace_packets(duration_seconds=1.0, read_size=128, channel_registry=registry))

        self.assertEqual(len(packets), 1)
        self.assertEqual(packets[0].header.channel, 2)