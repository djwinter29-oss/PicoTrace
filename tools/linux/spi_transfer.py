#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ctypes
import fcntl
import os
import time


SPI_IOC_WR_MODE = 0x40016B01
SPI_IOC_WR_BITS_PER_WORD = 0x40016B03
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04
SPI_IOC_MESSAGE_1 = 0x40206B00
DEFAULT_PAYLOAD_PATTERN = bytes(range(0x100))


class SpiIocTransfer(ctypes.Structure):
    _fields_ = [
        ("tx_buf", ctypes.c_uint64),
        ("rx_buf", ctypes.c_uint64),
        ("len", ctypes.c_uint32),
        ("speed_hz", ctypes.c_uint32),
        ("delay_usecs", ctypes.c_uint16),
        ("bits_per_word", ctypes.c_uint8),
        ("cs_change", ctypes.c_uint8),
        ("tx_nbits", ctypes.c_uint8),
        ("rx_nbits", ctypes.c_uint8),
        ("word_delay_usecs", ctypes.c_uint8),
        ("pad", ctypes.c_uint8),
    ]


def parse_payload_tokens(tokens: list[str]) -> bytes:
    values: list[int] = []

    for token in tokens:
        for part in token.split(","):
            item = part.strip()
            if not item:
                continue

            if item.startswith("0x") or item.startswith("0X"):
                value = int(item, 16)
            elif len(item) > 2 and all(char in "0123456789abcdefABCDEF" for char in item):
                if len(item) % 2 != 0:
                    raise ValueError(f"hex payload token must have an even number of digits: {item}")
                values.extend(int(item[index:index + 2], 16) for index in range(0, len(item), 2))
                continue
            else:
                value = int(item, 16)

            if not 0 <= value <= 0xFF:
                raise ValueError(f"payload byte out of range: {item}")
            values.append(value)

    if not values:
        raise ValueError("payload is empty")

    return bytes(values)


def build_transfer_payload(pattern: bytes, total_bytes: int | None) -> bytes:
    if total_bytes is None:
        return pattern

    if total_bytes <= 0:
        raise ValueError("total transfer bytes must be positive")

    repeats, remainder = divmod(total_bytes, len(pattern))
    return (pattern * repeats) + pattern[:remainder]


def format_bytes(data: bytes) -> str:
    return " ".join(f"{byte:02X}" for byte in data)


def transfer_once(
    device_path: str,
    payload: bytes,
    *,
    speed_hz: int,
    spi_mode: int,
    bits_per_word: int,
    delay_usecs: int,
) -> bytes:
    fd = os.open(device_path, os.O_RDWR)
    try:
        fcntl.ioctl(fd, SPI_IOC_WR_MODE, bytes([spi_mode]))
        fcntl.ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, bytes([bits_per_word]))
        fcntl.ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, speed_hz.to_bytes(4, "little"))

        tx = (ctypes.c_ubyte * len(payload))(*payload)
        rx = (ctypes.c_ubyte * len(payload))()
        transfer = SpiIocTransfer(
            tx_buf=ctypes.addressof(tx),
            rx_buf=ctypes.addressof(rx),
            len=len(payload),
            speed_hz=speed_hz,
            delay_usecs=delay_usecs,
            bits_per_word=bits_per_word,
            cs_change=0,
            tx_nbits=0,
            rx_nbits=0,
            word_delay_usecs=0,
            pad=0,
        )
        fcntl.ioctl(fd, SPI_IOC_MESSAGE_1, bytes(transfer))
        return bytes(rx)
    finally:
        os.close(fd)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate repeatable SPI transfers on Linux for PicoTrace bench testing.",
    )
    parser.add_argument(
        "payload",
        nargs="*",
        help="optional base payload pattern in hex, for example: 01 02 03 04 or 01020304 or 01,02,03,04",
    )
    parser.add_argument(
        "--total-bytes",
        type=int,
        help="repeat the base payload pattern until this many transfer bytes are generated; uses 00 through FF when payload is omitted",
    )
    parser.add_argument("--device", default="/dev/spidev0.0", help="SPI device path")
    parser.add_argument("--speed-hz", type=int, default=5000000, help="SPI clock rate in Hz")
    parser.add_argument("--mode", type=int, choices=range(4), default=0, help="SPI mode 0-3")
    parser.add_argument("--bits-per-word", type=int, default=8, help="bits per SPI word")
    parser.add_argument("--delay-usecs", type=int, default=0, help="inter-transfer delay in microseconds")
    parser.add_argument("--repeat", type=int, default=1, help="number of times to send the payload")
    parser.add_argument("--interval-ms", type=float, default=0.0, help="delay between repeated transfers in milliseconds")
    parser.add_argument("--quiet", action="store_true", help="suppress per-transfer output")
    return parser


def main() -> int:
    parser = build_argument_parser()
    args = parser.parse_args()

    try:
        base_payload = parse_payload_tokens(args.payload) if args.payload else DEFAULT_PAYLOAD_PATTERN
        payload = build_transfer_payload(base_payload, args.total_bytes)
    except ValueError as exc:
        parser.error(str(exc))

    if not args.payload and args.total_bytes is None:
        parser.error("payload is required unless --total-bytes is used")

    for index in range(args.repeat):
        response = transfer_once(
            args.device,
            payload,
            speed_hz=args.speed_hz,
            spi_mode=args.mode,
            bits_per_word=args.bits_per_word,
            delay_usecs=args.delay_usecs,
        )
        if not args.quiet:
            print(
                f"transfer {index + 1}/{args.repeat}: tx=[{format_bytes(payload)}] rx=[{format_bytes(response)}]"
            )

        if (index + 1) < args.repeat and args.interval_ms > 0.0:
            time.sleep(args.interval_ms / 1000.0)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())