# .NET Host Design Notes

These notes describe the current implementation shape of the PicoTrace .NET host.

- [Trace Design](trace-design.md)
- [Control Design](control-design.md)
- [App Design](app-design.md)

The .NET host mirrors the same high-level split as the Python host:

- `Trace/` owns packet framing, decode, filtering, and the bulk transport
- `Control/` owns HID protocol packing/decoding and the HID client
- `App/` owns CLI-oriented control flow and trace printing

The goal of this split is the same in both host implementations: keep packet and HID protocol logic reusable, keep USB interaction bounded to transport modules, and keep operator workflow concerns in the CLI layer instead of mixing them into the protocol code.