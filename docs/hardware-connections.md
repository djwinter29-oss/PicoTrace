# Hardware Connections

## Purpose

This document defines the PicoTrace pin allocation for Raspberry Pi Pico and Raspberry Pi Pico 2
carrier boards.

It is intentionally limited to board header pin use, GPIO allocation, and a few wiring constraints.
Protocol behavior and trace payload details belong in the protocol-specific documents.

## Board Assumption

This allocation is intended for boards that use the standard 40-pin Raspberry Pi Pico form factor:

- Raspberry Pi Pico based on RP2040
- Raspberry Pi Pico 2 based on RP2350

For this document, the important compatibility point is the board header exposure, not the internal
silicon package. The allocation only uses GPIOs that are available on the Pico and Pico 2 edge
headers.

Important board constraint:

- `GPIO23`, `GPIO24`, and `GPIO25` are not available on the standard Pico/Pico 2 header and should
  not be used in the board pin allocation

## Scope

The current allocation supports passive SPI and passive I2C observation.

Protocol-specific signal semantics and payload layouts remain in the corresponding trace-format documents:

- `docs/trace-formats/spi.md`
- `docs/trace-formats/i2c.md`

This document only covers how those observed signals map onto Pico-board header pins.

For a concrete Raspberry Pi bench hookup built on top of this allocation, see
`docs/raspberry-pi-test-setup.md`.

## SPI Allocation

The current SPI allocation supports observation of up to two SPI buses, with up to two observed
`CS_N` lines per bus.

| Pico/Pico 2 Header Pin | GPIO | Signal | Notes |
| --- | --- | --- | --- |
| `4` | `GPIO2` | `SPI0_SCLK` | SPI bus 0 clock input |
| `5` | `GPIO3` | `SPI0_MOSI` | SPI bus 0 controller-to-peripheral data input |
| `6` | `GPIO4` | `SPI0_MISO` | optional SPI bus 0 peripheral-to-controller data input |
| `7` | `GPIO5` | `SPI0_CS_N0` | first observed transaction boundary input on SPI bus 0 |
| `9` | `GPIO6` | `SPI0_CS_N1` | second observed chip-select input on SPI bus 0 |
| `10` | `GPIO7` | `SPI1_CS_N0` | first observed transaction boundary input on SPI bus 1 |
| `11` | `GPIO8` | `SPI1_CS_N1` | second observed chip-select input on SPI bus 1 |
| `12` | `GPIO9` | `SPI1_SCLK` | SPI bus 1 clock input |
| `14` | `GPIO10` | `SPI1_MOSI` | SPI bus 1 controller-to-peripheral data input |
| `15` | `GPIO11` | `SPI1_MISO` | optional SPI bus 1 peripheral-to-controller data input |

These are the recommended board allocations.

Important constraints:

- keep each SPI bus as one compact GPIO block
- keep the `CS_N` inputs grouped with their corresponding `SCLK`, `MOSI`, and optional `MISO`
- keep `GPIO0` and `GPIO1` free for UART/debug use unless there is a strong reason to reassign them

## I2C Allocation

The current I2C allocation supports observation of up to four I2C buses or channels.

The first three channels use compact adjacent GPIO pairs. The fourth channel uses `GPIO26` and
`GPIO27` because they are exposed on the Pico/Pico 2 header, while `GPIO23` through `GPIO25` are not.

| Pico/Pico 2 Header Pin | GPIO | Signal | Notes |
| --- | --- | --- | --- |
| `21` | `GPIO16` | `I2C0_SDA` | passive observation data input for I2C bus 0 |
| `22` | `GPIO17` | `I2C0_SCL` | passive observation clock input for I2C bus 0 |
| `24` | `GPIO18` | `I2C1_SDA` | passive observation data input for I2C bus 1 |
| `25` | `GPIO19` | `I2C1_SCL` | passive observation clock input for I2C bus 1 |
| `26` | `GPIO20` | `I2C2_SDA` | passive observation data input for I2C bus 2 |
| `27` | `GPIO21` | `I2C2_SCL` | passive observation clock input for I2C bus 2 |
| `31` | `GPIO26` | `I2C3_SDA` | passive observation data input for I2C bus 3; ADC-capable pin used as digital input |
| `32` | `GPIO27` | `I2C3_SCL` | passive observation clock input for I2C bus 3; ADC-capable pin used as digital input |

These are the recommended board allocations.

Important constraints:

- keep each I2C channel as an `SDA`/`SCL` pair
- preserve room for pull-up and level-shifting strategy appropriate to the observed target bus
- treat the PicoTrace connection as passive observation wiring, not as the bus pull-up source by default

The current I2C monitor scaffold samples each channel with its own PIO state machine, so the fourth
channel may remain on `GPIO26` and `GPIO27` without forcing the whole I2C allocation into one
contiguous 8-pin window.

## Reserved And Free GPIOs

Using the allocation above:

- `GPIO2` through `GPIO11` are assigned to SPI observation
- `GPIO16` through `GPIO21` plus `GPIO26` and `GPIO27` are assigned to I2C observation

That uses `18` board-accessible GPIOs for protocol observation.

Unassigned header-accessible GPIOs are:

- `GPIO0`
- `GPIO1`
- `GPIO12`
- `GPIO13`
- `GPIO14`
- `GPIO15`
- `GPIO22`
- `GPIO28`

Recommended reservation:

- keep `GPIO0` and `GPIO1` available for UART or bring-up logging
- keep at least one of `GPIO14`, `GPIO15`, `GPIO22`, or `GPIO28` free for future board support or protocol growth

## Logical Channel Map

The current allocation exposes `8` logical capture channels over the shared host-control model.

| `channel_id` | Protocol | Wiring Group | Local Slot | GPIOs |
| --- | --- | --- | --- | --- |
| `0x00` | SPI | `spi0` | `cs_slot = 0` | `SCLK=GPIO2`, `MOSI=GPIO3`, `MISO=GPIO4`, `CS_N=GPIO5` |
| `0x01` | SPI | `spi0` | `cs_slot = 1` | `SCLK=GPIO2`, `MOSI=GPIO3`, `MISO=GPIO4`, `CS_N=GPIO6` |
| `0x02` | SPI | `spi1` | `cs_slot = 0` | `SCLK=GPIO9`, `MOSI=GPIO10`, `MISO=GPIO11`, `CS_N=GPIO7` |
| `0x03` | SPI | `spi1` | `cs_slot = 1` | `SCLK=GPIO9`, `MOSI=GPIO10`, `MISO=GPIO11`, `CS_N=GPIO8` |
| `0x04` | I2C | `i2c0` | primary pair | `SDA=GPIO16`, `SCL=GPIO17` |
| `0x05` | I2C | `i2c1` | primary pair | `SDA=GPIO18`, `SCL=GPIO19` |
| `0x06` | I2C | `i2c2` | primary pair | `SDA=GPIO20`, `SCL=GPIO21` |
| `0x07` | I2C | `i2c3` | primary pair | `SDA=GPIO26`, `SCL=GPIO27` |

This table is the board-level bridge between Pico/Pico 2 header wiring and PicoTrace logical
channel selection.

## Electrical Guidance

- keep all PicoTrace inputs high-impedance
- connect grounds between PicoTrace and the observed target
- keep tap wires short
- use level translation when the target bus is not compatible with RP2040 input levels
- avoid wiring choices that materially load or distort the observed bus

This document describes recommended wiring only. It does not change the passive-sniffer architectural rule that PicoTrace should observe traffic without acting as a bus participant.
