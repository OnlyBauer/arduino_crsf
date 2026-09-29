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
 *   Jumper Serial1's TX pin to its RX pin. Results print on Serial.
 *
 * WHAT A PASS ESTABLISHES
 *   That this board's UART, at the real line rate, with real interrupt latency,
 *   carries CRSF frames correctly end to end.
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
 * Unlike ESP32, RP2040 and SAMD, the STM32 core does not provide a global
 * Serial1: a HardwareSerial exists only where the sketch declares one. PA9/
 * PA10 are USART1, wired to the D1/D0 pins on a Nucleo64 board's Arduino
 * header; adjust for a different board.
 */
HardwareSerial Serial1(PA10, PA9);
#endif

CRSFv3 crsf(Serial1);

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
    Serial.println(F("jumper Serial1 TX to Serial1 RX before running this\n"));

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
