# CRSFv3

A complete implementation of the [TBS CRSFv3 specification](https://github.com/tbs-fpv/tbs-crsf-spec),
as an Arduino library, usable as either end of a Crossfire link.

```cpp
#include <CRSFv3.h>

CRSFv3 crsf(Serial1);

void setup() { crsf.begin(416666, CRSF_ROLE_RX); }

void loop() {
    crsf.loop();
    if (crsf.linkUp()) {
        uint16_t throttle = crsf.channelUs(2);   // about 988..2012
    }
}
```

## Status: 0.3.0, verified on hardware — against itself, not against a radio

`examples/CrsfLoopbackSelfTest` passes **9 of 9 checks on an ESP32-WROOM-32**
with GPIO17 jumpered to GPIO16: battery, attitude and GPS frames round-trip
through a real UART at 416666 baud, channels decode and bring the link up, the
role guard refuses a receiving station the channel stream, the telemetry
scheduler paces to the board's own clock, and nothing fails CRC. It passes
identically with `CRSF_ARDUINO_MINIMAL`, so the stripped build runs and does not
merely compile.

That settles the frame layer on real silicon. It settles **nothing about a
peer**: no ELRS or Crossfire receiver has seen these bytes, no handset has
rendered a parameter tree, and half-duplex turnaround and signal inversion are
untested. [COMPLIANCE.md](COMPLIANCE.md) §6 is the exact list, and
[HARDWARE-TESTS.md](HARDWARE-TESTS.md) is the work list with results.

If you have a receiver, a result is worth sending back.

## What it is

The protocol is not implemented here. It is
[c_crsf](https://git.bauer.pub/Bauer/c_crsf), vendored flat into `src/` and
shared with the ESP-IDF and STM32 ports — so the frame layer, the telemetry
scheduler, the parameter protocol, the tunnels, routing and baudrate negotiation
are the same code that has been flying on an ESP32. Not a second implementation
of the same specification, which is how two implementations start to disagree.

What is in this repository is about 150 lines of C++ that read a `Stream`, feed
it to that core, and give `micros()` to it as a clock.

## The whole API, not just the methods

There are about fifteen convenience methods, for what a short sketch actually
does: `linkUp()`, `channel()`, `channelUs()`, `publishBattery()`,
`telemetryInterval()`, `onFrame()` and so on.

Everything else — the parameter protocol, the MAVLink and MSP tunnels, `0x32`
Direct Commands, routing, and every one of the ~120 `crsf_send_*` and
`crsf_publish_*` functions — is the C API, reached through the implicit
conversion:

```cpp
crsf_publish_gps(crsf, &gps);
crsf_mavlink_send(crsf, CRSF_ADDR_FLIGHT_CONTROLLER, frame, len);
crsf_params_attach_provider(crsf.port(), &provider);
```

That is deliberate. Wrapping all of them would double the documentation,
guarantee that a function added upstream silently does not appear here, and buy
nothing: Arduino builds with `-ffunction-sections -Wl,--gc-sections`, so an
unreferenced C function costs exactly as little as an unreferenced method would.

## Architecture support

`architectures=esp32,stm32,rp2040,samd`.

**AVR works, but only stripped.** A full port is **3249 bytes** of RAM against
an ATmega328P's **2048**, so the whole library does not fit. The compile-time
switches in [`src/crsf_local_conf.h`](src/crsf_local_conf.h) make it fit.
Measured with avr-gcc 7.3.0 at `-Os`, as the size of the protocol state:

| Configuration | RAM | against 2048 |
| --- | --- | --- |
| everything on | 3249 B | **159 %** |
| without the tunnels | 1640 B | 80 % |
| without tunnels *and* parameters | 1185 B | 58 % |
| `CRSF_ARDUINO_MINIMAL` | **542 B** | **26 %** |

`CRSF_ARDUINO_MINIMAL` is one line in `src/crsf_local_conf.h`. It drops the
MAVLink and MSP tunnels, the parameter protocol and the router, and cuts the
telemetry scheduler to four frame types — a receiver or transmitter station,
which is what a Nano is for. The biggest single saving is not a tunnel: the
scheduler holds a whole 60-byte payload per slot, so twelve slots are 900 bytes.

Whole sketches, compiled for `arduino:avr:nano`:

| Example | Flash of 30720 | RAM of 2048 | Free |
| --- | --- | --- | --- |
| `CrsfRxStation` | 8662 B (28 %) | 1285 B (62 %) | 763 B |
| `CrsfTxStation` | 12400 B (40 %) | 1305 B (63 %) | 743 B |

Leave the big features on and `src/crsf_arduino_conf.h` stops the build with an
`#error` naming the switch, rather than letting the IDE fail inside the linker
with a message nobody can act on. `CRSF_ALLOW_AVR` overrides that if you want to
try regardless.

Boards not listed are not refused, only untested. `esp8266` and
`renesas_uno` (UNO R4, 32 KB of RAM) would probably work; they will be added
here in the same commit that adds them to the CI matrix, and not before.

## Threading

There is no lock. A sketch is single-threaded: `loop()` feeds the port and the
sketch calls the API from that same `loop()`.

If that stops being true — a second FreeRTOS task on an ESP32, or telemetry
published from an interrupt — fill in `ops_.lock` and `ops_.unlock` in
`CRSFv3::begin()`. Leaving them empty in that case is silently wrong rather
than loudly wrong, which is why it is said here and in the source rather than
left to be discovered.

## Installing

- **Arduino IDE**: *Sketch -> Include Library -> Manage Libraries*, search for
  **CRSFv3**.
- **arduino-cli**: `arduino-cli lib install CRSFv3`
- **PlatformIO**: `lib_deps = https://github.com/OnlyBauer/arduino_crsf.git`

The source is at
[github.com/OnlyBauer/arduino_crsf](https://github.com/OnlyBauer/arduino_crsf),
which is a read-only mirror; development is on the self-hosted GitLab, so issues
and merge requests belong there.

## Documentation

Generated from this repository by `tools/wiki_build.py` and published with
`tools/wiki-sync.sh --push`:

| | |
| --- | --- |
| [**Getting started**](https://git.bauer.pub/Bauer/arduino_crsf/-/wikis/Getting-Started) | installing, board support, and the failsafe rule you should not skip |
| [**Hardware status**](https://git.bauer.pub/Bauer/arduino_crsf/-/wikis/Hardware-Status) | what is and is not established, and what a board and a jumper wire would settle |
| [**Vendored core**](https://git.bauer.pub/Bauer/arduino_crsf/-/wikis/Vendored-Core) | what the `crsf_*` files in `src/` are, and why they are flat |
| [**API reference**](https://git.bauer.pub/Bauer/arduino_crsf/-/wikis/API-Reference) | the generated per-function reference |

## Testing

```sh
make -C tests            # the wrapper, against a mock Arduino runtime
make -C tests sanitize   # the same under ASan and UBSan
```

Read `tests/arduino_stubs/Arduino.h` before trusting a pass: there is no board in
it, no real UART and no real clock. It tests the wrapper's logic — reading,
feeding, ticking, the 32-bit `micros()` widening and the refuse-rather-than-block
write path — and nothing else.

## Related repositories

Four repositories, one protocol implementation. The core is vendored into
each port rather than depended on, so a port is complete on its own; see any
port's *Vendored core* page for why.

| | | |
| --- | --- | --- |
| [**c_crsf**](https://git.bauer.pub/Bauer/c_crsf) | the protocol, with no platform attached | C11 + libm |
| [**esp_crsf**](https://git.bauer.pub/Bauer/esp_crsf) | ESP-IDF component | verified on ESP32 and ESP32-C3 |
| [**stm_crsf**](https://git.bauer.pub/Bauer/stm_crsf) | STM32Cube HAL, DMA | **not yet hardware-verified** |
| [**arduino_crsf**](https://git.bauer.pub/Bauer/arduino_crsf) | Arduino library, class `CRSFv3` | *you are here* |

## Licence

[Apache-2.0](LICENSE). The vendored core files in `src/` are a verbatim copy of
[c_crsf](https://git.bauer.pub/Bauer/c_crsf) under the same licence; its
`LICENSE` and `NOTICE` are kept in `extras/vendor/`. The CRSF V3 specification is
not redistributed here; it lives in the c_crsf repository, where the code that
cites it is.
