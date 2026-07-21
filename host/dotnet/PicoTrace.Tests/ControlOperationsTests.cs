using Microsoft.VisualStudio.TestTools.UnitTesting;
using PicoTrace.App;
using PicoTrace.Control;
using PicoTrace.Trace;

namespace PicoTrace.Tests;

[TestClass]
public sealed class ControlOperationsTests
{
    [TestMethod]
    public void BuildSpiApplyConfig_PreservesExistingBusMask()
    {
        var currentStatus = new SpiMonitorStatus(
            Bus: 1,
            Initialized: true,
            Running: true,
            Capture: SpiCaptureMode.Mosi,
            SpiMode: 1,
            ChannelSelectMask: 0x01,
            TimeoutUs: 50,
            PacketsEmitted: 0,
            OverrunCount: 0,
            SinkOverrunCount: 0,
            SamplerOverrunCount: 0,
            RingDropCount: 0,
            UsbStallCount: 0,
            UsbHostBackpressureStallCount: 0,
            UsbPolicyDeferralCount: 0,
            PeakRingDepthPackets: 0);

        var apply = ControlOperations.BuildSpiApplyConfig(4, SpiCaptureMode.MosiMiso, 3, 250, currentStatus);

        Assert.AreEqual(1, apply.Bus);
        Assert.AreEqual(SpiCaptureMode.MosiMiso, apply.Capture);
        Assert.AreEqual(3, apply.SpiMode);
        Assert.AreEqual(0x03, apply.ChannelSelectMask);
        Assert.AreEqual((uint)250, apply.TimeoutUs);
    }

    [TestMethod]
    public void BuildSpiStopConfig_ClearsOnlyRequestedLogicalChannel()
    {
        var currentStatus = new SpiMonitorStatus(
            Bus: 1,
            Initialized: true,
            Running: true,
            Capture: SpiCaptureMode.MosiMiso,
            SpiMode: 2,
            ChannelSelectMask: 0x03,
            TimeoutUs: 100,
            PacketsEmitted: 0,
            OverrunCount: 0,
            SinkOverrunCount: 0,
            SamplerOverrunCount: 0,
            RingDropCount: 0,
            UsbStallCount: 0,
            UsbHostBackpressureStallCount: 0,
            UsbPolicyDeferralCount: 0,
            PeakRingDepthPackets: 0);

        var stop = ControlOperations.BuildSpiStopConfig(3, currentStatus);

        Assert.AreEqual(1, stop.Bus);
        Assert.AreEqual(SpiCaptureMode.MosiMiso, stop.Capture);
        Assert.AreEqual(2, stop.SpiMode);
        Assert.AreEqual(0x02, stop.ChannelSelectMask);
        Assert.AreEqual((uint)100, stop.TimeoutUs);
    }

    [TestMethod]
    public void BuildSpiStopConfig_DisablesBusWhenLastLogicalChannelStops()
    {
        var currentStatus = new SpiMonitorStatus(
            Bus: 0,
            Initialized: true,
            Running: true,
            Capture: SpiCaptureMode.Mosi,
            SpiMode: 0,
            ChannelSelectMask: 0x01,
            TimeoutUs: 20,
            PacketsEmitted: 0,
            OverrunCount: 0,
            SinkOverrunCount: 0,
            SamplerOverrunCount: 0,
            RingDropCount: 0,
            UsbStallCount: 0,
            UsbHostBackpressureStallCount: 0,
            UsbPolicyDeferralCount: 0,
            PeakRingDepthPackets: 0);

        var stop = ControlOperations.BuildSpiStopConfig(0, currentStatus);

        Assert.AreEqual(0, stop.Bus);
        Assert.AreEqual(SpiCaptureMode.Disabled, stop.Capture);
        Assert.AreEqual(0, stop.SpiMode);
        Assert.AreEqual(0, stop.ChannelSelectMask);
        Assert.AreEqual((uint)0, stop.TimeoutUs);
    }

    [TestMethod]
    public void BuildSpiApplyConfig_StartsWithSingleChannelMaskWhenNoCurrentStatusExists()
    {
        var apply = ControlOperations.BuildSpiApplyConfig(4, SpiCaptureMode.MosiMiso, 3, 250, null);

        Assert.AreEqual(1, apply.Bus);
        Assert.AreEqual(SpiCaptureMode.MosiMiso, apply.Capture);
        Assert.AreEqual(3, apply.SpiMode);
        Assert.AreEqual(0x02, apply.ChannelSelectMask);
        Assert.AreEqual((uint)250, apply.TimeoutUs);
    }
}