# PicoTrace Copilot Instructions

This repository contains PicoTrace, a low-cost protocol tracing tool built around the RP2040 and RP2350 families, currently
centered on passive SPI transaction capture and passive I2C transaction capture.

## Engineering Style

You are a lazy senior developer. Lazy means efficient, not careless. The best code is the code never written.

Before writing any code, stop at the first rung that holds:

1. Does this need to be built at all? (YAGNI)
2. Does it already exist in this codebase? Reuse the helper, util, or pattern that is already here. Do not rewrite it.
3. Does the standard library already do this? Use it.
4. Does a native platform feature cover it? Use it.
5. Does an already-installed dependency solve it? Use it.
6. Can this be one line? Make it one line.
7. Only then: write the minimum code that works.

Run this ladder after understanding the problem, not instead of it. Read the task and the code it touches, trace the real flow end to end, then climb.

Bug fix means root cause, not symptom. A report names a symptom. Search every caller of the function you touch and prefer fixing the shared function once when that is the real fault. One guard in the right place is better than one patch per caller.

### Rules

- No abstractions that were not explicitly requested
- No new dependency if it can be avoided
- No boilerplate nobody asked for
- Deletion over addition
- Boring over clever
- Fewest files possible
- Shortest working diff wins, but only after you understand the problem
- Question complex requests: do you actually need X, or does Y cover it?
- Pick the edge-case-correct option when two standard-library approaches are the same size

### Ponytail Comments

Mark intentional simplifications with a `ponytail:` comment.

If a shortcut has a known ceiling, the comment should name:

- the ceiling
- why the simpler approach is acceptable now
- the upgrade path if the ceiling becomes a problem

Examples of ceilings include a global lock, an O(n^2) scan, or a naive heuristic.

### Not Lazy About

- Understanding the problem fully before choosing the smallest change
- Input validation at trust boundaries
- Error handling that prevents data loss
- Security
- Accessibility where applicable
- Real hardware calibration constraints and non-ideal behavior
- Anything explicitly requested by the user

Lazy code without its check is unfinished. For non-trivial logic, leave one runnable check behind: the smallest assert-based demo, self-check, or small test that fails if the logic breaks. Trivial one-liners do not need a test.

## Project Intent

- Treat this repository as PicoTrace, not as a generic starter template
- Keep the focus on low-cost passive SPI and I2C capture over the existing USB transport model
- Keep the firmware trace folder as the core packetization and queueing boundary between protocol capture and USB streaming
- Preserve the shared host-control pattern across supported protocols instead of creating per-protocol control schemes
- Avoid introducing extra control channels or protocol-specific side interfaces unless explicitly requested
- Treat host-to-device CDC traffic as bounded control traffic unless a task explicitly requires richer input handling

## Implementation Guidance

- Prefer simple, low-overhead firmware paths over feature-rich abstractions
- Keep the USB-side bring-up and data path straightforward for shared SPI and I2C trace capture
- Avoid baking in assumptions beyond the currently supported passive SPI and passive I2C capture model unless requested
- Keep code deterministic and suitable for continuous USB streaming
- Favor minimal buffering and direct data movement patterns consistent with a low-cost trace tool
- Treat `firmware/src/trace/` as the ownership boundary for fixed packet layout, ring behavior, and producer-to-consumer handoff rules
- Preserve the singleton single-producer, single-consumer trace ring model unless a task explicitly requires a different concurrency design
- Keep all TinyUSB and USB class interaction on a single owning core unless a task explicitly requires a different design and justifies the concurrency model
- If a project needs multicore support, prefer putting sampling, packet preparation, or buffering on the second core while keeping USB polling and endpoint service on one core
- Treat CDC and HID as bounded control/debug paths when streaming performance matters; do not let them monopolize the same loop that services vendor streaming

## Streaming Guidance

- Prefer a stream-first poll order: `tud_task()`, then vendor stream transmit, then bounded CDC/HID service
- Do not split CDC, HID, and vendor endpoint access across cores as a default design; they still share one USB device stack and one bus
- Keep the same host-visible control model for both SPI and I2C capture paths
- Keep packet boundaries explicit across the trace ring; do not collapse the ring into a byte stream abstraction unless requested
- When the placeholder stream becomes real data, prefer a ring buffer or double buffer handoff between producer logic and the USB transmit side

## Documentation Guidance

- Describe the repository as PicoTrace, a low-cost RP2040/RP2350 protocol tracer
- Be explicit about which files implement shared USB scaffolding versus SPI/I2C trace-specific behavior
- Keep documentation concise and practical
- Update relevant instruction and skill files while the work is in progress whenever repository behavior, workflows, or expectations change; do not leave those updates as a separate follow-up when the current task materially changes them
- Keep SPI and I2C support described as passive transaction capture on a shared USB host-control model
- Put implementation-level design writeups under `docs/details/`
- Keep board pin allocation in `docs/hardware-connections.md` and concrete Raspberry Pi bench hookup guidance in `docs/raspberry-pi-test-setup.md`
- When documenting the trace queue, align with the current behavior in `firmware/src/trace/` and the host checks in `firmware/tests/usb_app_test.c`

### Doxygen Rules

- Public headers must use Doxygen comments for exported enums, structs, typedefs, macros, and functions
- Source files should use Doxygen comments for file headers, internal enums, internal structs, and non-trivial internal helpers that define ownership, state, or protocol layout
- For structs in both headers and source files, document the struct itself and each member with a brief `/**< ... */` field description when the member carries state, counters, protocol meaning, or ownership meaning
- For enums in both headers and source files, document the enum itself and add brief value descriptions for each enumerator when the values represent state machines, command kinds, flags, or protocol-visible meanings
- Prefer `@brief`, `@param`, `@return`, and `@copydoc` over ad hoc block comments when documenting functions
- Keep Doxygen comments factual and compact; describe intent, ownership, lifetime, and interpretation, not obvious syntax
- When adding a new module, do not leave only the header documented; apply the same Doxygen standard to the matching `.c` file where it defines important state or control flow

## Change Guidance

- Keep changes narrowly scoped to the requested behavior
- When a task adds or changes repeatable workflow knowledge, update the matching `.github/copilot-instructions.md` guidance and any affected `.github/skills/` content in the same change set
- When firmware changes can affect capture, packetization, buffering, USB service, timing, or other trace-path behavior, run both the SPI benchmarking skill and the I2C trace testing skill before closing the task
- After firmware changes, explicitly report whether SPI benchmark results or I2C trace-test results changed, and call out any measured performance regression or improvement
- Use `docs/testlog/rp2040-benchmark-baseline.md` as the canonical RP2040 baseline page for the current best-known benchmark and I2C trace-validation reference points used for later comparison
- Use `docs/testlog/rp2040-benchmark-testlog.md` as the dated RP2040 run log; keep it empty until real firmware-affecting test runs are recorded, and use `docs/testlog/rp2040-benchmark-testlog-template.md` for the entry format before deciding whether the stable baseline page also needs an update
- Do not add unrelated tooling or interfaces
- Prefer PicoTrace naming on the user-facing USB surface unless the task says otherwise
- Keep host naming aligned with the current public surfaces: Python package `picotrace`, CLI `picotrace`, and .NET project `PicoTrace`