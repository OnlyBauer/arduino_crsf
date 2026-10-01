# Hardware tests

Everything else in this library is checked on a host against a mock Arduino
runtime. That proves the logic and says nothing about a board. This file is the
list of what hardware can establish, what each item needs, and what has actually
been run.

`COMPLIANCE.md` §6 is the summary; this is the working list.

## The rig

| | |
| --- | --- |
| Board | ESP32-WROOM-32 DevKit |
| Wiring | **GPIO16 bridged to GPIO17** — UART2 RX to its own TX, a loopback |
| Console | USB CDC on `Serial` (UART0), 115200 |
| Toolchain | `arduino-cli`, core `esp32:esp32` |

One board and one jumper wire. No receiver, no radio, no second device.

> **ESP32 pin note.** `Serial1`'s default pins on an ESP32 are GPIO9/GPIO10,
> which are wired to the flash chip on a WROOM-32 module and cannot be used.
> `Serial2` defaults to GPIO16/GPIO17, which is why the bridge goes there and
> why the examples select `Serial2` on `ARDUINO_ARCH_ESP32`.

## What a loopback can and cannot establish

A loopback puts the library's transmit path and its receive path on the same
real UART, at the real line rate, with real interrupt latency. That covers the
whole frame layer end to end. It cannot cover anything that needs a *peer* with
its own clock and its own opinion.

| Established | Not established |
| --- | --- |
| Framing, CRC and parsing over real wire bytes | That an ELRS receiver accepts our frames |
| The real line rate (416666 baud) on real hardware | Half-duplex turnaround timing |
| `micros()` as a clock, and the telemetry cadence | That a handset renders a parameter tree |
| That a stripped build runs, not just compiles | Baudrate negotiation with a real peer |
| Flash and RAM on a real target | Signal inversion |

## Work items

Status: **done** = run on the rig above, with the result recorded;
**blocked** = needs hardware not present.

### arduino_crsf

| # | Test | Needs | Status |
| --- | --- | --- | --- |
| HW-1 | `CrsfLoopbackSelfTest`, full build — every frame type round-trips through a real UART | the rig | **done** |
| HW-2 | `CrsfLoopbackSelfTest`, `CRSF_ARDUINO_MINIMAL` — the stripped build runs, not just compiles | the rig | **done** |
| HW-3 | Flash and RAM on `esp32:esp32:esp32`, both profiles, measured not estimated | toolchain only | **done** |
| HW-4 | All six examples compile for `esp32:esp32:esp32` | toolchain only | **done** |
| HW-5 | Link state on hardware: down at boot, up once channels arrive | the rig | **done** (folded into HW-1) |
| HW-6 | Telemetry scheduler cadence observed against a real clock, not a simulated one | the rig | **done** (folded into HW-1) |
| HW-7 | Two-port routing between UART1 and UART2 on one ESP32 | a second jumper pair | blocked |
| HW-8 | `CrsfRxStation` on an ATmega328P, where the RAM budget is actually tight | an Arduino Nano | blocked |
| HW-9 | Interop with a real ELRS or TBS receiver | a receiver | blocked |
| HW-10 | Half-duplex, single-wire, inverted signalling | a receiver and a scope | blocked |

### esp_crsf

| # | Test | Needs | Status |
| --- | --- | --- | --- |
| HW-11 | Every frame type, both tunnels and a parameter walk, through the ESP-IDF port | ESP-IDF installed | **done** |
| HW-12 | `esp_crsf` loopback on UART2 | ESP-IDF installed | **done** |

### c_crsf

The core has no hardware of its own: it is platform-neutral C, verified on a
host and through whichever port is carrying it. A pass on HW-1 is a pass for the
core's frame layer too, since that is the code being exercised.

### stm_crsf

| # | Test | Needs | Status |
| --- | --- | --- | --- |
| HW-13 | Loopback on a Nucleo64, USART1 PA9 to PA10 | an STM32 board | blocked |
| HW-14 | The DMA transmit path, which has no host equivalent | an STM32 board | blocked |

## Results

### HW-1 / HW-2 / HW-5 / HW-6 — `CrsfLoopbackSelfTest`

Rig: ESP32-WROOM-32 DevKit, GPIO16 bridged to GPIO17, arduino-cli with esp32:esp32 3.3.12, 2026-10-01.

Both the full build and `CRSF_ARDUINO_MINIMAL` produce identical output:

```
CRSFv3 loopback self-test
jumper GPIO17 (TX) to GPIO16 (RX) before running this

  ok   0x08 Battery
  ok   0x1E Attitude
  ok   0x02 GPS
  ok   a receiving station refuses to send 0x16
  ok   0x16 Channels
  ok   channel values survive a real UART
  ok   link comes up once channels arrive
  ok   scheduler paces to a real clock
       11 frames in 525 ms at a 50 ms interval (expected ~10)
  ok   no CRC failures on the way back

  9 checks, 0 failed
  PASS - bytes left a real UART and came back correct
```

Two things worth recording from the run itself.

The channel checks **failed on the first attempt**, and the library was right:
`crsf_send_channels()` returns `CRSF_ERR_INVALID_STATE` on a `CRSF_ROLE_RX`
port, because letting a receiving station emit 0x16 would put two masters on
one wire. The test had asked a receiver to transmit. It now asserts the refusal
and injects a 0x16 the way a transmitter would, so the role guard is itself
covered — on hardware.

The scheduler check reports the count it measured, not just a verdict, so a
drift in clock handling shows up as a number rather than as a pass.

### HW-3 — flash and RAM on `esp32:esp32:esp32`

| Build | Flash of 1310720 | RAM of 327680 |
| --- | --- | --- |
| `CrsfLoopbackSelfTest`, full | 282108 B (21 %) | 25732 B (7 %) |
| `CrsfLoopbackSelfTest`, `CRSF_ARDUINO_MINIMAL` | 277420 B (21 %) | 22876 B (6 %) |

The minimal profile saves 4688 bytes of flash and 2856 of RAM on an ESP32. The
RAM figure is larger than the 2707 the same switches save on an AVR, because a
32-bit target pays more for the pointers inside the structures that go away.

### HW-4 — all six examples compile for `esp32:esp32:esp32`

`CrsfCommandsAndTunnel`, `CrsfLoopbackSelfTest`, `CrsfParameterDevice`,
`CrsfParameterHost`, `CrsfRxStation`, `CrsfTxStation` — all OK.

### HW-11 / HW-12 — `esp_crsf/examples/loopback_selftest`

The ESP-IDF port on the same rig and the same jumper, ESP-IDF v6.0, ESP32-WROOM-32
chip rev v3.1. This covers far more of the protocol than the Arduino sketch, and
it matters here because it is **the same vendored c_crsf** that `arduino_crsf`
carries — the frame layer, the tunnels and the parameter protocol are the same
bytes either way.

**35 passed, 0 failed.** Parser: 204 frames, 0 bad CRC, 0 bad length, 0 resyncs.

- 26 telemetry frame types, 0x02 through 0xAC
- `0x16` channels (16 intact) and `0x17` subset channels (8 at 11 bits)
- `0x28`/`0x29` discovery — ping answered with device info
- `0x32` Direct Command — the nested CRC validated by the parser
- `0x3A.0x10` timing correction
- `0xAA` MAVLink tunnel — 281 bytes over 5 chunks
- `0x7A` MSP tunnel — 205 bytes over 4 chunks
- `0x2B`/`0x2C` parameter walk — 4 of 4 — and a `0x2D` write applied

Getting there needed two build fixes, neither of them in the protocol: c_crsf's
standalone `CMakeLists.txt` read `VERSION` by a relative path, which ESP-IDF
resolves against the build directory; and the esp_crsf examples pointed
`EXTRA_COMPONENT_DIRS` at the parent of the whole repository, so every sibling
checkout was scanned as a component.
