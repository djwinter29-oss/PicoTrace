# Documentation Index

This folder contains the project documentation for PicoTrace.

The documents are grouped by purpose so you can move from the high-level architecture view down to
component detail, then into hardware hookup and bench setup guidance.

## Architecture

Architecture documents describe the major firmware subsystems, core ownership, module interaction,
interfaces, synchronization, and race-avoidance strategy.

- [architecture/README.md](architecture/README.md): entry point for architecture-level documents
- [architecture/firmware-architecture.md](architecture/firmware-architecture.md): top-level firmware structure, data flow, and component interaction diagrams
- [architecture/interface-and-synchronization.md](architecture/interface-and-synchronization.md): interface contracts, synchronization mechanisms, and concurrency model

## Detail Design

Detail-design documents describe the behavior of individual subsystems and implementation-level
boundaries.

- [details/i2c-pio-sampler-design.md](details/i2c-pio-sampler-design.md): I2C sampling and decode-side design
- [details/spi-pio-monitor-design.md](details/spi-pio-monitor-design.md): SPI monitor design, bus ownership, and packetization intent
- [details/trace-ring-buffer-design.md](details/trace-ring-buffer-design.md): trace packet ring structure and handoff model
- [details/usb-multi-interface-design.md](details/usb-multi-interface-design.md): how CDC, HID, and vendor bulk share one USB device

## System Behavior

These documents explain broader cross-cutting runtime behavior.

- [streaming-design.md](streaming-design.md): streaming ownership and service-shaping guidance

## Benchmark Notes

- [testlog/rp2040-benchmark-baseline.md](testlog/rp2040-benchmark-baseline.md): current RP2040 benchmark baseline, reference results, and reusable benchmark guidance
- [testlog/rp2040-benchmark-testlog.md](testlog/rp2040-benchmark-testlog.md): dated RP2040 benchmark and I2C validation log, intentionally empty until real test runs are recorded
- [testlog/rp2040-benchmark-testlog-template.md](testlog/rp2040-benchmark-testlog-template.md): template for new RP2040 dated test-log entries

## Hardware And Bench Setup

These documents explain board wiring and practical bench usage.

- [hardware-connections.md](hardware-connections.md): observed pin mapping and board-side wiring guidance
- [raspberry-pi-test-setup.md](raspberry-pi-test-setup.md): Raspberry Pi based validation and hookup notes

## Suggested Reading Order

If you are new to the repository, the most efficient order is:

1. [architecture/firmware-architecture.md](architecture/firmware-architecture.md)
2. [architecture/interface-and-synchronization.md](architecture/interface-and-synchronization.md)
3. [details/usb-multi-interface-design.md](details/usb-multi-interface-design.md)
4. [details/trace-ring-buffer-design.md](details/trace-ring-buffer-design.md)
5. protocol-specific detail notes under [details/](details/)
