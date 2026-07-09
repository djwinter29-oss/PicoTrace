using PicoTrace.Trace;

namespace PicoTrace.App;

internal static class TraceStreaming
{
    private const double StreamChunkSeconds = 24.0 * 60.0 * 60.0;

    public static string FormatTracePacket(TracePacket packet)
    {
        var header = packet.Header;
        var prefix = $"[{header.TimestampUs,10}] seq={header.Sequence,6} ch={header.Channel} {header.TraceType} ";
        if (header.TraceType is TraceType.I2C)
        {
            var payload = string.Join(' ', TraceDecoder.DecodeI2cEvents(packet).Select(evt => $"{evt.EventType}:{evt.Value:X2}"));
            return prefix + payload;
        }

        var samples = TraceDecoder.DecodeSpiSamples(packet);
        if (samples.Miso is null)
        {
            var payload = string.Join(' ', samples.Mosi.Select(value => $"{value:X2}"));
            return prefix + $"MOSI {payload}";
        }

        var pairs = string.Join(' ', samples.Mosi.Zip(samples.Miso, (mosi, miso) => $"{mosi:X2}/{miso:X2}"));
        return prefix + $"{samples.CaptureMode} {pairs}";
    }

    public static int StreamChannel(int channel) => StreamChannelWithHooks(channel);

    public static int StreamChannelWithHooks(int channel, Action? onStarted = null)
    {
        var registry = new TraceChannelRegistry([channel]);
        var cancelRequested = false;
        ConsoleCancelEventHandler? handler = null;
        handler = (_, eventArgs) =>
        {
            cancelRequested = true;
            eventArgs.Cancel = true;
        };

        Console.CancelKeyPress += handler;
        Console.WriteLine($"streaming channel {channel}; press Ctrl+C to stop");
        try
        {
            foreach (var packet in UsbBulkTraceTransport.IterTracePackets(
                         durationSeconds: StreamChunkSeconds,
                         channelRegistry: registry,
                         keepRunning: () => !cancelRequested,
                         onOpened: onStarted))
            {
                Console.WriteLine(FormatTracePacket(packet));
            }

            if (cancelRequested)
            {
                Console.WriteLine("stream stopped");
            }

            return 0;
        }
        finally
        {
            Console.CancelKeyPress -= handler;
        }
    }
}