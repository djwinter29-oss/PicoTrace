# Architecture Documents

This folder contains architecture-level design notes for PicoTrace.

These documents sit above the implementation detail notes in `docs/details/`.

Use the architecture notes when you want to understand:

- the major subsystems in the firmware
- which core owns which responsibility
- how control, capture, packetization, and USB streaming interact
- which interfaces are stable boundaries between modules
- how synchronization works across the current multicore design

Current architecture documents:

- [firmware-architecture.md](firmware-architecture.md): top-level firmware structure, component relationships, and end-to-end data flow
- [interface-and-synchronization.md](interface-and-synchronization.md): interface contracts, synchronization mechanisms, and race-condition avoidance strategy

Supporting implementation detail notes remain under `docs/details/`, including:

- `i2c-pio-sampler-design.md`
- `spi-pio-monitor-design.md`
- `trace-ring-buffer-design.md`
- `usb-multi-interface-design.md`