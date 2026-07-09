using PicoTrace.Control;
using PicoTrace.Trace;

namespace PicoTrace.App;

internal static class ControlOperations
{
    internal readonly record struct SpiBusControlConfig(int Bus, SpiCaptureMode Capture, int SpiMode, int ChannelSelectMask, uint TimeoutUs);

    public static int WithControl(Action<HidControlClient> operation)
    {
        using var control = HidControlClient.Open();
        operation(control);
        return 0;
    }

    public static void PrintDeviceStatus(HidControlClient control)
    {
        var status = control.GetStatus();
        Console.WriteLine($"stream_enabled={status.StreamEnabled}");

        Console.WriteLine("i2c_status:");
        foreach (var channelStatus in control.I2cGetAllStatus())
        {
            Console.WriteLine(
                $"  channel={channelStatus.Channel} initialized={channelStatus.Initialized} running={channelStatus.Running} " +
                $"overrun={channelStatus.Overrun} sample_hz={channelStatus.SampleHz} overrun_count={channelStatus.OverrunCount} " +
                $"transition_pending={channelStatus.TransitionPending} transition_reason={channelStatus.TransitionReason}");
        }

        Console.WriteLine("spi_status:");
        foreach (var busStatus in control.SpiGetAllStatus())
        {
            Console.WriteLine(
                $"  bus={busStatus.Bus} initialized={busStatus.Initialized} running={busStatus.Running} " +
                $"capture={busStatus.Capture} spi_mode={busStatus.SpiMode} timeout_us={busStatus.TimeoutUs} overrun={busStatus.Overrun}");
        }
    }

    public static void DisableStreamBestEffort()
    {
        try
        {
            using var control = HidControlClient.Open();
            control.SetStreamEnabled(false);
        }
        catch
        {
            // Best-effort cleanup.
        }
    }

    public static void StopI2cChannel(int channel)
    {
        using var control = HidControlClient.Open();
        control.I2cSetRate(channel, 0);
    }

    public static void StopSpiLogicalChannel(int logicalChannel)
    {
        var bus = logicalChannel / 3;
        using var control = HidControlClient.Open();
        var currentStatus = control.SpiGetStatus(bus);
        var stopConfig = BuildSpiStopConfig(logicalChannel, currentStatus);
        control.SpiSetConfig(stopConfig.Bus, stopConfig.Capture, stopConfig.SpiMode, stopConfig.ChannelSelectMask, stopConfig.TimeoutUs);
    }

    public static void ConfigureI2cChannel(int channel, uint sampleHz)
    {
        WithControl(control =>
        {
            control.I2cSetRate(channel, sampleHz);
            control.SetStreamEnabled(true);
        });
    }

    public static void PrintConfiguredI2cChannel(int channel, uint sampleHz)
    {
        ConfigureI2cChannel(channel, sampleHz);
        Console.WriteLine($"configured i2c channel {channel} at {sampleHz} Hz");
    }

    public static (int Bus, int ChannelSelectMask) ConfigureSpiChannel(int logicalChannel, SpiCaptureMode capture, int spiMode, uint timeoutUs)
    {
        WithControl(control =>
        {
            var currentStatus = control.SpiGetStatus(logicalChannel / 3);
            var applyConfig = BuildSpiApplyConfig(logicalChannel, capture, spiMode, timeoutUs, currentStatus);
            control.SpiSetConfig(applyConfig.Bus, applyConfig.Capture, applyConfig.SpiMode, applyConfig.ChannelSelectMask, applyConfig.TimeoutUs);
            control.SetStreamEnabled(true);
        });

        var config = BuildSpiApplyConfig(logicalChannel, capture, spiMode, timeoutUs, null);
        return (config.Bus, 1 << (logicalChannel % 3));
    }

    public static void PrintConfiguredSpiChannel(int logicalChannel, SpiCaptureMode capture, int spiMode, uint timeoutUs)
    {
        var (bus, channelSelectMask) = ConfigureSpiChannel(logicalChannel, capture, spiMode, timeoutUs);
        Console.WriteLine(
            $"configured spi logical channel {logicalChannel} on bus {bus} " +
            $"capture={capture} mode={spiMode} mask=0x{channelSelectMask:X2} timeout_us={timeoutUs}");
    }

    internal static SpiBusControlConfig BuildSpiApplyConfig(
        int logicalChannel,
        SpiCaptureMode capture,
        int spiMode,
        uint timeoutUs,
        SpiMonitorStatus? currentStatus)
    {
        var bus = logicalChannel / 3;
        var slotMask = 1 << (logicalChannel % 3);
        var mergedMask = slotMask;

        if (currentStatus is { } status && status.Bus == bus && status.Capture is not SpiCaptureMode.Disabled)
        {
            mergedMask |= status.ChannelSelectMask;
        }

        return new SpiBusControlConfig(bus, capture, spiMode, mergedMask, timeoutUs);
    }

    internal static SpiBusControlConfig BuildSpiStopConfig(int logicalChannel, SpiMonitorStatus currentStatus)
    {
        var bus = logicalChannel / 3;
        var slotMask = 1 << (logicalChannel % 3);
        var remainingMask = currentStatus.Bus == bus ? (currentStatus.ChannelSelectMask & ~slotMask) : 0;
        if (remainingMask == 0)
        {
            return new SpiBusControlConfig(bus, SpiCaptureMode.Disabled, 0, 0, 0);
        }

        return new SpiBusControlConfig(bus, currentStatus.Capture, currentStatus.SpiMode, remainingMask, currentStatus.TimeoutUs);
    }
}