using System.Buffers.Binary;
using System.Runtime.InteropServices;

namespace PicoTrace.Trace;

public static class TraceDecoder
{
    public const byte TracePacketVersion = 1;
    public const int TracePacketBytes = 128;
    public const int TracePacketHeaderBytes = 16;
    public const int TracePacketPayloadBytes = TracePacketBytes - TracePacketHeaderBytes;

    public static TracePacket DecodeTracePacket(ReadOnlySpan<byte> packetBytes)
    {
        if (packetBytes.Length < TracePacketHeaderBytes)
        {
            throw new TraceDecodeException("trace packet is shorter than the fixed header");
        }

        var header = DecodeHeader(packetBytes[..TracePacketHeaderBytes]);
        ValidateHeader(header);

        var expectedSize = TracePacketHeaderBytes + header.PayloadLength;
        if (packetBytes.Length != expectedSize)
        {
            throw new TraceDecodeException(
                $"trace packet length {packetBytes.Length} does not match header-declared size {expectedSize}");
        }

        return new TracePacket(header, packetBytes[TracePacketHeaderBytes..expectedSize].ToArray());
    }

    public static IReadOnlyList<I2cEvent> DecodeI2cEvents(TracePacket packet)
    {
        if (packet.Header.TraceType is not TraceType.I2C)
        {
            throw new TraceDecodeException("trace packet does not carry I2C data");
        }

        if ((packet.Header.PayloadLength % 2) != 0)
        {
            throw new TraceDecodeException("I2C payload length must be an even number of bytes");
        }

        var events = new List<I2cEvent>(packet.Payload.Length / 2);
        for (var index = 0; index < packet.Payload.Length; index += 2)
        {
            events.Add(new I2cEvent(packet.Payload[index], packet.Payload[index + 1]));
        }

        if (packet.Header.Meta != events.Count)
        {
            throw new TraceDecodeException(
                $"I2C event count mismatch: header meta {packet.Header.Meta}, decoded {events.Count}");
        }

        return events;
    }

    public static SpiSamples DecodeSpiSamples(TracePacket packet)
    {
        if (packet.Header.TraceType is not TraceType.SPI)
        {
            throw new TraceDecodeException("trace packet does not carry SPI data");
        }

        if (!Enum.IsDefined((SpiCaptureMode)packet.Header.Meta))
        {
            throw new TraceDecodeException($"unknown SPI capture mode: {packet.Header.Meta}");
        }

        var captureMode = (SpiCaptureMode)packet.Header.Meta;
        if (captureMode is SpiCaptureMode.Mosi)
        {
            return new SpiSamples(captureMode, packet.Payload.ToArray(), null);
        }

        if (captureMode is SpiCaptureMode.MosiMiso)
        {
            if ((packet.Header.PayloadLength % 2) != 0)
            {
                throw new TraceDecodeException("SPI MOSI+MISO payload length must be even");
            }

            var mosi = new byte[packet.Payload.Length / 2];
            var miso = new byte[packet.Payload.Length / 2];
            for (var index = 0; index < packet.Payload.Length; index += 2)
            {
                mosi[index / 2] = packet.Payload[index];
                miso[index / 2] = packet.Payload[index + 1];
            }

            return new SpiSamples(captureMode, mosi, miso);
        }

        throw new TraceDecodeException("SPI capture mode DISABLED is not valid in emitted trace packets");
    }

    internal static TracePacketHeader DecodeHeader(ReadOnlySpan<byte> headerBytes)
    {
        return new TracePacketHeader(
            Version: headerBytes[0],
            Type: headerBytes[1],
            Channel: headerBytes[2],
            Flags: headerBytes[3],
            PayloadLength: BinaryPrimitives.ReadUInt16LittleEndian(headerBytes[4..6]),
            Meta: BinaryPrimitives.ReadUInt16LittleEndian(headerBytes[6..8]),
            Sequence: BinaryPrimitives.ReadUInt32LittleEndian(headerBytes[8..12]),
            TimestampUs: BinaryPrimitives.ReadUInt32LittleEndian(headerBytes[12..16]));
    }

    internal static void ValidateHeader(TracePacketHeader header)
    {
        if (header.Version != TracePacketVersion)
        {
            throw new TraceDecodeException($"unsupported trace packet version: {header.Version}");
        }

        if (header.PayloadLength > TracePacketPayloadBytes)
        {
            throw new TraceDecodeException(
                $"payload_len {header.PayloadLength} exceeds maximum {TracePacketPayloadBytes}");
        }
    }
}

public sealed class TraceStreamDecoder
{
    private readonly List<byte> _buffer = [];

    public IReadOnlyList<TracePacket> Append(ReadOnlySpan<byte> data)
    {
        foreach (var value in data)
        {
            _buffer.Add(value);
        }

        var packets = new List<TracePacket>();
        while (true)
        {
            if (_buffer.Count < TraceDecoder.TracePacketHeaderBytes)
            {
                return packets;
            }

            var headerBytes = CollectionsMarshal.AsSpan(_buffer).Slice(0, TraceDecoder.TracePacketHeaderBytes);
            var header = TraceDecoder.DecodeHeader(headerBytes);
            TraceDecoder.ValidateHeader(header);
            var packetSize = TraceDecoder.TracePacketHeaderBytes + header.PayloadLength;
            if (_buffer.Count < packetSize)
            {
                return packets;
            }

            var packetBytes = CollectionsMarshal.AsSpan(_buffer).Slice(0, packetSize).ToArray();
            _buffer.RemoveRange(0, packetSize);
            packets.Add(new TracePacket(header, packetBytes[TraceDecoder.TracePacketHeaderBytes..]));
        }
    }

    public int BufferedByteCount => _buffer.Count;
}