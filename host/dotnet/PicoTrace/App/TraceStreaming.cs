using PicoTrace.Trace;

namespace PicoTrace.App;

internal static class TraceStreaming
{
    private const double StreamChunkSeconds = 24.0 * 60.0 * 60.0;
    private const uint SpiPacketCoalesceGapUs = 1000;

    private static string FormatCaptureMode(SpiCaptureMode capture)
    {
        return capture switch
        {
            SpiCaptureMode.Disabled => "DISABLED",
            SpiCaptureMode.Mosi => "MOSI",
            SpiCaptureMode.MosiMiso => "MOSI_MISO",
            _ => capture.ToString(),
        };
    }

    internal static string FormatTimestampUs(uint timestampUs)
    {
        var totalSeconds = timestampUs / 1_000_000;
        var micros = timestampUs % 1_000_000;
        var minutes = totalSeconds / 60;
        var seconds = totalSeconds % 60;
        var hours = minutes / 60;
        minutes %= 60;
        return $"{hours:00}:{minutes:00}:{seconds:00}.{micros:000000}";
    }

    internal static string FormatI2cEvent(I2cEvent evt)
    {
        return evt.EventType switch
        {
            I2cEventType.Start => "START",
            I2cEventType.Data => $"DATA:{evt.Value:X2}",
            I2cEventType.Ack => evt.Value == 0 ? "ACK" : "NACK",
            I2cEventType.Stop => "STOP",
            _ => $"{evt.EventType}:{evt.Value:X2}",
        };
    }

    public static string FormatTracePacket(TracePacket packet)
    {
        var header = packet.Header;
        if (header.TraceType is TraceType.I2C)
        {
            var prefix = $"[{FormatTimestampUs(header.TimestampUs)}] seq={header.Sequence,6} {header.TraceType} CH{header.Channel}: ";
            var payload = string.Join(' ', TraceDecoder.DecodeI2cEvents(packet).Select(FormatI2cEvent));
            return prefix + payload;
        }

        var prefix = $"[{FormatTimestampUs(header.TimestampUs)}] seq={header.Sequence,6} {header.TraceType} CH{header.Channel} ";
        var samples = TraceDecoder.DecodeSpiSamples(packet);
        if (samples.Miso is null)
        {
            var payload = string.Join(' ', samples.Mosi.Select(value => $"{value:X2}"));
            return prefix + $"MOSI: {payload}";
        }

        var pairs = string.Join(' ', samples.Mosi.Zip(samples.Miso, (mosi, miso) => $"{mosi:X2}/{miso:X2}"));
        return prefix + $"{FormatCaptureMode(samples.CaptureMode)} {pairs}";
    }

    internal static bool CanCoalesceSpiPackets(TracePacket current, TracePacket next)
    {
        var currentHeader = current.Header;
        var nextHeader = next.Header;

        if (currentHeader.TraceType is not TraceType.SPI || nextHeader.TraceType is not TraceType.SPI)
        {
            return false;
        }

        if (currentHeader.Channel != nextHeader.Channel || currentHeader.Meta != nextHeader.Meta)
        {
            return false;
        }

        if (nextHeader.Sequence != currentHeader.Sequence)
        {
            return false;
        }

        if ((nextHeader.TimestampUs - currentHeader.TimestampUs) > SpiPacketCoalesceGapUs)
        {
            return false;
        }

        if ((nextHeader.FlagBits & TraceFlags.Continued) == 0)
        {
            return false;
        }

        var disallowedFlags = TraceFlags.Overflow | TraceFlags.Truncated | TraceFlags.Error;
        if ((currentHeader.FlagBits & disallowedFlags) != 0 || (nextHeader.FlagBits & disallowedFlags) != 0)
        {
            return false;
        }

        return true;
    }

    internal static TracePacket CoalesceSpiPackets(TracePacket current, TracePacket next)
    {
        var payload = current.Payload.Concat(next.Payload).ToArray();
        var header = new TracePacketHeader(
            current.Header.Version,
            current.Header.Type,
            current.Header.Channel,
            (byte)(current.Header.Flags | next.Header.Flags),
            (ushort)payload.Length,
            current.Header.Meta,
            current.Header.Sequence,
            current.Header.TimestampUs);
        return new TracePacket(header, payload);
    }

    internal static bool ShouldFlushImmediately(TracePacket packet) => (packet.Header.FlagBits & TraceFlags.End) != 0;

    public static int StreamChannel(int channel) => StreamChannelWithHooks(channel);

    public static int StreamAll() => StreamAllWithHooks();

    public static int StreamChannelWithHooks(int channel, Action? onStarted = null)
    {
        var registry = new TraceChannelRegistry([channel]);
        return StreamWithRegistry(registry, $"streaming channel {channel}; press Ctrl+C to stop", StreamChunkSeconds, onStarted);
    }

    public static int StreamAllWithHooks(Action? onStarted = null)
    {
        return StreamWithRegistry(new TraceChannelRegistry(), "streaming all trace traffic; press Ctrl+C to stop", null, onStarted);
    }

    private static int StreamWithRegistry(
        TraceChannelRegistry registry,
        string startMessage,
        double? durationSeconds,
        Action? onStarted)
    {
        TracePacket? pendingPacket = null;
        var cancelRequested = false;
        var streamOpened = false;
        ConsoleCancelEventHandler? handler = null;
        handler = (_, eventArgs) =>
        {
            cancelRequested = true;
            eventArgs.Cancel = true;
        };

        Console.CancelKeyPress += handler;

        void HandleOpened()
        {
            if (streamOpened)
            {
                return;
            }

            streamOpened = true;
            Console.WriteLine(startMessage);
            onStarted?.Invoke();
        }

        try
        {
            foreach (var packet in UsbBulkTraceTransport.IterTracePackets(
                         durationSeconds: durationSeconds,
                         channelRegistry: registry,
                         keepRunning: () => !cancelRequested,
                         onOpened: HandleOpened))
            {
                if (pendingPacket is null)
                {
                    pendingPacket = packet;
                    if (ShouldFlushImmediately(pendingPacket.Value))
                    {
                        Console.WriteLine(FormatTracePacket(pendingPacket.Value));
                        pendingPacket = null;
                    }
                    continue;
                }

                if (CanCoalesceSpiPackets(pendingPacket.Value, packet))
                {
                    pendingPacket = CoalesceSpiPackets(pendingPacket.Value, packet);
                    if (ShouldFlushImmediately(pendingPacket.Value))
                    {
                        Console.WriteLine(FormatTracePacket(pendingPacket.Value));
                        pendingPacket = null;
                    }
                    continue;
                }

                Console.WriteLine(FormatTracePacket(pendingPacket.Value));
                pendingPacket = packet;
                if (ShouldFlushImmediately(pendingPacket.Value))
                {
                    Console.WriteLine(FormatTracePacket(pendingPacket.Value));
                    pendingPacket = null;
                }
            }

            if (pendingPacket is not null)
            {
                Console.WriteLine(FormatTracePacket(pendingPacket.Value));
            }

            if (cancelRequested)
            {
                Console.WriteLine("stream stopped");
            }

            return 0;
        }
        catch (OperationCanceledException)
        {
            if (pendingPacket is not null)
            {
                Console.WriteLine(FormatTracePacket(pendingPacket.Value));
            }
            Console.WriteLine("stream stopped");
            return 0;
        }
        finally
        {
            Console.CancelKeyPress -= handler;
        }
    }
}