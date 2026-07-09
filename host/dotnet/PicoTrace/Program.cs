using PicoTrace.App;
using PicoTrace.Trace;

namespace PicoTrace;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            return Run(args);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"picotrace failed: {ex.Message}");
            return 1;
        }
    }

    private static int Run(string[] args)
    {
        if (args.Length == 0)
        {
            return InteractiveMode();
        }

        if (args[0] is "--help" or "-h")
        {
            PrintUsage();
            return 0;
        }

        return args[0] switch
        {
            "status" => RunStatus(),
            "stream" => RunStreamState(ParseState(args)),
            "led" => RunLed(ParseState(args)),
            "reboot" => RunReboot(args.Skip(1).Contains("--yes", StringComparer.OrdinalIgnoreCase)),
            "i2c" => RunI2c(ParseI2cOptions(args[1..])),
            "spi" => RunSpi(ParseSpiOptions(args[1..])),
            "trace" => RunTrace(ParseTraceOptions(args[1..])),
            _ => throw new ArgumentException($"unknown command {args[0]}")
        };
    }

    private static void PrintUsage()
    {
        Console.WriteLine("PicoTrace control and trace CLI");
        Console.WriteLine("Commands:");
        Console.WriteLine("  status");
        Console.WriteLine("  stream <on|off>");
        Console.WriteLine("  led <on|off>");
        Console.WriteLine("  reboot [--yes]");
        Console.WriteLine("  i2c --channel <0-3> --sample-hz <hz> [--no-stream]");
        Console.WriteLine("  spi --channel <0-5> [--capture MOSI|MOSI_MISO] [--spi-mode 0-3] [--timeout-us <us>] [--no-stream]");
        Console.WriteLine("  trace --channel <0-255>");
        Console.WriteLine("  trace --all");
    }

    private static int RunStatus() => ControlOperations.WithControl(ControlOperations.PrintDeviceStatus);

    private static int RunStreamState(bool enabled) => ControlOperations.WithControl(control => control.SetStreamEnabled(enabled));

    private static int RunLed(bool on) => ControlOperations.WithControl(control => control.SetLed(on));

    private static int RunReboot(bool yes)
    {
        if (!yes)
        {
            Console.Write("reboot the PicoTrace device? [y/N]: ");
            var answer = (Console.ReadLine() ?? string.Empty).Trim().ToLowerInvariant();
            if (answer is not ("y" or "yes"))
            {
                Console.WriteLine("reboot cancelled");
                return 1;
            }
        }

        return ControlOperations.WithControl(control => control.Reboot());
    }

    private static int RunI2c(I2cOptions options)
    {
        Action stop = () => ControlOperations.StopI2cChannel(options.Channel);
        if (options.NoStream)
        {
            ControlOperations.PrintConfiguredI2cChannel(options.Channel, options.SampleHz);
            return 0;
        }

        return RunForegroundConfiguredMonitor(
            options.Channel,
            configure: () => ControlOperations.PrintConfiguredI2cChannel(options.Channel, options.SampleHz),
            stop: stop);
    }

    private static int RunSpi(SpiOptions options)
    {
        Action stop = () => ControlOperations.StopSpiLogicalChannel(options.Channel);
        if (options.NoStream)
        {
            ControlOperations.PrintConfiguredSpiChannel(options.Channel, options.Capture, options.SpiMode, options.TimeoutUs);
            return 0;
        }

        return RunForegroundConfiguredMonitor(
            options.Channel,
            configure: () => ControlOperations.PrintConfiguredSpiChannel(options.Channel, options.Capture, options.SpiMode, options.TimeoutUs),
            stop: stop);
    }

    private static int RunTrace(TraceOptions options) => options.All
        ? TraceStreaming.StreamAll()
        : TraceStreaming.StreamChannel(options.Channel!.Value);

    private static int RunForegroundConfiguredMonitor(int channel, Action configure, Action? stop)
    {
        var started = false;
        try
        {
            configure();
            return TraceStreaming.StreamChannelWithHooks(channel, () => started = true);
        }
        catch
        {
            if (!started)
            {
                if (stop is not null)
                {
                    RunBestEffort(stop);
                }
                else
                {
                    ControlOperations.DisableStreamBestEffort();
                }
            }

            throw;
        }
        finally
        {
            if (started && stop is not null)
            {
                RunBestEffort(stop);
            }
        }
    }

    private static int InteractiveMode()
    {
        while (true)
        {
            Console.WriteLine();
            Console.WriteLine("PicoTrace CLI");
            Console.WriteLine("1. status");
            Console.WriteLine("2. stream on");
            Console.WriteLine("3. stream off");
            Console.WriteLine("4. led on");
            Console.WriteLine("5. led off");
            Console.WriteLine("6. configure i2c channel and stream");
            Console.WriteLine("7. configure spi channel and stream");
            Console.WriteLine("8. reboot");
            Console.WriteLine("9. stream existing channel");
            Console.WriteLine("0. exit");
            Console.Write("> ");

            var choice = Console.ReadLine()?.Trim() ?? string.Empty;
            switch (choice)
            {
                case "0":
                    return 0;
                case "1":
                    RunStatus();
                    break;
                case "2":
                    RunStreamState(true);
                    break;
                case "3":
                    RunStreamState(false);
                    break;
                case "4":
                    RunLed(true);
                    break;
                case "5":
                    RunLed(false);
                    break;
                case "6":
                    RunI2c(new I2cOptions(PromptInt("i2c channel [0-3]: ", 0, 3), (uint)PromptInt("sample_hz: ", 1, int.MaxValue), false));
                    break;
                case "7":
                    var logicalChannel = PromptInt("spi logical channel [0-5]: ", 0, 5);
                    var captureIndex = PromptInt("capture 1=MOSI 2=MOSI_MISO: ", 1, 2);
                    var spiMode = PromptInt("spi_mode [0-3]: ", 0, 3);
                    var timeoutUs = (uint)PromptInt("timeout_us: ", 1, int.MaxValue);
                    var capture = captureIndex == 1 ? SpiCaptureMode.Mosi : SpiCaptureMode.MosiMiso;
                    RunSpi(new SpiOptions(logicalChannel, capture, spiMode, timeoutUs, false));
                    break;
                case "8":
                    RunReboot(false);
                    break;
                case "9":
                    RunTrace(new TraceOptions(PromptInt("logical trace channel: ", 0, 255), false));
                    break;
                default:
                    Console.WriteLine("unknown selection");
                    break;
            }
        }
    }

    private static int PromptInt(string prompt, int minimum, int maximum)
    {
        while (true)
        {
            Console.Write(prompt);
            var raw = Console.ReadLine();
            if (!int.TryParse(raw, out var value))
            {
                Console.WriteLine("enter a decimal integer");
                continue;
            }

            if (value >= minimum && value <= maximum)
            {
                return value;
            }

            Console.WriteLine($"enter a value between {minimum} and {maximum}");
        }
    }

    private static bool ParseState(string[] args)
    {
        if (args.Length < 2)
        {
            throw new ArgumentException("missing state for command");
        }

        return args[1] switch
        {
            "on" => true,
            "off" => false,
            _ => throw new ArgumentException($"unknown state {args[1]}")
        };
    }

    private static I2cOptions ParseI2cOptions(string[] args)
    {
        int? channel = null;
        uint? sampleHz = null;
        var noStream = false;

        for (var index = 0; index < args.Length; index += 1)
        {
            switch (args[index])
            {
                case "--channel":
                    channel = ParseRequiredInt(args, ref index);
                    break;
                case "--sample-hz":
                    sampleHz = ParseRequiredUInt(args, ref index, "--sample-hz");
                    break;
                case "--no-stream":
                    noStream = true;
                    break;
                default:
                    throw new ArgumentException($"unknown option {args[index]}");
            }
        }

        if (channel is null or < 0 or > 3)
        {
            throw new ArgumentException("i2c channel must be between 0 and 3");
        }

        if (sampleHz is null || sampleHz == 0)
        {
            throw new ArgumentException("sample_hz must be positive");
        }

        return new I2cOptions(channel.Value, sampleHz.Value, noStream);
    }

    private static SpiOptions ParseSpiOptions(string[] args)
    {
        int? channel = null;
        var capture = SpiCaptureMode.MosiMiso;
        var spiMode = 0;
        uint timeoutUs = 100;
        var noStream = false;

        for (var index = 0; index < args.Length; index += 1)
        {
            switch (args[index])
            {
                case "--channel":
                    channel = ParseRequiredInt(args, ref index);
                    break;
                case "--capture":
                    capture = ParseCapture(ParseRequiredString(args, ref index));
                    break;
                case "--spi-mode":
                    spiMode = ParseRequiredInt(args, ref index);
                    break;
                case "--timeout-us":
                    timeoutUs = ParseRequiredUInt(args, ref index, "--timeout-us");
                    break;
                case "--no-stream":
                    noStream = true;
                    break;
                default:
                    throw new ArgumentException($"unknown option {args[index]}");
            }
        }

        if (channel is null or < 0 or > 5)
        {
            throw new ArgumentException("spi logical channel must be between 0 and 5");
        }

        if (spiMode is < 0 or > 3)
        {
            throw new ArgumentException("spi_mode must be between 0 and 3");
        }

        if (timeoutUs == 0)
        {
            throw new ArgumentException("timeout_us must be positive");
        }

        return new SpiOptions(channel.Value, capture, spiMode, timeoutUs, noStream);
    }

    private static TraceOptions ParseTraceOptions(string[] args)
    {
        int? channel = null;
        var all = false;
        for (var index = 0; index < args.Length; index += 1)
        {
            switch (args[index])
            {
                case "--channel":
                    channel = ParseRequiredInt(args, ref index);
                    break;
                case "--all":
                    all = true;
                    break;
                default:
                    throw new ArgumentException($"unknown option {args[index]}");
            }
        }

        if (all && channel is not null)
        {
            throw new ArgumentException("trace accepts either --channel or --all, but not both");
        }

        if (all)
        {
            return new TraceOptions(null, true);
        }

        if (channel is null or < 0 or > 255)
        {
            throw new ArgumentException("trace requires either --channel <0-255> or --all");
        }

        return new TraceOptions(channel.Value, false);
    }

    private static int ParseRequiredInt(string[] args, ref int index)
    {
        var value = ParseRequiredString(args, ref index);
        return value.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? Convert.ToInt32(value[2..], 16)
            : int.Parse(value, System.Globalization.CultureInfo.InvariantCulture);
    }

    private static uint ParseRequiredUInt(string[] args, ref int index, string optionName)
    {
        var value = ParseRequiredString(args, ref index);
        try
        {
            return value.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
                ? Convert.ToUInt32(value[2..], 16)
                : uint.Parse(value, System.Globalization.CultureInfo.InvariantCulture);
        }
        catch (FormatException)
        {
            throw new ArgumentException($"{optionName} must be an unsigned integer");
        }
        catch (OverflowException)
        {
            throw new ArgumentException($"{optionName} must be an unsigned integer");
        }
    }

    private static string ParseRequiredString(string[] args, ref int index)
    {
        if (index + 1 >= args.Length)
        {
            throw new ArgumentException($"missing value for {args[index]}");
        }

        index += 1;
        return args[index];
    }

    private static SpiCaptureMode ParseCapture(string value)
    {
        return value.ToUpperInvariant() switch
        {
            "MOSI" => SpiCaptureMode.Mosi,
            "MOSI_MISO" => SpiCaptureMode.MosiMiso,
            _ => throw new ArgumentException($"unknown capture mode {value}")
        };
    }

    private static void RunBestEffort(Action action)
    {
        try
        {
            action();
        }
        catch
        {
            // Best-effort cleanup.
        }
    }

    private readonly record struct I2cOptions(int Channel, uint SampleHz, bool NoStream);
    private readonly record struct SpiOptions(int Channel, SpiCaptureMode Capture, int SpiMode, uint TimeoutUs, bool NoStream);
    private readonly record struct TraceOptions(int? Channel, bool All);
}