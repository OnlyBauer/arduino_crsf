# Compliance — the Arduino port

The protocol audit is not here. It belongs with the code it audits, which is
[c_crsf](https://git.bauer.pub/Bauer/c_crsf/-/blob/main/COMPLIANCE.md): the frame
layer, the CRCs, every payload encoding, the parameter protocol, the tunnels, the
routing decisions and the four documented deviations all live there.

What this file holds is the part that only exists on an Arduino.

Status vocabulary is c_crsf's, unchanged:

| Status | Meaning |
| --- | --- |
| **Full** | Implemented *and* covered by an automated test |
| **Not Verified** | Implemented, but only provable on real hardware — **not** counted as verified |
| **Partial** | Mechanism provided, but the obligation is not fully discharged in code |

---

## P1. What this port is responsible for

| Area | Status | How it is discharged |
| --- | --- | --- |
| Opening a `HardwareSerial` | Full | `begin()` opens it at the requested rate; tested |
| Running on a bare `Stream` | Full | The other constructor leaves the stream alone; tested, including that it does *not* call `begin()` |
| Reading and feeding | Full | Including a frame delivered one byte per `loop()`, which is the realistic case on a slow sketch |
| Refusing a write it cannot complete | Full | `Stream::write()` blocks once the outgoing ring is full, and the protocol layer calls the sink from inside its periodic tick — so blocking there would make the telemetry schedule the sketch's latency budget. Checked with `availableForWrite()` where the API allows it to be asked. |
| Clock widening across the `micros()` wrap | Full | `micros()` wraps every 71.6 minutes; a flight exceeds that often enough to matter. Tested across the boundary. |
| Clock never returns zero | Full | Zero is the protocol layer's "never happened" sentinel, and `micros()` reads 0 for the first microsecond after reset |
| Baudrate negotiation timing | **Partial, by construction** | A `Stream` has no "transmission complete", so `crsf_port_after_tx()` is called once per `loop()` rather than when a frame has physically left. A negotiated rate therefore takes effect one `loop()` late. That is within the negotiation's timing and is the best a `Stream` can offer. |
| Thread safety | **Partial, by decision** | No lock. A sketch is single-threaded and paying for one would be waste. Where that stops being true — a second task on an ESP32, telemetry from an interrupt — the lock hooks must be filled in. Called out in `CRSFv3.cpp`, `CRSFv3.h` and the README rather than left implicit. |
| AVR | **Supported, stripped** | 3249 bytes of RAM against an ATmega328P's 2048, so the full library does not fit. `CRSF_ARDUINO_MINIMAL` in `src/crsf_local_conf.h` brings the protocol state to 542 and a whole `CrsfRxStation` sketch to 1285 of 2048, measured with avr-gcc 7.3.0. Any other configuration is still refused with an `#error` naming the switch. |

## P2. Vendored core

The core is copied **flat** into `src/`, because the Arduino 1.5 library format
compiles `src/` recursively but puts only `src/` itself on the include path — a
subdirectory would force every vendored file to be edited, which is precisely the
drift this mechanism exists to prevent. It is pinned in `c_crsf.pin` and checked
by the `vendor-unmodified` CI job. Nothing here edits it.

---

## 6. Not verified

**The frame layer is verified on hardware. Nothing involving a peer is.**
Bytes produced by this code have left a physical UART and come back correct, on
an ESP32-WROOM-32 with GPIO17 jumpered to GPIO16 (esp32:esp32 3.3.12,
2026-10-01): `examples/CrsfLoopbackSelfTest` passes 9 of 9 checks, in the full
build and again with `CRSF_ARDUINO_MINIMAL`. No radio, receiver or handset has
been involved.

What *is* established, and by what:

| Established | By |
| --- | --- |
| That frames cross a real UART at 416666 baud and arrive correct — framing, CRC, parsing, the channel path, link state, and the telemetry scheduler against a real clock | `examples/CrsfLoopbackSelfTest` on an ESP32-WROOM-32, 9 of 9 checks, full and minimal builds |
| That a stripped build runs, not just compiles | the same, with `CRSF_ARDUINO_MINIMAL` |
| The wrapper's logic — reading, feeding, ticking, clock widening, the write-refusal path, the channel defaults | `tests/test_arduino_port.cpp`, 43 checks against a mock Arduino runtime |
| That the protocol layer is correct | c_crsf's 14 suites, against the identical bytes this repository vendors |
| That it compiles for five architectures, AVR included | the `arduino-cli` matrix |
| That `library.properties`, the `src/` layout and the example naming are valid | `arduino-lint --compliance strict` |

What would establish the rest, in order of value:

| Step | What it settles | What it needs |
| --- | --- | --- |
| ~~Run `examples/CrsfLoopbackSelfTest`~~ | done, 9 of 9 on an ESP32-WROOM-32 — see above and `HARDWARE-TESTS.md` | — |
| Run `examples/CrsfRxStation` against a transmitter | that a real peer's channel stream is decoded and telemetry is accepted | an ELRS or Crossfire TX module |
| ~~Measure with `size-avr`~~ | done: the `size-avr` job reports flash and RAM for both station sketches on `arduino:avr:nano`, and the README's table carries the measured numbers | — |
| Browse a parameter tree from a handset | that EdgeTX / Agent Lite renders it as intended | a handset |

A pass on the first line is enough to move this library off `0.x`. If you run it,
the board, the core version and the result are worth adding here.

## 7. Known limitations

- **`crsf_port_after_tx()` is called per `loop()`, not per frame.** A `Stream`
  cannot say when a frame has physically left, so a negotiated baudrate takes
  effect one `loop()` late. On a sketch that calls `loop()` often — which is
  every sketch that works at all — this is well inside the negotiation window.
- **Half duplex is not offered.** It needs the UART to release the line, which
  the Arduino `Stream` API cannot express. Use the ESP-IDF or STM32 port for a
  single-wire link.
- **The flash figure is an estimate.** 25–40 KB, extrapolated rather than
  measured, and labelled as such everywhere it appears.
