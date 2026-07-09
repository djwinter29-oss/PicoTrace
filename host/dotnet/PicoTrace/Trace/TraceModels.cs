namespace PicoTrace.Trace;

public enum TraceType : byte
{
    I2C = 1,
    SPI = 2,
}

[Flags]
public enum TraceFlags : byte
{
    End = 1 << 0,
    Continued = 1 << 1,
    Overflow = 1 << 2,
    Truncated = 1 << 3,
    Error = 1 << 4,
}

public enum I2cEventType : byte
{
    Start = 1,
    Data = 2,
    Ack = 3,
    Stop = 4,
    Error = 128,
    Overflow = 129,
    ControlReconfig = 130,
    ControlStop = 131,
}

public enum SpiCaptureMode : byte
{
    Disabled = 0,
    Mosi = 1,
    MosiMiso = 2,
}

public sealed class TraceDecodeException(string message) : FormatException(message)
{
}

public readonly record struct TracePacketHeader(
    byte Version,
    byte Type,
    byte Channel,
    byte Flags,
    ushort PayloadLength,
    ushort Meta,
    uint Sequence,
    uint TimestampUs)
{
    public TraceType TraceType => Enum.IsDefined((TraceType)Type)
        ? (TraceType)Type
        : throw new TraceDecodeException($"unknown trace type: {Type}");

    public TraceFlags FlagBits => (TraceFlags)Flags;
}

public readonly record struct TracePacket(TracePacketHeader Header, byte[] Payload);

public readonly record struct I2cEvent(byte Type, byte Value)
{
    public I2cEventType EventType => Enum.IsDefined((I2cEventType)Type)
        ? (I2cEventType)Type
        : throw new TraceDecodeException($"unknown I2C event type: {Type}");
}

public readonly record struct SpiSamples(SpiCaptureMode CaptureMode, byte[] Mosi, byte[]? Miso);