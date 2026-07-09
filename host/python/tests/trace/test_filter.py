from __future__ import annotations

import struct
import unittest

from picotrace.trace import TraceChannelRegistry
from picotrace.trace.decode import TraceType, decode_trace_packet


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


class TraceFilterTests(unittest.TestCase):
    def test_empty_registry_allows_all_packets(self) -> None:
        registry = TraceChannelRegistry()
        packet = decode_trace_packet(
            make_packet_bytes(
                trace_type=TraceType.I2C,
                channel=7,
                flags=0,
                payload=b"\x01\x00",
                meta=1,
                sequence=1,
                timestamp_us=1,
            )
        )

        self.assertTrue(registry.matches_packet(packet))

    def test_registry_filters_to_registered_channels(self) -> None:
        registry = TraceChannelRegistry()
        registry.register_channel(1)
        registry.register_channel(3)

        packets = [
            decode_trace_packet(
                make_packet_bytes(
                    trace_type=TraceType.I2C,
                    channel=0,
                    flags=0,
                    payload=b"\x01\x00",
                    meta=1,
                    sequence=1,
                    timestamp_us=1,
                )
            ),
            decode_trace_packet(
                make_packet_bytes(
                    trace_type=TraceType.I2C,
                    channel=1,
                    flags=0,
                    payload=b"\x01\x00",
                    meta=1,
                    sequence=2,
                    timestamp_us=2,
                )
            ),
            decode_trace_packet(
                make_packet_bytes(
                    trace_type=TraceType.I2C,
                    channel=3,
                    flags=0,
                    payload=b"\x01\x00",
                    meta=1,
                    sequence=3,
                    timestamp_us=3,
                )
            ),
        ]

        filtered = list(registry.iter_packets(packets))

        self.assertEqual([packet.header.channel for packet in filtered], [1, 3])

    def test_registry_rejects_invalid_channel_numbers(self) -> None:
        registry = TraceChannelRegistry()

        with self.assertRaises(ValueError):
            registry.register_channel(256)