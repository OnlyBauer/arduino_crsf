# Hardware status

**Nothing in this repository has been run on a board.** No byte produced by this
code has left a physical UART. The author has no Arduino hardware.

That is stated first, and on the front page, because a library for something that
flies has to be honest about what has actually been tried.

## What is established

| Established | By | Strength |
| --- | --- | --- |
| The protocol itself is correct | [c_crsf](https://git.bauer.pub/Bauer/c_crsf)'s 13 suites and 2624 checks, against the identical bytes this repository vendors | strong — the same code has been flying on an ESP32 |
| The wrapper's logic: reading, feeding, ticking, a frame delivered one byte per `loop()`, the 32-bit `micros()` widening across its wrap, the refuse-rather-than-block write path, and the channel midpoint default | `tests/test_arduino_port.cpp`, 43 checks against a mock Arduino runtime | good for logic, silent about boards |
| It compiles for four architectures | the `arduino-cli` matrix: esp32, rp2040, stm32 and samd | rules out an architecture-specific compile error |
| The library metadata is valid | `arduino-lint --compliance strict` | catches the things that break silently: `library.properties`, the `src/` layout, and the `examples/<Name>/<Name>.ino` rule |

There is very little wrapper to get wrong — about 150 lines — which is the point
of the arrangement. The hard part is the vendored core, and that has been on
hardware for a while under a different port.

## What is not

Read `tests/arduino_stubs/Arduino.h` for the full version. In summary:

- **There is no board.** `Stream`, `HardwareSerial` and `micros()` are test
  doubles. Nothing here says anything about a real UART, real timing, or
  interrupt latency.
- **Nothing says `Serial1` exists** on the part you have, or that it can produce
  416666 baud closely enough. Several popular boards cannot hit it exactly.
- **The flash figure is an estimate.** 25–40 KB, extrapolated rather than
  measured, because there is no `avr-gcc` here to measure with. It is labelled an
  estimate everywhere it appears and should not be quoted as a fact.

## What would close it

### 1. `examples/CrsfLoopbackSelfTest` — one board, one jumper wire

Jumper `Serial1`'s TX to its RX and run it. It round-trips frames through the
real UART at the real line rate and prints a pass/fail table.

This settles that the UART works, that the board reaches the line rate closely
enough, and that framing survives a real peripheral. **A pass is enough to move
this library off `0.x`.**

### 2. `examples/CrsfRxStation` against a transmitter

Needs an ELRS or Crossfire TX module. Settles that a real peer's channel stream
decodes and that telemetry is accepted going the other way.

### 3. A `size` measurement

Replaces the estimated flash figure in the README with a real one. Needs a
toolchain in CI — `arduino-cli compile` already reports the number, so this is a
matter of capturing it rather than of new work.

### 4. Boards beyond the four

`esp8266` and `renesas_uno` (UNO R4, 32 KB of RAM) would probably work. They are
deliberately not listed in `architectures` yet: that list means "CI compiles
this", and adding a board to it and to the matrix should happen in the same
commit.

## If you run any of it

The result is worth more than anything else that could be added here right now.
What to record in [Compliance](Compliance) §6:

- the board, and the core version from the Boards Manager
- the line rate, and whether `parserStats()` showed CRC failures
- for the loopback, the pass/fail table as printed
- for anything involving a peer, what the peer was

A failure is as useful as a pass, and rather more interesting.
