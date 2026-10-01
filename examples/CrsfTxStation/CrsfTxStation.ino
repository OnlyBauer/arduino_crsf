/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @brief CrsfTxStation — the handset end of a Crossfire link.
 *
 * Sends stick positions and prints the telemetry that comes back. Pair it with
 * CrsfRxStation on a second board.
 *
 *   TxStation  --- sticks ------->  RxStation
 *              <-- telemetry ------
 *
 * WIRING
 *   board A TX -> board B RX, board A RX -> board B TX, and a common ground.
 */

#include <CRSFv3.h>

/* Which serial port the other board is on. See README.md for other boards. */
#if defined(ARDUINO_ARCH_AVR)
CRSFv3 crsf(Serial); /* a Nano has only one, so this sketch cannot print */
#elif defined(ARDUINO_ARCH_STM32)
Uart CrsfSerial(PA10, PA9); /* USART1; this board has no usable Serial1 */
CRSFv3 crsf(CrsfSerial);
#else
CRSFv3 crsf(Serial1);
#endif

/* Sticks go out 50 times a second; the link report every five seconds. */
CrsfEvery sticks(20);
CrsfEvery report(5000);

/* --- telemetry arriving from the other board ------------------------------ */
/* Each of these runs when that kind of frame arrives. The same pattern works
 * for every other telemetry type -- see the CRSF_TYPE_* list in the docs. */

void onBattery(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    crsf_battery_t b;
    if (crsf_decode_battery(frame->payload, frame->payload_len, &b)) {
        Serial.print(F("battery "));
        Serial.print(b.voltage / 10.0f, 1); /* tenths of a volt */
        Serial.print(F(" V  "));
        Serial.print(b.remaining);
        Serial.println(F("%"));
    }
}

void onAttitude(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    crsf_attitude_t a;
    if (crsf_decode_attitude(frame->payload, frame->payload_len, &a)) {
        Serial.print(F("attitude  pitch "));
        Serial.print(a.pitch * 0.0057295779f, 1); /* counts -> degrees */
        Serial.print(F("  roll "));
        Serial.println(a.roll * 0.0057295779f, 1);
    }
}

void onGps(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    crsf_gps_t g;
    if (crsf_decode_gps(frame->payload, frame->payload_len, &g)) {
        Serial.print(F("gps "));
        Serial.print(g.latitude / 1e7, 7);
        Serial.print(' ');
        Serial.print(g.longitude / 1e7, 7);
        Serial.print(F("  "));
        Serial.print(g.satellites);
        Serial.println(F(" sats"));
    }
}

void setup()
{
    Serial.begin(115200);
    crsf.begin(416666, CRSF_ROLE_TX);

    crsf.onFrame(CRSF_TYPE_BATTERY, onBattery);
    crsf.onFrame(CRSF_TYPE_ATTITUDE, onAttitude);
    crsf.onFrame(CRSF_TYPE_GPS, onGps);
}

void loop()
{
    /* Must run every pass. Never put a delay() in this loop. */
    crsf.loop();

    if (sticks.due()) {
        /* Start with every stick centred. */
        crsf_channels_t channels;
        for (uint8_t i = 0; i < CRSF_NUM_CHANNELS; i++) {
            channels.channel[i] = CRSF_CHANNEL_CENTER;
        }

        /* Sweep stick 1 back and forth so the far end sees movement.
         * Replace this with a real reading, for example analogRead(). */
        static int16_t percent = 0;
        static int16_t step = 2;
        percent += step;
        if (percent >= 100 || percent <= 0) {
            step = -step;
        }
        const int32_t span = CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN;
        channels.channel[0] = (uint16_t)(CRSF_CHANNEL_MIN + span * percent / 100);

        /* Channel 5 as a two-position switch, written in microseconds. */
        channels.channel[4] = (uint16_t)CRSF_US_TO_TICKS(percent < 50 ? 1000 : 2000);

        crsf.sendChannels(channels);
    }

    if (report.due()) {
        crsf_parser_stats_t stats;
        if (crsf.parserStats(stats)) {
            Serial.print(F("received "));
            Serial.print(stats.frames_ok);
            Serial.print(F(" frames, "));
            Serial.print(stats.bad_crc);
            Serial.println(F(" bad"));
        }
    }
}
