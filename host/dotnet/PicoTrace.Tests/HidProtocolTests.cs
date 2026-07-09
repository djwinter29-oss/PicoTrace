using Microsoft.VisualStudio.TestTools.UnitTesting;
using PicoTrace.Control;
using PicoTrace.Trace;

namespace PicoTrace.Tests;

[TestClass]
public sealed class HidProtocolTests
{
    [TestMethod]
    public void HidCommand_ToReportBytes_EncodesFixedSizeReport()
    {
        var command = new HidCommand((byte)HidOpcode.StreamEnable, 7, [0xAA, 0xBB]);

        var report = command.ToReportBytes();

        Assert.AreEqual(HidProtocol.DefaultReportSize, report.Length);
        Assert.AreEqual((byte)HidOpcode.StreamEnable, report[0]);
        Assert.AreEqual((byte)7, report[1]);
        Assert.AreEqual((byte)2, report[3]);
        Assert.AreEqual((byte)0xAA, report[4]);
        Assert.AreEqual((byte)0xBB, report[5]);
    }

    [TestMethod]
    public void DecodeHidResponse_DecodesStatusAndPayload()
    {
        var report = new byte[HidProtocol.DefaultReportSize];
        report[0] = (byte)HidOpcode.GetStatus;
        report[1] = 9;
        report[2] = (byte)HidStatus.Ok;
        report[3] = 8;
        report[4] = 1;
        report[5] = 6;
        report[6] = (byte)'v';
        report[7] = (byte)'1';
        report[8] = (byte)'.';
        report[9] = (byte)'2';
        report[10] = (byte)'.';
        report[11] = (byte)'3';

        var response = HidProtocol.DecodeHidResponse(report);
        var status = HidProtocol.DecodeDeviceStatusPayload(response.Payload);

        Assert.IsTrue(response.Ok);
        Assert.IsTrue(status.StreamEnabled);
        Assert.AreEqual("v1.2.3", status.FirmwareVersion);
    }

    [TestMethod]
    public void DecodeDeviceStatusPayload_AcceptsLegacyStreamOnlyPayload()
    {
        var status = HidProtocol.DecodeDeviceStatusPayload(new byte[] { 1 });

        Assert.IsTrue(status.StreamEnabled);
        Assert.AreEqual(string.Empty, status.FirmwareVersion);
    }

    [TestMethod]
    public void BuildI2cSetRatePayload_EncodesLittleEndianRate()
    {
        var payload = HidProtocol.BuildI2cSetRatePayload(2, 1_000_000);

        CollectionAssert.AreEqual(new byte[] { 2, 0x40, 0x42, 0x0F, 0x00 }, payload);
    }

    [TestMethod]
    public void BuildSpiSetConfigPayload_EncodesExpectedFields()
    {
        var payload = HidProtocol.BuildSpiSetConfigPayload(1, SpiCaptureMode.MosiMiso, 3, 0x04, 250);

        CollectionAssert.AreEqual(new byte[] { 1, 2, 3, 0x04, 0xFA, 0x00, 0x00, 0x00 }, payload);
    }

    [TestMethod]
    public void DecodeSpiMonitorAllStatusPayload_DecodesTypedStatuses()
    {
        var payload = new byte[]
        {
            0, 1, 1, 2, 3, 100, 0, 0, 0, 0,
            1, 1, 0, 1, 0, 250, 0, 0, 0, 1,
        };

        var statuses = HidProtocol.DecodeSpiMonitorAllStatusPayload(payload);

        Assert.AreEqual(2, statuses.Count);
        Assert.AreEqual((byte)0, statuses[0].Bus);
        Assert.AreEqual(SpiCaptureMode.MosiMiso, statuses[0].Capture);
        Assert.AreEqual((uint)250, statuses[1].TimeoutUs);
        Assert.IsTrue(statuses[1].Overrun);
    }

    [TestMethod]
    public void BuildI2cSetRatePayload_RejectsOutOfRangeChannel()
    {
        Assert.ThrowsException<ArgumentOutOfRangeException>(() => HidProtocol.BuildI2cSetRatePayload(-1, 10));
        Assert.ThrowsException<ArgumentOutOfRangeException>(() => HidProtocol.BuildI2cSetRatePayload(256, 10));
    }

    [TestMethod]
    public void BuildSpiSetConfigPayload_RejectsInvalidInputs()
    {
        Assert.ThrowsException<ArgumentOutOfRangeException>(() => HidProtocol.BuildSpiSetConfigPayload(-1, SpiCaptureMode.Mosi, 0, 0x01, 10));
        Assert.ThrowsException<ArgumentOutOfRangeException>(() => HidProtocol.BuildSpiSetConfigPayload(0, SpiCaptureMode.Mosi, 4, 0x01, 10));
        Assert.ThrowsException<ArgumentOutOfRangeException>(() => HidProtocol.BuildSpiSetConfigPayload(0, SpiCaptureMode.Mosi, 0, 256, 10));
    }
}