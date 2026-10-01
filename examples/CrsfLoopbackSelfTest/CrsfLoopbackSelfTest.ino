/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @brief CrsfLoopbackSelfTest — round-trip every frame type through a real UART.
 *
 * This is the most useful sketch here. Everything else in this library has been
 * checked on a host against a mock Arduino runtime, which proves the logic and
 * says nothing whatever about your board. This needs no receiver, no radio and
 * no second device: one board and one jumper wire.
 *
 * WIRING
 *   Jumper the CRSF UART's TX pin to its own RX pin. Results print on Serial.
 *
 *   On an ESP32 those are GPIO17 and GPIO16 by default here -- not because the
 *   core picks them (it does not; see below) but because they are the pins this
 *   sketch asks for. Override with -DCRSF_LOOPBACK_TX_PIN / _RX_PIN.
 *   Elsewhere it is whatever Serial1 is wired to on your board.
 *
 * WHAT A PASS ESTABLISHES
 *   That this board's UART, at the real line rate, with real interrupt latency,
 *   carries CRSF frames correctly end to end; that the channel path and link
 *   state work on real bytes; and that the telemetry scheduler paces against
 *   this board's micros() rather than a clock a host test moves by hand.
 *
 * WHAT IT DOES NOT
 *   Anything about a peer. Whether a real ELRS receiver accepts a baudrate
 *   proposal, whether a handset renders a parameter tree, and half-duplex
 *   turnaround timing all need a second device.
 *
 * If it passes on your board, that is evidence worth having: the board, the core
 * version and the result are worth adding to COMPLIANCE.md section 6.
 */

#include <CRSFv3.h>

#if defined(ARDUINO_ARCH_STM32)
/*
 * Serial1 is not usable here. Unlike ESP32, RP2040 and SAMD, the STM32 core
 * always forward-declares it (Serial.h, whenever USART1 exists) but only
 * defines it when the board's one "generic Serial" happens to be assigned to
 * USART1 -- on a Nucleo64 board that slot is USART2 (the ST-Link VCP)
 * instead, so Serial1 is a name with no object behind it. Uart is the
 * concrete class Serial1 would have been; PA9/PA10 are USART1, wired to the
 * D1/D0 pins on a Nucleo64 board's Arduino header -- adjust for a different
 * board.
 */
Uart CrsfSerial(PA10, PA9);
#endif

#if defined(ARDUINO_ARCH_ESP32)
/*
 * An ESP32's default UART pins are not the ones printed on a dev board, and
 * they have moved between core versions: in esp32 core 3.x Serial1 is
 * GPIO26/27 and Serial2 is GPIO4/25, while the GPIO16/17 everyone remembers
 * was core 2.x's Serial2. Picking a Serial and hoping is how this fails on
 * somebody else's board.
 *
 * So the pins are named here and the sketch opens the port itself, using the
 * Stream constructor -- which exists for exactly this and leaves begin() to
 * the caller. On a WROOM-32, avoid GPIO6-11: they are wired to the flash chip.
 */
#ifndef CRSF_LOOPBACK_RX_PIN
#define CRSF_LOOPBACK_RX_PIN 16
#endif
#ifndef CRSF_LOOPBACK_TX_PIN
#define CRSF_LOOPBACK_TX_PIN 17
#endif
CRSFv3 crsf((Stream &)Serial1);
#elif defined(ARDUINO_ARCH_STM32)
CRSFv3 crsf(CrsfSerial);
#else
CRSFv3 crsf(Serial1);
#endif

/* The port the jumper is on, for writing raw bytes onto the wire. */
#if defined(ARDUINO_ARCH_STM32)
#define CRSF_LOOPBACK_WRITE(buf, len) CrsfSerial.write((buf), (len))
#else
#define CRSF_LOOPBACK_WRITE(buf, len) Serial1.write((buf), (len))
#endif

static int checks = 0;
static int failures = 0;

static volatile bool gotAny = false;
static uint8_t gotType = 0;
static uint8_t gotPayload[CRSF_MAX_PAYLOAD_SIZE];
static size_t gotLen = 0;

static void onAnyFrame(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    (void)h;
    (void)ctx;
    gotType = frame->type;
    gotLen = frame->payload_len < sizeof(gotPayload) ? frame->payload_len
                                                     : sizeof(gotPayload);
    memcpy(gotPayload, frame->payload, gotLen);
    gotAny = true;
}

static void report(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
    }
    Serial.print(ok ? F("  ok   ") : F("  FAIL "));
    Serial.println(what);
}

static void roundtrip(uint8_t type, const uint8_t *expect, size_t n, const char *what)
{
    const uint32_t deadline = millis() + 100;
    while (!gotAny && millis() < deadline) {
        crsf.loop();
    }

    if (!gotAny) {
        report(false, what);
        Serial.println(F("       nothing came back within 100 ms"));
    } else if (gotType != type) {
        report(false, what);
        Serial.print(F("       came back as type 0x"));
        Serial.println(gotType, HEX);
    } else if (gotLen != n || memcmp(gotPayload, expect, n) != 0) {
        report(false, what);
        Serial.println(F("       payload differs"));
    } else {
        report(true, what);
    }
    gotAny = false;
}

void setup()
{
    Serial.begin(115200);
    while (!Serial) {
    }
    Serial.println(F("\nCRSFv3 loopback self-test"));

#if defined(ARDUINO_ARCH_ESP32)
    Serial.print(F("jumper GPIO"));
    Serial.print(CRSF_LOOPBACK_TX_PIN);
    Serial.print(F(" (TX) to GPIO"));
    Serial.print(CRSF_LOOPBACK_RX_PIN);
    Serial.println(F(" (RX) before running this\n"));
    Serial1.begin(416666, SERIAL_8N1, CRSF_LOOPBACK_RX_PIN, CRSF_LOOPBACK_TX_PIN);
#else
    Serial.println(F("jumper the CRSF UART's TX to its own RX before running this\n"));
#endif

    if (!crsf.begin(416666, CRSF_ROLE_RX)) {
        Serial.println(F("crsf.begin() failed"));
        return;
    }
    crsf.onFrame(0, onAnyFrame, nullptr); /* every type */

    uint8_t pl[CRSF_MAX_PAYLOAD_SIZE];

    crsf_battery_t bat = {};
    bat.voltage = 168;
    bat.current = 425;
    bat.capacity_used = 1350;
    bat.remaining = 72;
    size_t n = crsf_encode_battery(pl, &bat);
    gotAny = false;
    crsf_send_battery(crsf, &bat);
    roundtrip(CRSF_TYPE_BATTERY, pl, n, "0x08 Battery");

    crsf_attitude_t att = {};
    att.pitch = 1234;
    att.roll = -4321;
    att.yaw = 15000;
    n = crsf_encode_attitude(pl, &att);
    gotAny = false;
    crsf_send_attitude(crsf, &att);
    roundtrip(CRSF_TYPE_ATTITUDE, pl, n, "0x1E Attitude");

    crsf_gps_t gps = {};
    gps.latitude = 521234567;
    gps.longitude = 133456789;
    gps.groundspeed = 1234;
    gps.heading = 18000;
    gps.altitude = 1250;
    gps.satellites = 11;
    n = crsf_encode_gps(pl, &gps);
    gotAny = false;
    crsf_send_gps(crsf, &gps);
    roundtrip(CRSF_TYPE_GPS, pl, n, "0x02 GPS");

    /*
     * Channels, and the link state that follows from them.
     *
     * This port is CRSF_ROLE_RX, so crsf_send_channels() refuses it: only the
     * sending station drives the channel stream, and two masters on one wire is
     * what that guard exists to prevent. Check the refusal, then put a real
     * 0x16 on the wire the way a transmitter would -- built by hand and written
     * straight to the UART, so it still crosses the jumper and comes back
     * through the parser.
     */
    const bool linkWasDown = !crsf.linkUp();
    crsf_channels_t ch;
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        ch.channel[i] = (uint16_t)(172 + i * 100);
    }
    report(crsf_send_channels(crsf, &ch) == CRSF_ERR_INVALID_STATE,
           "a receiving station refuses to send 0x16");

    uint8_t chPl[CRSF_CHANNELS_PAYLOAD_SIZE];
    const size_t chN = crsf_encode_channels(chPl, &ch);
    uint8_t chFrame[CRSF_MAX_FRAME_SIZE];
    const size_t chLen = crsf_build_frame(chFrame, CRSF_SYNC_BYTE,
                                          CRSF_TYPE_RC_CHANNELS_PACKED,
                                          CRSF_ADDR_BROADCAST, CRSF_ADDR_TRANSMITTER,
                                          chPl, chN);
    gotAny = false;
    CRSF_LOOPBACK_WRITE(chFrame, chLen);

    const uint32_t chDeadline = millis() + 100;
    while (!gotAny && millis() < chDeadline) {
        crsf.loop();
    }
    report(gotAny && gotType == CRSF_TYPE_RC_CHANNELS_PACKED, "0x16 Channels");

    crsf_channels_t back;
    const bool gotCh = crsf.channels(back);
    report(gotCh && back.channel[0] == ch.channel[0] &&
               back.channel[CRSF_NUM_CHANNELS - 1] == ch.channel[CRSF_NUM_CHANNELS - 1],
           "channel values survive a real UART");
    report(linkWasDown && crsf.linkUp(), "link comes up once channels arrive");

    /*
     * The telemetry scheduler against this board's own clock. Ten intervals of
     * 50 ms, counted over 525 ms: a real micros(), real UART latency, no
     * simulated time anywhere. The bounds are loose because the point is that
     * it paces at all and does not free-run -- a scheduler that ignored the
     * interval would emit hundreds.
     */
    unsigned emitted = 0;
    gotAny = false;
    crsf.telemetryInterval(CRSF_TYPE_BATTERY, 50);
    crsf.publishBattery(bat);

    const uint32_t until = millis() + 525;
    while (millis() < until) {
        crsf.loop();
        if (gotAny) {
            if (gotType == CRSF_TYPE_BATTERY) {
                emitted++;
            }
            gotAny = false;
        }
    }
    crsf.telemetryInterval(CRSF_TYPE_BATTERY, 0);
    report(emitted >= 8 && emitted <= 13, "scheduler paces to a real clock");
    Serial.print(F("       "));
    Serial.print(emitted);
    Serial.println(F(" frames in 525 ms at a 50 ms interval (expected ~10)"));

    crsf_parser_stats_t st;
    crsf.parserStats(st);
    report(st.bad_crc == 0, "no CRC failures on the way back");

    Serial.print(F("\n  "));
    Serial.print(checks);
    Serial.print(F(" checks, "));
    Serial.print(failures);
    Serial.println(F(" failed"));
    Serial.println(failures == 0
                       ? F("  PASS - bytes left a real UART and came back correct")
                       : F("  FAIL"));
}

void loop()
{
    crsf.loop();
}
