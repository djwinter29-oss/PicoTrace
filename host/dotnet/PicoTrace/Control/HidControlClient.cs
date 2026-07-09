using LibUsbDotNet;
using LibUsbDotNet.Info;
using LibUsbDotNet.LibUsb;
using LibUsbDotNet.Main;
using PicoTrace.Trace;

namespace PicoTrace.Control;

public sealed class HidControlClient : IDisposable
{
    public const byte DefaultHidControlInEndpoint = 0x84;
    private const byte HidInputReport = 0x01;
    private const byte HidOutputReport = 0x02;
    private const byte HidRequestGetReport = 0x01;
    private const byte HidRequestSetReport = 0x09;
    private const int HidRequestTimeoutMilliseconds = 250;

    private readonly UsbContext _context;
    private readonly UsbDevice _device;
    private readonly int _interfaceNumber;
    private byte _nextSequence;

    private HidControlClient(UsbContext context, UsbDevice device, int interfaceNumber)
    {
        _context = context;
        _device = device;
        _interfaceNumber = interfaceNumber;
    }

    public static HidControlClient Open(
        int vendorId = UsbBulkTraceTransport.DefaultVid,
        int productId = UsbBulkTraceTransport.DefaultPid,
        byte hidInEndpoint = DefaultHidControlInEndpoint)
    {
        var (context, device, interfaceNumber) = OpenHidControlDevice(vendorId, productId, hidInEndpoint);
        return new HidControlClient(context, device, interfaceNumber);
    }

    public static (UsbContext Context, UsbDevice Device, int InterfaceNumber) OpenHidControlDevice(
        int vendorId = UsbBulkTraceTransport.DefaultVid,
        int productId = UsbBulkTraceTransport.DefaultPid,
        byte hidInEndpoint = DefaultHidControlInEndpoint)
    {
        var (context, device) = UsbBulkTraceTransport.FindUsbDevice(vendorId, productId);
        device.Open();

        var wholeDevice = device as IUsbDevice;
        var interfaceNumber = -1;
        try
        {
            wholeDevice?.SetConfiguration(1);
            interfaceNumber = FindHidInterfaceNumber(device, hidInEndpoint);
            wholeDevice?.ClaimInterface(interfaceNumber);
            return (context, device, interfaceNumber);
        }
        catch
        {
            CloseHidControlDevice(context, device, interfaceNumber);
            throw;
        }
    }

    public static void CloseHidControlDevice(UsbContext context, UsbDevice device, int interfaceNumber)
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

    public void Dispose() => CloseHidControlDevice(_context, _device, _interfaceNumber);

    public HidResponse Transact(HidCommand command)
    {
        var report = command.ToReportBytes();
        var writePacket = new UsbSetupPacket(
            (byte)(UsbCtrlFlags.Direction_Out | UsbCtrlFlags.RequestType_Class | UsbCtrlFlags.Recipient_Interface),
            HidRequestSetReport,
            (short)(HidOutputReport << 8),
            (short)_interfaceNumber,
            (short)report.Length);

        var written = _device.ControlTransfer(writePacket, report, 0, report.Length);
        if (written != HidProtocol.DefaultReportSize)
        {
            throw new HidProtocolException($"short HID write: expected {HidProtocol.DefaultReportSize}, wrote {written}");
        }

        var readBuffer = new byte[HidProtocol.DefaultReportSize];
        var readPacket = new UsbSetupPacket(
            (byte)(UsbCtrlFlags.Direction_In | UsbCtrlFlags.RequestType_Class | UsbCtrlFlags.Recipient_Interface),
            HidRequestGetReport,
            (short)(HidInputReport << 8),
            (short)_interfaceNumber,
            (short)readBuffer.Length);

        var read = _device.ControlTransfer(readPacket, readBuffer, 0, readBuffer.Length);
        if (read != HidProtocol.DefaultReportSize)
        {
            throw new HidProtocolException($"short HID read: expected {HidProtocol.DefaultReportSize}, read {read}");
        }

        var response = HidProtocol.DecodeHidResponse(readBuffer);
        if (response.Sequence != command.Sequence)
        {
            throw new HidProtocolException($"HID response sequence mismatch: expected {command.Sequence}, got {response.Sequence}");
        }

        if (response.Opcode != command.Opcode)
        {
            throw new HidProtocolException($"HID response opcode mismatch: expected {command.Opcode}, got {response.Opcode}");
        }

        return response;
    }

    public HidResponse Request(HidOpcode opcode, byte[]? payload = null, byte? sequence = null)
    {
        var actualSequence = sequence ?? _nextSequence;
        if (sequence is null)
        {
            _nextSequence += 1;
        }

        return Transact(new HidCommand((byte)opcode, actualSequence, payload ?? []));
    }

    public HidDeviceStatus GetStatus()
    {
        var response = RequireOk(Request(HidOpcode.GetStatus));
        return HidProtocol.DecodeDeviceStatusPayload(response.Payload);
    }

    public void SetStreamEnabled(bool enabled)
    {
        RequireOk(Request(enabled ? HidOpcode.StreamEnable : HidOpcode.StreamDisable));
    }

    public void SetLed(bool on)
    {
        RequireOk(Request(on ? HidOpcode.LedOn : HidOpcode.LedOff));
    }

    public void Reboot()
    {
        RequireOk(Request(HidOpcode.Reboot));
    }

    public void I2cSetRate(int channel, uint sampleHz)
    {
        RequireOk(Request(HidOpcode.I2cMonitorSetRate, HidProtocol.BuildI2cSetRatePayload(channel, sampleHz)));
    }

    public I2cMonitorStatus I2cGetStatus(int channel)
    {
        var response = RequireOk(Request(HidOpcode.I2cMonitorGetStatus, [(byte)channel]));
        return HidProtocol.DecodeI2cMonitorStatusPayload(response.Payload);
    }

    public IReadOnlyList<I2cMonitorAllStatus> I2cGetAllStatus()
    {
        var response = RequireOk(Request(HidOpcode.I2cMonitorGetAllStatus));
        return HidProtocol.DecodeI2cMonitorAllStatusPayload(response.Payload);
    }

    public void SpiSetConfig(int bus, SpiCaptureMode capture, int spiMode, int channelSelectMask, uint timeoutUs)
    {
        RequireOk(Request(HidOpcode.SpiMonitorSetConfig, HidProtocol.BuildSpiSetConfigPayload(bus, capture, spiMode, channelSelectMask, timeoutUs)));
    }

    public SpiMonitorStatus SpiGetStatus(int bus)
    {
        var response = RequireOk(Request(HidOpcode.SpiMonitorGetStatus, [(byte)bus]));
        return HidProtocol.DecodeSpiMonitorStatusPayload(response.Payload);
    }

    public IReadOnlyList<SpiMonitorAllStatus> SpiGetAllStatus()
    {
        var response = RequireOk(Request(HidOpcode.SpiMonitorGetAllStatus));
        return HidProtocol.DecodeSpiMonitorAllStatusPayload(response.Payload);
    }

    private static HidResponse RequireOk(HidResponse response)
    {
        if (response.Ok)
        {
            return response;
        }

        throw new HidControlException($"HID command failed with status {response.Status}");
    }

    private static int FindHidInterfaceNumber(UsbDevice device, byte hidInEndpoint)
    {
        foreach (UsbConfigInfo configuration in device.Configs)
        {
            foreach (UsbInterfaceInfo interfaceInfo in configuration.Interfaces)
            {
                if (interfaceInfo.Class != ClassCode.Hid)
                {
                    continue;
                }

                foreach (UsbEndpointInfo endpointInfo in interfaceInfo.Endpoints)
                {
                    if (endpointInfo.EndpointAddress == hidInEndpoint)
                    {
                        return interfaceInfo.Number;
                    }
                }
            }
        }

        throw new InvalidOperationException($"HID control endpoint 0x{hidInEndpoint:x2} not found on the device");
    }
}