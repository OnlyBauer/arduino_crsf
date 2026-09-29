# CrsfPort — CRSFv3 for Arduino

A complete implementation of the [TBS CRSFv3 specification](https://github.com/tbs-fpv/tbs-crsf-spec),
as an Arduino library, usable as either end of a Crossfire link.

```cpp
#include <CrsfPort.h>

CrsfPort crsf(Serial1);

void setup() { crsf.begin(416666, CRSF_ROLE_RX); }

void loop() {
    crsf.loop();
    if (crsf.linkUp()) {
        uint16_t throttle = crsf.channelUs(2);   // about 988..2012
    }
}
```

## Status: 0.1.0, not hardware-verified

This library compiles for ESP32, STM32, RP2040 and SAMD, and passes its host
suites against a mock Arduino runtime. It has **not** been run on a board, or
against a radio, a receiver, or any physical CRSF peer. The author has no
Arduino hardware.

[COMPLIANCE.md](COMPLIANCE.md) §6 says exactly what is and is not established.
`examples/CrsfLoopbackSelfTest` closes most of the gap with one board and one
jumper wire, and a result from it is worth sending back.

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

**AVR is not supported, and the numbers are not close.** A port needs about
**3552 bytes** of RAM, measured. An ATmega328P has **2048** in total — before
`HardwareSerial`'s two 64-byte rings, before the stack, before your sketch.

| Configuration | RAM | against 2048 |
| --- | --- | --- |
| Full | 3552 B | **173 %** |
| without the tunnels | 1947 B | 95 % |
| without tunnels *and* parameters | 1499 B | 73 % |

The last row leaves about 550 bytes for everything else, and what it leaves is a
library that can no longer do the parameter protocol or the tunnels — that is,
the popular subset this project exists not to be. So `src/crsf_arduino_conf.h`
stops the build with an `#error` rather than letting the IDE fail somewhere
inside the linker with a message nobody can act on.

Flash is *estimated* at 25–40 KB against an ATmega328P's 32256 usable, and is
labelled an estimate because no one has measured it: there is no `avr-gcc` here.
When the `size-avr` CI job has run, this table gets real numbers.

Boards not listed are not refused, only untested. `esp8266` and
`renesas_uno` (UNO R4, 32 KB of RAM) would probably work; they will be added
here in the same commit that adds them to the CI matrix, and not before.

## Threading

There is no lock. A sketch is single-threaded: `loop()` feeds the port and the
sketch calls the API from that same `loop()`.

If that stops being true — a second FreeRTOS task on an ESP32, or telemetry
published from an interrupt — fill in `ops_.lock` and `ops_.unlock` in
`CrsfPort::begin()`. Leaving them empty in that case is silently wrong rather
than loudly wrong, which is why it is said here and in the source rather than
left to be discovered.

## Installing

Not in the Library Manager index. Clone or download it into your `libraries`
folder, with the folder named **`CrsfPort`**:

```sh
cd ~/Documents/Arduino/libraries
git clone https://git.bauer.pub/Bauer/arduino_crsf.git CrsfPort
```

For PlatformIO, add the repository to `lib_deps`.

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
| [**arduino_crsf**](https://git.bauer.pub/Bauer/arduino_crsf) | Arduino library, class `CrsfPort` | *you are here* |
## Licence

[Apache-2.0](LICENSE). The vendored core files in `src/` are a verbatim copy of
[c_crsf](https://git.bauer.pub/Bauer/c_crsf) under the same licence; its
`LICENSE` and `NOTICE` are kept in `extras/vendor/`. The CRSF V3 specification is
not redistributed here; it lives in the c_crsf repository, where the code that
cites it is.
