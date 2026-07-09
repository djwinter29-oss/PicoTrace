from __future__ import annotations

"""Channel-based filtering helpers for PicoTrace packet streams."""

from collections.abc import Iterable, Iterator

from .decode import TracePacket, TracePacketHeader


__all__ = ["TraceChannelRegistry"]


class TraceChannelRegistry:
    def __init__(self, channels: Iterable[int] | None = None) -> None:
        self._channels: set[int] = set()
        if channels is not None:
            for channel in channels:
                self.register_channel(channel)

    def register_channel(self, channel: int) -> None:
        self._channels.add(_normalize_channel(channel))

    def unregister_channel(self, channel: int) -> None:
        self._channels.discard(_normalize_channel(channel))

    def clear_channels(self) -> None:
        self._channels.clear()

    @property
    def registered_channels(self) -> frozenset[int]:
        return frozenset(self._channels)

    def matches_header(self, header: TracePacketHeader) -> bool:
        if not self._channels:
            return True
        return header.channel in self._channels

    def matches_packet(self, packet: TracePacket) -> bool:
        return self.matches_header(packet.header)

    def iter_packets(self, packets: Iterable[TracePacket]) -> Iterator[TracePacket]:
        for packet in packets:
            if self.matches_packet(packet):
                yield packet


def _normalize_channel(channel: int) -> int:
    if not isinstance(channel, int):
        raise TypeError("trace channel must be an integer")
    if channel < 0 or channel > 0xFF:
        raise ValueError("trace channel must be between 0 and 255")
    return channel