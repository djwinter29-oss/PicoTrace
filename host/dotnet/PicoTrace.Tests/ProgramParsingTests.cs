using System.Reflection;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace PicoTrace.Tests;

[TestClass]
public sealed class ProgramParsingTests
{
    [TestMethod]
    public void ParseI2cOptions_RejectsNegativeSampleRate()
    {
        var exception = Assert.ThrowsException<TargetInvocationException>(() => InvokePrivate("ParseI2cOptions", [new[] { "--channel", "0", "--sample-hz", "-1" }]));

        Assert.IsInstanceOfType<ArgumentException>(exception.InnerException);
        StringAssert.Contains(exception.InnerException!.Message, "--sample-hz must be an unsigned integer");
    }

    [TestMethod]
    public void ParseSpiOptions_RejectsNegativeTimeout()
    {
        var exception = Assert.ThrowsException<TargetInvocationException>(() => InvokePrivate("ParseSpiOptions", [new[] { "--channel", "0", "--timeout-us", "-1" }]));

        Assert.IsInstanceOfType<ArgumentException>(exception.InnerException);
        StringAssert.Contains(exception.InnerException!.Message, "--timeout-us must be an unsigned integer");
    }

    [TestMethod]
    public void ParseTraceOptions_AcceptsAll()
    {
        var result = InvokePrivate("ParseTraceOptions", [new[] { "--all" }]);

        Assert.IsNotNull(result);
        var all = (bool)result.GetType().GetProperty("All")!.GetValue(result)!;
        var channel = result.GetType().GetProperty("Channel")!.GetValue(result);
        Assert.IsTrue(all);
        Assert.IsNull(channel);
    }

    [TestMethod]
    public void ParseTraceOptions_RejectsAllAndChannelTogether()
    {
        var exception = Assert.ThrowsException<TargetInvocationException>(() => InvokePrivate("ParseTraceOptions", [new[] { "--all", "--channel", "0" }]));

        Assert.IsInstanceOfType<ArgumentException>(exception.InnerException);
        StringAssert.Contains(exception.InnerException!.Message, "either --channel or --all");
    }

    [TestMethod]
    public void ParseTraceOptions_RejectsMissingSelection()
    {
        var exception = Assert.ThrowsException<TargetInvocationException>(() => InvokePrivate("ParseTraceOptions", [Array.Empty<string>()]));

        Assert.IsInstanceOfType<ArgumentException>(exception.InnerException);
        StringAssert.Contains(exception.InnerException!.Message, "either --channel <0-255> or --all");
    }

    private static object? InvokePrivate(string methodName, object?[] arguments)
    {
        var programType = typeof(PicoTrace.Program);
        var method = programType.GetMethod(methodName, BindingFlags.NonPublic | BindingFlags.Static);
        Assert.IsNotNull(method);
        return method.Invoke(null, arguments);
    }
}