namespace PicoTrace.Trace;

public sealed class TraceChannelRegistry
{
    private readonly HashSet<int> _channels = [];

    public TraceChannelRegistry(IEnumerable<int>? channels = null)
    {
        if (channels is null)
        {
            return;
        }

        foreach (var channel in channels)
        {
            RegisterChannel(channel);
        }
    }

    public void RegisterChannel(int channel) => _channels.Add(NormalizeChannel(channel));

    public void UnregisterChannel(int channel) => _channels.Remove(NormalizeChannel(channel));

    public void ClearChannels() => _channels.Clear();

    public IReadOnlyCollection<int> RegisteredChannels => _channels.ToArray();

    public bool MatchesHeader(TracePacketHeader header) => _channels.Count == 0 || _channels.Contains(header.Channel);

    public bool MatchesPacket(TracePacket packet) => MatchesHeader(packet.Header);

    public IEnumerable<TracePacket> FilterPackets(IEnumerable<TracePacket> packets)
    {
        foreach (var packet in packets)
        {
            if (MatchesPacket(packet))
            {
                yield return packet;
            }
        }
    }

    private static int NormalizeChannel(int channel)
    {
        if (channel is < 0 or > 0xFF)
        {
            throw new ArgumentOutOfRangeException(nameof(channel), "trace channel must be between 0 and 255");
        }

        return channel;
    }
}