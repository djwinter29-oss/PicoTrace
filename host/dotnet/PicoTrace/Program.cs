using System.Diagnostics;
using LibUsbDotNet;
using LibUsbDotNet.Info;
using LibUsbDotNet.LibUsb;
using LibUsbDotNet.Main;

namespace PicoTrace;

internal static class Program
{
    private static int Main(string[] args)
    {
        var options = ParseArguments(args);

        try
        {
            var stats = CaptureBulkStream(options);
            Console.WriteLine($"total bytes read: {stats.TotalBytes}");
            Console.WriteLine($"average speed: {stats.BytesPerSecond:F2} B/s");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"capture failed: {ex.Message}");
            return 1;
        }
    }

    private static CaptureOptions ParseArguments(string[] args)
    {
        var options = new CaptureOptions(
            VendorId: Defaults.Vid,
            ProductId: Defaults.Pid,
            EndpointAddress: Defaults.EndpointAddress,
            DurationSeconds: Defaults.DurationSeconds,
            ReadSize: Defaults.ReadSize,
            TimeoutMilliseconds: Defaults.TimeoutMilliseconds);

        for (var index = 0; index < args.Length; index += 1)
        {
            var argument = args[index];

            if (argument is "--help" or "-h")
            {
                PrintUsage();
                return options;
            }

            if (index + 1 >= args.Length)
            {
                throw new ArgumentException($"missing value for {argument}");
            }

            var value = args[index + 1];

            switch (argument)
            {
                case "--duration":
                    options = options with { DurationSeconds = double.Parse(value, System.Globalization.CultureInfo.InvariantCulture) };
                    break;
                case "--vid":
                    options = options with { VendorId = ParseInt(value) };
                    break;
                case "--pid":
                    options = options with { ProductId = ParseInt(value) };
                    break;
                case "--endpoint":
                    options = options with { EndpointAddress = (byte)ParseInt(value) };
                    break;
                case "--read-size":
                    options = options with { ReadSize = int.Parse(value, System.Globalization.CultureInfo.InvariantCulture) };
                    break;
                case "--timeout-ms":
                    options = options with { TimeoutMilliseconds = int.Parse(value, System.Globalization.CultureInfo.InvariantCulture) };
                    break;
                default:
                    throw new ArgumentException($"unknown argument {argument}");
            }

            index += 1;
        }

        if (options.DurationSeconds <= 0.0)
        {
            throw new ArgumentException("duration must be positive");
        }

        if (options.ReadSize <= 0)
        {
            throw new ArgumentException("read size must be positive");
        }

        return options;
    }

    private static void PrintUsage()
    {
        Console.WriteLine("Capture PicoTrace vendor bulk data and report total bytes plus average speed.");
        Console.WriteLine("Options:");
        Console.WriteLine("  --duration <seconds>");
        Console.WriteLine("  --vid <vendor-id>");
        Console.WriteLine("  --pid <product-id>");
        Console.WriteLine("  --endpoint <endpoint-address>");
        Console.WriteLine("  --read-size <bytes>");
        Console.WriteLine("  --timeout-ms <milliseconds>");
    }

    private static int ParseInt(string value)
    {
        if (value.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            return Convert.ToInt32(value[2..], 16);
        }

        return int.Parse(value, System.Globalization.CultureInfo.InvariantCulture);
    }

    private static CaptureStats CaptureBulkStream(CaptureOptions options)
    {
        using var context = new UsbContext();
        var device = context.List()
            .OfType<UsbDevice>()
            .FirstOrDefault(candidate =>
                candidate.Info.VendorId == options.VendorId &&
                candidate.Info.ProductId == options.ProductId);
        if (device is null)
        {
            throw new InvalidOperationException($"Device {options.VendorId:x4}:{options.ProductId:x4} not found");
        }

        device.Open();

        IUsbDevice? wholeDevice = device as IUsbDevice;
        var claimedInterface = -1;
        UsbEndpointReader? reader = null;
        var stopwatch = Stopwatch.StartNew();
        var totalBytes = 0L;
        var buffer = new byte[options.ReadSize];

        try
        {
            if (wholeDevice is not null)
            {
                wholeDevice.SetConfiguration(1);
                claimedInterface = FindInterfaceNumber(device, options.EndpointAddress);
                wholeDevice.ClaimInterface(claimedInterface);
            }

            reader = device.OpenEndpointReader((ReadEndpointID)options.EndpointAddress);
            var deadline = TimeSpan.FromSeconds(options.DurationSeconds);

            while (stopwatch.Elapsed < deadline)
            {
                var error = reader.Read(buffer, options.TimeoutMilliseconds, out var bytesRead);
                if (error == Error.Success)
                {
                    totalBytes += bytesRead;
                    continue;
                }

                if (error == Error.Timeout)
                {
                    continue;
                }

                throw new InvalidOperationException($"USB read failed: {error}");
            }
        }
        finally
        {
            if ((wholeDevice is not null) && (claimedInterface >= 0))
            {
                wholeDevice.ReleaseInterface(claimedInterface);
            }

            device.Close();
        }

        stopwatch.Stop();
        return new CaptureStats(totalBytes, stopwatch.Elapsed.TotalSeconds);
    }

    private static int FindInterfaceNumber(UsbDevice device, byte endpointAddress)
    {
        foreach (UsbConfigInfo configuration in device.Configs)
        {
            foreach (UsbInterfaceInfo interfaceInfo in configuration.Interfaces)
            {
                foreach (UsbEndpointInfo endpointInfo in interfaceInfo.Endpoints)
                {
                    if (endpointInfo.EndpointAddress == endpointAddress)
                    {
                        return interfaceInfo.Number;
                    }
                }
            }
        }

        throw new InvalidOperationException($"Endpoint 0x{endpointAddress:x2} not found on the device");
    }
}

internal static class Defaults
{
    public const int Vid = 0xCAFE;
    public const int Pid = 0x4003;
    public const byte EndpointAddress = 0x83;
    public const double DurationSeconds = 5.0;
    public const int ReadSize = 16384;
    public const int TimeoutMilliseconds = 250;
}

internal readonly record struct CaptureOptions(
    int VendorId = Defaults.Vid,
    int ProductId = Defaults.Pid,
    byte EndpointAddress = Defaults.EndpointAddress,
    double DurationSeconds = Defaults.DurationSeconds,
    int ReadSize = Defaults.ReadSize,
    int TimeoutMilliseconds = Defaults.TimeoutMilliseconds);

internal readonly record struct CaptureStats(long TotalBytes, double ElapsedSeconds)
{
    public double BytesPerSecond => ElapsedSeconds <= 0.0 ? 0.0 : TotalBytes / ElapsedSeconds;
}