using System.Buffers.Binary;
using PicoTrace.Trace;

namespace PicoTrace.Control;

public enum HidOpcode : byte
{
    Nop = 0x00,
    GetStatus = 0x01,
    StreamEnable = 0x02,
    StreamDisable = 0x03,
    I2cMonitorSetRate = 0x04,
    I2cMonitorGetStatus = 0x05,
    I2cMonitorGetAllStatus = 0x06,
    SpiMonitorSetConfig = 0x07,
    SpiMonitorGetStatus = 0x08,
    SpiMonitorGetAllStatus = 0x09,
    LedOn = 0x80,
    LedOff = 0x81,
    Reboot = 0x82,
}

public enum HidStatus : byte
{
    Ok = 0x00,
    UnknownCommand = 0x01,
    BadLength = 0x02,
    Pending = 0x03,
    Rejected = 0x04,
    Busy = 0x05,
}

public class HidControlException(string message) : InvalidOperationException(message)
{
}

public sealed class HidProtocolException(string message) : HidControlException(message)
{
}

public readonly record struct HidCommand(byte Opcode, byte Sequence, byte[] Payload)
{
    public byte[] ToReportBytes()
    {
        if (Payload.Length > HidProtocol.DefaultReportSize - 4)
        {
            throw new ArgumentException("HID payload exceeds the fixed PicoTrace report size", nameof(Payload));
        }

        var report = new byte[HidProtocol.DefaultReportSize];
        report[0] = Opcode;
        report[1] = Sequence;
        report[2] = 0;
        report[3] = (byte)Payload.Length;
        Array.Copy(Payload, 0, report, 4, Payload.Length);
        return report;
    }
}

public readonly record struct HidResponse(byte Opcode, byte Sequence, HidStatus Status, byte[] Payload)
{
    public bool Ok => Status is HidStatus.Ok;
}

public readonly record struct HidDeviceStatus(bool StreamEnabled, string FirmwareVersion);

public readonly record struct I2cMonitorStatus(
    byte Channel,
    bool Initialized,
    bool Running,
    bool Overrun,
    uint SampleHz,
    uint CompletedBuffers,
    uint OverrunCount,
    bool TransitionPending,
    byte TransitionReason);

public readonly record struct I2cMonitorAllStatus(
    byte Channel,
    bool Initialized,
    bool Running,
    bool Overrun,
    uint SampleHz,
    uint OverrunCount,
    bool TransitionPending,
    byte TransitionReason);

public readonly record struct SpiBusConfig(SpiCaptureMode Capture, byte SpiMode, byte ChannelSelectMask, uint TimeoutUs);

public readonly record struct SpiMonitorStatus(
    byte Bus,
    bool Initialized,
    bool Running,
    SpiCaptureMode Capture,
    byte SpiMode,
    byte ChannelSelectMask,
    uint TimeoutUs,
    uint PacketsEmitted,
    uint OverrunCount,
    uint SinkOverrunCount,
    uint SamplerOverrunCount,
    uint RingDropCount,
    uint UsbStallCount,
    uint UsbHostBackpressureStallCount,
    uint UsbPolicyDeferralCount,
    uint PeakRingDepthPackets);

public readonly record struct SpiMonitorAllStatus(
    byte Bus,
    bool Initialized,
    bool Running,
    SpiCaptureMode Capture,
    byte SpiMode,
    uint TimeoutUs,
    bool Overrun);

public static class HidProtocol
{
    public const int DefaultReportSize = 64;
    private const int HidI2cStatusBytes = 18;
    private const int HidI2cAllStatusChannelBytes = 14;
    private const int HidSpiStatusBytes = 46;
    private const int HidSpiAllStatusChannelBytes = 10;

    public static HidResponse DecodeHidResponse(ReadOnlySpan<byte> reportBytes)
    {
        if (reportBytes.Length != DefaultReportSize)
        {
            throw new HidProtocolException("HID response must match the fixed 64-byte PicoTrace report size");
        }

        var payloadLength = reportBytes[3];
        if (payloadLength > DefaultReportSize - 4)
        {
            throw new HidProtocolException("HID response payload length exceeds the fixed PicoTrace report size");
        }

        if (!Enum.IsDefined((HidStatus)reportBytes[2]))
        {
            throw new HidProtocolException($"unknown HID status: {reportBytes[2]}");
        }

        return new HidResponse(reportBytes[0], reportBytes[1], (HidStatus)reportBytes[2], reportBytes[4..(4 + payloadLength)].ToArray());
    }

    public static HidDeviceStatus DecodeDeviceStatusPayload(ReadOnlySpan<byte> payload)
    {
        if (payload.Length == 1)
        {
            return new HidDeviceStatus(payload[0] != 0, string.Empty);
        }

        if (payload.Length < 2)
        {
            throw new HidProtocolException("GET_STATUS response must contain at least one payload byte");
        }

        var versionLength = payload[1];
        if (payload.Length != 2 + versionLength)
        {
            throw new HidProtocolException("GET_STATUS response has an unexpected firmware version payload size");
        }

        return new HidDeviceStatus(payload[0] != 0, System.Text.Encoding.ASCII.GetString(payload[2..(2 + versionLength)]));
    }

    public static byte[] BuildI2cSetRatePayload(int channel, uint sampleHz)
    {
        ValidateByteRange(channel, nameof(channel));
        var payload = new byte[5];
        payload[0] = (byte)channel;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(1), sampleHz);
        return payload;
    }

    public static I2cMonitorStatus DecodeI2cMonitorStatusPayload(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != HidI2cStatusBytes)
        {
            throw new HidProtocolException("I2C monitor status response has an unexpected payload size");
        }

        return new I2cMonitorStatus(
            payload[0],
            payload[1] != 0,
            payload[2] != 0,
            payload[3] != 0,
            BinaryPrimitives.ReadUInt32LittleEndian(payload[4..8]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..12]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..16]),
            payload[16] != 0,
            payload[17]);
    }

    public static IReadOnlyList<I2cMonitorAllStatus> DecodeI2cMonitorAllStatusPayload(ReadOnlySpan<byte> payload)
    {
        if ((payload.Length % HidI2cAllStatusChannelBytes) != 0)
        {
            throw new HidProtocolException("I2C all-status response has an unexpected payload size");
        }

        var statuses = new List<I2cMonitorAllStatus>(payload.Length / HidI2cAllStatusChannelBytes);
        for (var offset = 0; offset < payload.Length; offset += HidI2cAllStatusChannelBytes)
        {
            var channelPayload = payload.Slice(offset, HidI2cAllStatusChannelBytes);
            statuses.Add(new I2cMonitorAllStatus(
                channelPayload[0],
                channelPayload[1] != 0,
                channelPayload[2] != 0,
                channelPayload[3] != 0,
                BinaryPrimitives.ReadUInt32LittleEndian(channelPayload[4..8]),
                BinaryPrimitives.ReadUInt32LittleEndian(channelPayload[8..12]),
                channelPayload[12] != 0,
                channelPayload[13]));
        }

        return statuses;
    }

    public static byte[] BuildSpiSetConfigPayload(int bus, SpiCaptureMode capture, int spiMode, int channelSelectMask, uint timeoutUs)
    {
        ValidateByteRange(bus, nameof(bus));
        if (!Enum.IsDefined(capture))
        {
            throw new ArgumentOutOfRangeException(nameof(capture), "SPI capture mode must be a defined value");
        }

        if (spiMode is < 0 or > 3)
        {
            throw new ArgumentOutOfRangeException(nameof(spiMode), "SPI mode must be between 0 and 3");
        }

        ValidateByteRange(channelSelectMask, nameof(channelSelectMask));
        var payload = new byte[8];
        payload[0] = (byte)bus;
        payload[1] = (byte)capture;
        payload[2] = (byte)spiMode;
        payload[3] = (byte)channelSelectMask;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4), timeoutUs);
        return payload;
    }

    public static SpiMonitorStatus DecodeSpiMonitorStatusPayload(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != HidSpiStatusBytes)
        {
            throw new HidProtocolException("SPI monitor status response has an unexpected payload size");
        }

        return new SpiMonitorStatus(
            payload[0],
            payload[1] != 0,
            payload[2] != 0,
            (SpiCaptureMode)payload[3],
            payload[4],
            payload[5],
            BinaryPrimitives.ReadUInt32LittleEndian(payload[6..10]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[10..14]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[14..18]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[18..22]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[22..26]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[26..30]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[30..34]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[34..38]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[38..42]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[42..46]));
    }

    public static IReadOnlyList<SpiMonitorAllStatus> DecodeSpiMonitorAllStatusPayload(ReadOnlySpan<byte> payload)
    {
        if ((payload.Length % HidSpiAllStatusChannelBytes) != 0)
        {
            throw new HidProtocolException("SPI all-status response has an unexpected payload size");
        }

        var statuses = new List<SpiMonitorAllStatus>(payload.Length / HidSpiAllStatusChannelBytes);
        for (var offset = 0; offset < payload.Length; offset += HidSpiAllStatusChannelBytes)
        {
            var channelPayload = payload.Slice(offset, HidSpiAllStatusChannelBytes);
            statuses.Add(new SpiMonitorAllStatus(
                channelPayload[0],
                channelPayload[1] != 0,
                channelPayload[2] != 0,
                (SpiCaptureMode)channelPayload[3],
                channelPayload[4],
                BinaryPrimitives.ReadUInt32LittleEndian(channelPayload[5..9]),
                channelPayload[9] != 0));
        }

        return statuses;
    }

    private static void ValidateByteRange(int value, string parameterName)
    {
        if (value is < 0 or > 0xFF)
        {
            throw new ArgumentOutOfRangeException(parameterName, "Value must be between 0 and 255");
        }
    }
}