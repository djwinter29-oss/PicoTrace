from __future__ import annotations

import struct
import unittest

from picotrace.trace.decode import (
    I2cEventType,
    SpiCaptureMode,
    TRACE_PACKET_PAYLOAD_BYTES,
    TraceDecodeError,
    TraceFlags,
    TraceStreamDecoder,
    TraceType,
    decode_i2c_events,
    decode_spi_samples,
    decode_trace_packet,
)


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


class TraceDecodeTests(unittest.TestCase):
    def test_decode_trace_packet_round_trips_header_and_payload(self) -> None:
        raw = make_packet_bytes(
            trace_type=TraceType.I2C,
            channel=3,
            flags=int(TraceFlags.END | TraceFlags.OVERFLOW),
            payload=b"\x01\x00\x04\x00",
            meta=2,
            sequence=7,
            timestamp_us=1234,
        )

        packet = decode_trace_packet(raw)

        self.assertEqual(packet.header.trace_type, TraceType.I2C)
        self.assertEqual(packet.header.channel, 3)
        self.assertEqual(packet.header.flag_bits, TraceFlags.END | TraceFlags.OVERFLOW)
        self.assertEqual(packet.header.meta, 2)
        self.assertEqual(packet.header.sequence, 7)
        self.assertEqual(packet.header.timestamp_us, 1234)
        self.assertEqual(packet.payload, b"\x01\x00\x04\x00")

    def test_stream_decoder_handles_split_packets(self) -> None:
        raw = make_packet_bytes(
            trace_type=TraceType.SPI,
            channel=1,
            flags=int(TraceFlags.CONTINUED),
            payload=b"\x12\x34\x56\x78",
            meta=SpiCaptureMode.MOSI_MISO,
            sequence=2,
            timestamp_us=99,
        )
        decoder = TraceStreamDecoder()

        self.assertEqual(decoder.append(raw[:5]), [])
        self.assertEqual(decoder.buffered_byte_count, 5)

        packets = decoder.append(raw[5:])

        self.assertEqual(len(packets), 1)
        self.assertEqual(packets[0].payload, b"\x12\x34\x56\x78")
        self.assertEqual(decoder.buffered_byte_count, 0)

    def test_decode_i2c_events_validates_meta_count(self) -> None:
        packet = decode_trace_packet(
            make_packet_bytes(
                trace_type=TraceType.I2C,
                channel=0,
                flags=0,
                payload=b"\x01\x00\x02\xA5\x03\x00\x04\x00",
                meta=4,
                sequence=1,
                timestamp_us=10,
            )
        )

        events = decode_i2c_events(packet)

        self.assertEqual(
            [event.event_type for event in events],
            [
                I2cEventType.START,
                I2cEventType.DATA,
                I2cEventType.ACK,
                I2cEventType.STOP,
            ],
        )
        self.assertEqual(events[1].value, 0xA5)

    def test_decode_spi_samples_splits_interleaved_payload(self) -> None:
        packet = decode_trace_packet(
            make_packet_bytes(
                trace_type=TraceType.SPI,
                channel=1,
                flags=int(TraceFlags.END),
                payload=b"\x11\xAA\x22\xBB",
                meta=SpiCaptureMode.MOSI_MISO,
                sequence=5,
                timestamp_us=42,
            )
        )

        samples = decode_spi_samples(packet)

        self.assertEqual(samples.capture_mode, SpiCaptureMode.MOSI_MISO)
        self.assertEqual(samples.mosi, b"\x11\x22")
        self.assertEqual(samples.miso, b"\xAA\xBB")

    def test_stream_decoder_rejects_invalid_payload_len(self) -> None:
        bad_header = struct.pack("<BBBBHHII", 1, TraceType.I2C, 0, 0, TRACE_PACKET_PAYLOAD_BYTES + 1, 0, 1, 0)
        decoder = TraceStreamDecoder()

        self.assertEqual(decoder.append(bad_header), [])
        self.assertLess(decoder.buffered_byte_count, len(bad_header))

    def test_stream_decoder_resynchronizes_after_noise_before_valid_packet(self) -> None:
        raw = b"STATUS" + make_packet_bytes(
            trace_type=TraceType.I2C,
            channel=3,
            flags=int(TraceFlags.END),
            payload=b"\x01\x00",
            meta=1,
            sequence=8,
            timestamp_us=123,
        )
        decoder = TraceStreamDecoder()

        packets = decoder.append(raw)

        self.assertEqual(len(packets), 1)
        self.assertEqual(packets[0].header.channel, 3)
        self.assertEqual(packets[0].header.sequence, 8)