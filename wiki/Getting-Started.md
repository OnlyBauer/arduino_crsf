# Getting started

> **Read [Hardware status](Hardware-Status) first.** This library has never run
> on a board. It compiles for four architectures and passes its host suites, but
> nothing here has been on a wire, and you should know that before you connect it
> to something that flies.

## Installing

It is not in the Library Manager index. Clone or download it into your
`libraries` folder, with the folder named **`CRSFv3`**:

```sh
cd ~/Documents/Arduino/libraries
git clone https://git.bauer.pub/Bauer/arduino_crsf.git CRSFv3
```

The folder name matters: the IDE matches it against `name=` in
`library.properties`, and a mismatch means the examples menu and the include both
misbehave.

For PlatformIO, add the repository to `lib_deps`:

```ini
lib_deps = https://git.bauer.pub/Bauer/arduino_crsf.git
```

## Your board

`architectures=esp32,stm32,rp2040,samd`.

**AVR will not work and says so at compile time.** A port needs about 3552 bytes
of RAM; an ATmega328P has 2048 in total. There is no set of switches that closes
a gap of that size and leaves something still worth calling a CRSF library. If
you try, `crsf_arduino_conf.h` stops the build with a message explaining why,
rather than letting the IDE fail somewhere inside the linker.

Boards not listed are untested rather than refused. An UNO R4 (`renesas_uno`,
32 KB of RAM) would very likely work; it is not in the list because nothing has
compiled it, and the list means "CI compiles this", not "this ought to be fine".

## A first sketch

```cpp
#include <CRSFv3.h>

CRSFv3 crsf(Serial1);

void setup() {
    Serial.begin(115200);              // the console
    crsf.begin(416666, CRSF_ROLE_RX);  // the link
    crsf.telemetryInterval(CRSF_TYPE_BATTERY, 200);
}

void loop() {
    crsf.loop();                       // call this every pass

    if (crsf.linkUp()) {
        uint16_t throttle = crsf.channelUs(2);   // about 988..2012
        applyControls(throttle);
    } else {
        enterFailsafe();
    }

    crsf_battery_t battery = {};
    battery.voltage   = readDecivolts();
    battery.remaining = readPercent();
    crsf.publishBattery(battery);      // the library decides when it goes out
}
```

Wiring: the receiver's TX to your board's RX, and its RX to your board's TX.
416666 baud is the CRSF default and not every board reaches it exactly; a few per
cent of error is fine, and a long way out shows up as CRC failures rather than as
an obvious fault. `crsf.parserStats()` will tell you.

## The failsafe rule, which you should not skip

When the link fails, `0x16` simply stops arriving. There is no "link lost"
message, and the last stick positions stay exactly where they were.

`crsf.channel()` deliberately returns the **midpoint** (992) when nothing has
arrived — which means a sketch that reads it without checking `linkUp()` will
happily keep flying on a centred stick after the link drops. That is the single
most dangerous thing this library can be made to do quietly, so:

```cpp
if (crsf.linkUp()) { /* act */ } else { /* do the safe thing */ }
```

`linkUp()` is true while a channel frame has arrived within the failsafe hold —
one second by default, which is what crsf.md:519 recommends.

## Call `loop()` often

`crsf.loop()` reads the serial port, feeds the protocol layer, and runs the
periodic work. The telemetry scheduler cannot resolve finer than the interval
between calls, so a sketch that sits in `delay(200)` cannot honour a 100 ms
cadence.

Avoid `delay()` entirely if you can; the example uses a `millis()` comparison.

## The whole API, not just the methods

The fifteen or so methods on `CRSFv3` cover what a short sketch does. For
anything else — the parameter protocol, the MAVLink and MSP tunnels, `0x32`
Direct Commands, routing, and every one of the ~120 `crsf_send_*` and
`crsf_publish_*` functions — use the C API directly. The implicit conversion
makes it read naturally:

```cpp
crsf_publish_gps(crsf, &gps);
crsf_mavlink_send(crsf, CRSF_ADDR_FLIGHT_CONTROLLER, frame, len);
crsf_params_attach_provider(crsf.port(), &provider);
```

That is the same code you would write on an ESP-IDF or STM32 target, which is the
point of the whole arrangement. The reference for all of it is
[c_crsf](https://git.bauer.pub/Bauer/c_crsf), documented once rather than
paraphrased here.

## Threading

There is no lock. A sketch is single-threaded: `loop()` feeds the port and your
code calls the API from that same `loop()`.

If that stops being true — a second FreeRTOS task on an ESP32, or telemetry
published from an interrupt — fill in `ops_.lock` and `ops_.unlock` in
`CRSFv3::begin()`. Leaving them empty in that case is silently wrong rather
than loudly wrong, which is why it is said here and in the source.

## Proving it on your board

Run [`examples/CrsfLoopbackSelfTest`](Examples) with `Serial1`'s TX jumpered to
its RX. It round-trips frames through the real UART and prints a pass/fail table,
and it is the difference between "compiles" and "works".

If it passes, that result is worth recording in [Compliance](Compliance) §6 — it
is what moves this library off `0.x`.

## Where to go next

| | |
| --- | --- |
| [Hardware status](Hardware-Status) | what is and is not established, and what would establish the rest |
| [Examples](Examples) | the sketches under `examples/` |
| [Vendored core](Vendored-Core) | what the `crsf_*` files in `src/` are, and how to refresh them |
| [API reference](API-Reference) | the generated per-function reference |
| [Compliance](Compliance) | what this library is responsible for, and what has actually been observed |
