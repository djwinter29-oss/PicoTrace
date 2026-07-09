using System.Diagnostics;
using LibUsbDotNet;
using LibUsbDotNet.Info;
using LibUsbDotNet.LibUsb;
using LibUsbDotNet.Main;

namespace PicoTrace.Trace;

public static class UsbBulkTraceTransport
{
    public const int DefaultVid = 0xCAFE;
    public const int DefaultPid = 0x4003;
    public const byte DefaultVendorInEndpoint = 0x83;
    public const double DefaultDurationSeconds = 20.0;
    public const int DefaultReadSize = 16384;
    public const int DefaultTimeoutMilliseconds = 250;

    public static (UsbContext Context, UsbDevice Device) FindUsbDevice(int vendorId, int productId)
    {
        var context = new UsbContext();
        var device = context.List()
            .OfType<UsbDevice>()
            .FirstOrDefault(candidate =>
                candidate.Info.VendorId == vendorId &&
                candidate.Info.ProductId == productId);
        if (device is null)
        {
            context.Dispose();
            throw new InvalidOperationException($"Device {vendorId:x4}:{productId:x4} not found");
        }

        return (context, device);
    }

    public static (UsbContext Context, UsbDevice Device, int InterfaceNumber) OpenTraceDevice(
        int vendorId = DefaultVid,
        int productId = DefaultPid,
        byte endpointAddress = DefaultVendorInEndpoint)
    {
        var (context, device) = FindUsbDevice(vendorId, productId);
        device.Open();

        var wholeDevice = device as IUsbDevice;
        var claimedInterface = -1;
        try
        {
            if (wholeDevice is not null)
            {
                wholeDevice.SetConfiguration(1);
                claimedInterface = FindInterfaceNumber(device, endpointAddress);
                wholeDevice.ClaimInterface(claimedInterface);
            }

            return (context, device, claimedInterface);
        }
        catch
        {
            CloseTraceDevice(context, device, claimedInterface);
            throw;
        }
    }

    public static void CloseTraceDevice(UsbContext context, UsbDevice device, int interfaceNumber)
    {
        try
        {
            if (device is IUsbDevice wholeDevice && interfaceNumber >= 0)
            {
                wholeDevice.ReleaseInterface(interfaceNumber);
            }
        }
        catch
        {
            // Best-effort close.
        }
        finally
        {
            try
            {
                device.Close();
            }
            finally
            {
                context.Dispose();
            }
        }
    }

    public static IEnumerable<TracePacket> IterTracePackets(
        double? durationSeconds = DefaultDurationSeconds,
        int vendorId = DefaultVid,
        int productId = DefaultPid,
        byte endpointAddress = DefaultVendorInEndpoint,
        int readSize = DefaultReadSize,
        int timeoutMilliseconds = DefaultTimeoutMilliseconds,
        TraceChannelRegistry? channelRegistry = null,
        Func<bool>? keepRunning = null,
        Action? onOpened = null)
    {
        if (durationSeconds is not null && durationSeconds <= 0.0)
        {
            throw new ArgumentOutOfRangeException(nameof(durationSeconds), "durationSeconds must be positive");
        }

        if (readSize <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(readSize), "readSize must be positive");
        }

        var (context, device, interfaceNumber) = OpenTraceDevice(vendorId, productId, endpointAddress);
        var decoder = new TraceStreamDecoder();
        var reader = device.OpenEndpointReader((ReadEndpointID)endpointAddress);
        var buffer = new byte[readSize];
        var deadline = durationSeconds is null ? (TimeSpan?)null : TimeSpan.FromSeconds(durationSeconds.Value);
        var stopwatch = Stopwatch.StartNew();
        onOpened?.Invoke();

        try
        {
            while (deadline is null || stopwatch.Elapsed < deadline.Value)
            {
                if (keepRunning is not null && !keepRunning())
                {
                    yield break;
                }

                var error = reader.Read(buffer, timeoutMilliseconds, out var bytesRead);
                if (error == Error.Timeout)
                {
                    continue;
                }

                if (error != Error.Success)
                {
                    throw new InvalidOperationException($"USB read failed: {error}");
                }

                if (bytesRead <= 0)
                {
                    continue;
                }

                foreach (var packet in decoder.Append(buffer.AsSpan(0, bytesRead)))
                {
                    if (channelRegistry is not null && !channelRegistry.MatchesPacket(packet))
                    {
                        continue;
                    }

                    yield return packet;
                }
            }
        }
        finally
        {
            CloseTraceDevice(context, device, interfaceNumber);
        }
    }

    public static IReadOnlyList<TracePacket> ReadTracePackets(
        double? durationSeconds = DefaultDurationSeconds,
        int vendorId = DefaultVid,
        int productId = DefaultPid,
        byte endpointAddress = DefaultVendorInEndpoint,
        int readSize = DefaultReadSize,
        int timeoutMilliseconds = DefaultTimeoutMilliseconds,
        TraceChannelRegistry? channelRegistry = null,
        Func<bool>? keepRunning = null,
        Action? onOpened = null)
    {
        return IterTracePackets(
                durationSeconds,
                vendorId,
                productId,
                endpointAddress,
                readSize,
                timeoutMilliseconds,
                channelRegistry,
                keepRunning,
                onOpened)
            .ToArray();
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