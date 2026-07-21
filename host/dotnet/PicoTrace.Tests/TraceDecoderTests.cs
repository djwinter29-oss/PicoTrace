using Microsoft.VisualStudio.TestTools.UnitTesting;
using PicoTrace.Trace;

namespace PicoTrace.Tests;

[TestClass]
public sealed class TraceDecoderTests
{
    [TestMethod]
    public void DecodeTracePacket_DecodesI2cPacket()
    {
        var packetBytes = new byte[]
        {
            1, 1, 2, 0,
            4, 0,
            2, 0,
            7, 0, 0, 0,
            123, 0, 0, 0,
            1, 0x55, 4, 0x00,
        };

        var packet = TraceDecoder.DecodeTracePacket(packetBytes);
        var events = TraceDecoder.DecodeI2cEvents(packet);

        Assert.AreEqual((byte)2, packet.Header.Channel);
        Assert.AreEqual(2, events.Count);
        Assert.AreEqual(I2cEventType.Start, events[0].EventType);
        Assert.AreEqual((byte)0x55, events[0].Value);
    }

    [TestMethod]
    public void DecodeSpiSamples_DecodesMosiMisoPairs()
    {
        var packet = new TracePacket(
            new TracePacketHeader(1, (byte)TraceType.SPI, 1, 0, 4, (ushort)SpiCaptureMode.MosiMiso, 2, 9),
            [0x11, 0x22, 0x33, 0x44]);

        var samples = TraceDecoder.DecodeSpiSamples(packet);

        CollectionAssert.AreEqual(new byte[] { 0x11, 0x33 }, samples.Mosi);
        CollectionAssert.AreEqual(new byte[] { 0x22, 0x44 }, samples.Miso!);
    }

    [TestMethod]
    public void TraceStreamDecoder_FramesIncrementalStream()
    {
        var decoder = new TraceStreamDecoder();
        var packetBytes = new byte[]
        {
            1, 1, 2, 0,
            2, 0,
            1, 0,
            5, 0, 0, 0,
            9, 0, 0, 0,
            1, 0x7F,
        };

        var first = decoder.Append(packetBytes.AsSpan(0, 8));
        var second = decoder.Append(packetBytes.AsSpan(8));

        Assert.AreEqual(0, first.Count);
        Assert.AreEqual(1, second.Count);
        Assert.AreEqual((byte)2, second[0].Header.Channel);
    }

    [TestMethod]
    public void TraceChannelRegistry_FiltersByRegisteredChannels()
    {
        var registry = new TraceChannelRegistry([1, 3]);
        var packets = new[]
        {
            new TracePacket(new TracePacketHeader(1, (byte)TraceType.I2C, 1, 0, 0, 0, 0, 0), []),
            new TracePacket(new TracePacketHeader(1, (byte)TraceType.I2C, 2, 0, 0, 0, 0, 0), []),
            new TracePacket(new TracePacketHeader(1, (byte)TraceType.I2C, 3, 0, 0, 0, 0, 0), []),
        };

        var filtered = registry.FilterPackets(packets).ToArray();

        Assert.AreEqual(2, filtered.Length);
        Assert.AreEqual((byte)1, filtered[0].Header.Channel);
        Assert.AreEqual((byte)3, filtered[1].Header.Channel);
    }

    [TestMethod]
    public void TraceStreamDecoder_SkipsInvalidLeadingBytesAndResynchronizes()
    {
        var decoder = new TraceStreamDecoder();
        var packetBytes = new byte[]
        {
            (byte)'S', (byte)'T', (byte)'A', (byte)'T', (byte)'U', (byte)'S',
            1, 1, 2, 0,
            2, 0,
            1, 0,
            5, 0, 0, 0,
            9, 0, 0, 0,
            1, 0x7F,
        };

        var packets = decoder.Append(packetBytes);

        Assert.AreEqual(1, packets.Count);
        Assert.AreEqual((byte)2, packets[0].Header.Channel);
        Assert.AreEqual((uint)5, packets[0].Header.Sequence);
        Assert.AreEqual(0, decoder.BufferedByteCount);
    }

    [TestMethod]
    public void TraceStreamDecoder_SkipsInvalidPayloadLengthHeader()
    {
        var decoder = new TraceStreamDecoder();
        var invalidPayloadLength = (ushort)(TraceDecoder.TracePacketPayloadBytes + 1);
        var invalidHeader = new byte[]
        {
            1, 1, 0, 0,
            (byte)(invalidPayloadLength & 0xFF), (byte)(invalidPayloadLength >> 8),
            0, 0,
            1, 0, 0, 0,
            0, 0, 0, 0,
        };

        var packets = decoder.Append(invalidHeader);

        Assert.AreEqual(0, packets.Count);
        Assert.IsTrue(decoder.BufferedByteCount < invalidHeader.Length);
    }

    [TestMethod]
    public void FormatTracePacket_FormatsI2cEventsLikePythonHost()
    {
        var packet = new TracePacket(
            new TracePacketHeader(1, (byte)TraceType.I2C, 4, (byte)TraceFlags.End, 8, 4, 7, 1_234_567),
            [
                (byte)I2cEventType.Start, 0x00,
                (byte)I2cEventType.Data, 0x50,
                (byte)I2cEventType.Ack, 0x01,
                (byte)I2cEventType.Stop, 0x00,
            ]);

        var text = App.TraceStreaming.FormatTracePacket(packet);

        Assert.AreEqual("[00:00:01.234567] seq=     7 I2C CH4: START DATA:50 NACK STOP", text);
    }

    [TestMethod]
    public void CanCoalesceSpiPackets_AcceptsContinuedSiblingFragment()
    {
        var first = new TracePacket(
            new TracePacketHeader(1, (byte)TraceType.SPI, 1, 0, 2, (ushort)SpiCaptureMode.Mosi, 9, 100),
            [0x11, 0x22]);
        var second = new TracePacket(
            new TracePacketHeader(1, (byte)TraceType.SPI, 1, (byte)(TraceFlags.Continued | TraceFlags.End), 2, (ushort)SpiCaptureMode.Mosi, 9, 200),
            [0x33, 0x44]);

        Assert.IsTrue(App.TraceStreaming.CanCoalesceSpiPackets(first, second));

        var merged = App.TraceStreaming.CoalesceSpiPackets(first, second);
        CollectionAssert.AreEqual(new byte[] { 0x11, 0x22, 0x33, 0x44 }, merged.Payload);
        Assert.AreEqual(4, merged.Header.PayloadLength);
        Assert.AreEqual((byte)(TraceFlags.Continued | TraceFlags.End), merged.Header.Flags);
        Assert.IsTrue(App.TraceStreaming.ShouldFlushImmediately(merged));
    }

    [TestMethod]
    public void FormatTracePacket_FormatsSpiCaptureModeLikePythonHost()
    {
        var packet = new TracePacket(
            new TracePacketHeader(1, (byte)TraceType.SPI, 1, (byte)TraceFlags.End, 4, (ushort)SpiCaptureMode.MosiMiso, 9, 200),
            [0x11, 0x22, 0x33, 0x44]);

        var text = App.TraceStreaming.FormatTracePacket(packet);

        Assert.AreEqual("[00:00:00.000200] seq=     9 SPI CH1 MOSI_MISO 11/22 33/44", text);
    }
}