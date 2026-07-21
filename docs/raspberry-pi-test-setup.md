# Raspberry Pi Test Setup

## Purpose

This document describes one concrete Raspberry Pi to PicoTrace bench setup.

It is a practical wiring example for bring-up and validation. The authoritative Pico and Pico 2 pin
allocation remains in `docs/hardware-connections.md`.

## Scope

This setup assumes:

- Raspberry Pi Pico or Raspberry Pi Pico 2 running PicoTrace firmware
- a Raspberry Pi host used both as the USB host and as the active I2C or SPI traffic generator
- passive observation only on the PicoTrace side

This bench example uses:

- PicoTrace I2C channel `0`
- PicoTrace SPI channel `0x00`

## Host, Debug, And Console Links

| Raspberry Pi Side | PicoTrace Side | Purpose |
| --- | --- | --- |
| USB host port | Pico USB port | USB connection for firmware bring-up, host communication, and trace streaming |
| Debug Probe SWD | Pico debug port | programming and debug access |
| Debug Probe UART TX/RX | `GPIO0` / `GPIO1` | optional UART console during bring-up |
| Ground | Ground | common reference between Raspberry Pi, debug probe, and PicoTrace |

Keep all grounds common when using USB, SWD, and UART at the same time.

## Active Protocol Wiring

This bench wiring maps one Raspberry Pi I2C bus and one Raspberry Pi SPI bus into the default
PicoTrace allocation.

### I2C Wiring

| Raspberry Pi Signal | Raspberry Pi Header Pin | PicoTrace GPIO | PicoTrace Signal | `channel_id` |
| --- | --- | --- | --- | --- |
| `I2C1_SDA` | pin `3` | `GPIO16` | `I2C0_SDA` | `0` |
| `I2C1_SCL` | pin `5` | `GPIO17` | `I2C0_SCL` | `0` |
| Ground | any ground pin | Ground | Ground | n/a |

This maps the Raspberry Pi primary I2C bus onto PicoTrace logical I2C channel `0`.

### SPI Wiring

| Raspberry Pi Signal | Raspberry Pi Header Pin | PicoTrace GPIO | PicoTrace Signal | `channel_id` |
| --- | --- | --- | --- | --- |
| `SPI0_SCLK` | pin `23` | `GPIO2` | `SPI0_SCLK` | `0x00` |
| `SPI0_MOSI` | pin `19` | `GPIO3` | `SPI0_MOSI` | `0x00` |
| `SPI0_MISO` | pin `21` | `GPIO4` | `SPI0_MISO` | `0x00` |
| `SPI0_CE0_N` | pin `24` | `GPIO5` | `SPI0_CS_N0` | `0x00` |
| Ground | any ground pin | Ground | Ground | n/a |

This maps Raspberry Pi `SPI0` using `CE0` onto PicoTrace logical SPI channel `0x00`.

## Wiring Notes

- PicoTrace must observe these lines passively and must not drive the target bus.
- Keep the tap wires short.
- Use a shared ground between the Raspberry Pi and PicoTrace.
- If the observed target bus is not at RP2040-compatible voltage levels, add appropriate level translation.
- For I2C, let the active target bus provide the pull-ups unless the bench setup explicitly requires something else.

## Raspberry Pi Preconditions

Before generating traffic, confirm that the Raspberry Pi interfaces used by this setup are enabled.

Expected device nodes:

- `/dev/i2c-1`
- `/dev/spidev0.0`

Example check:

```bash
ls /dev/i2c-1 /dev/spidev0.0
```

If those device nodes are missing, enable I2C and SPI in Raspberry Pi OS configuration first.

## Generating I2C Traffic

The bench I2C wiring above feeds Raspberry Pi `I2C1` into PicoTrace channel `0x06`.

Useful commands on Raspberry Pi OS include:

```bash
sudo i2cdetect -y 1
```

This produces address-probe traffic on `I2C1`.

If you want a repo-local helper that configures the PicoTrace I2C monitor over the board CDC CLI,
runs one `i2cdetect -y 1` scan, and checks that the trace contains the expected `112`
transactions:

```bash
./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112
```

The `sample_hz` value is the PicoTrace I2C sampler rate passed to the board-local `i2cmon`
command, not the Linux bus speed. Older live firmware builds may only accept sampler rates such as
`4000000`, `8000000`, or `12000000`.

```bash
sudo i2ctransfer -y 1 w3@0x50 0x00 0xAA 0x55
```

This produces one write transaction on `I2C1`.

```bash
sudo i2ctransfer -y 1 w1@0x50 0x00 r4
```

This produces a combined write-then-read transaction and is useful for exercising repeated-start
behavior.

If no target device is present at the selected address, the Raspberry Pi can still generate
observable address and clock traffic, but the transaction may end with a NACK.

## Generating SPI Traffic

The bench SPI wiring above feeds Raspberry Pi `SPI0` with `CE0` into PicoTrace channel `0x00`.

If `spidev_test` is available:

```bash
spidev_test -D /dev/spidev0.0 -s 500000 -p "\x9A\xBC\xDE\xF0" -v
```

This asserts `CE0` and clocks four bytes on `MOSI`.

If you want a repo-local helper that does the same kind of framed transfer without relying on the
`spidev` Python package:

```bash
python3 ./tools/linux/spi_transfer.py 01 02 03 04
```

For repeated transfers during PicoTrace capture tests:

```bash
python3 ./tools/linux/spi_transfer.py 01 02 03 04 --repeat 10 --interval-ms 250
```

For a larger fixed-pattern transfer, repeat the same base bytes until a requested transfer size is
reached:

```bash
python3 ./tools/linux/spi_transfer.py 01 02 03 04 --total-bytes 256
```

If you omit the payload, the helper uses the default fixed pattern `00` through `FF` and repeats it:

```bash
python3 ./tools/linux/spi_transfer.py --total-bytes 256
```

If `spidev_test` is not available, a small Python example works as well:

```python
import spidev

spi = spidev.SpiDev()
spi.open(0, 0)
spi.max_speed_hz = 500000
spi.mode = 0
response = spi.xfer2([0x9A, 0xBC, 0xDE, 0xF0])
print(response)
spi.close()
```

If no SPI peripheral is connected, PicoTrace can still observe `CS_N`, `SCLK`, and `MOSI`. In that
case, `MISO` data may be undefined because no target is driving the return line.

## Validation Use

Use this setup when you want a simple repeatable bench arrangement to validate:

- passive I2C observation on channel `0`
- passive SPI observation on channel `0x00`
- USB transport, firmware bring-up, and physical wiring at the same time

## Flash And Immediate Bring-Up

On Linux, `./tools/linux/load.sh` programs the firmware over the Debug Probe and then sends a
best-effort CDC `reboot` before returning. The script also waits for the CDC device to disappear
and re-enumerate, so you can normally start using the CLI immediately after the command exits.

Typical quick bring-up on this bench is:

```bash
./tools/linux/load.sh
```

```bash
python3 - <<'PY'
import glob, os, select, termios, time

port = glob.glob('/dev/serial/by-id/usb-PicoTrace_*if00')[0]
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[0] = 0
attrs[1] = 0
attrs[3] = 0
attrs[2] = attrs[2] | termios.CREAD | termios.CLOCAL
attrs[6][termios.VMIN] = 0
attrs[6][termios.VTIME] = 1
termios.tcsetattr(fd, termios.TCSANOW, attrs)
termios.tcflush(fd, termios.TCIOFLUSH)

def transact(cmd, settle=0.4, window=2.0):
    os.write(fd, cmd)
    time.sleep(settle)
    out = b''
    end = time.time() + window
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if ready:
            chunk = os.read(fd, 4096)
            if chunk:
                out += chunk
    return out.decode('utf-8', 'replace').strip()

print(transact(b'version\r\n'))
print(transact(b'stream on\r\n'))
print(transact(b'i2cmon 0 400000\r\n'))
os.close(fd)
PY
```
