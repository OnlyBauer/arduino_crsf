/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @brief CrsfTxStation — the handset end of a Crossfire link.
 *
 * Drives the RC channel stream and listens for telemetry coming back. Pair
 * this with CrsfRxStation on a second board, or connect a real ELRS / Crossfire
 * transmitter module to Serial1.
 *
 *   TxStation  --- channels (0x16) --->  RxStation
 *              <--- telemetry ---------
 *
 * Wiring for two boards:
 *   TX board's Serial1 TX ---- RX board's Serial1 RX
 *   TX board's Serial1 RX ---- RX board's Serial1 TX
 *   GND ---- GND
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

#if defined(ARDUINO_ARCH_STM32)
CRSFv3 crsf(CrsfSerial);
#else
CRSFv3 crsf(Serial1);
#endif

/* 50 Hz channel rate. crsf.md:132 asks for a rate the baudrate can sustain: at
 * 416666 baud a 64-byte frame takes 1.5 ms, so 20 ms is very comfortable. */
static const uint32_t CHANNEL_PERIOD_MS = 20;

/* --- telemetry coming back from the RX station ---------------------------- */

static void onBattery(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    (void)h;
    (void)ctx;
    crsf_battery_t b;
    if (crsf_decode_battery(frame->payload, frame->payload_len, &b)) {
        /* Units are 0.1 V and 0.1 A -- see the note on crsf_battery_t. */
        Serial.print(F("battery: "));
        Serial.print(b.voltage / 10);
        Serial.print('.');
        Serial.print(b.voltage % 10);
        Serial.print(F(" V  "));
        Serial.print(b.current / 10);
        Serial.print('.');
        Serial.print(b.current % 10);
        Serial.print(F(" A  "));
        Serial.print(b.capacity_used);
        Serial.print(F(" mAh used  "));
        Serial.print(b.remaining);
        Serial.println('%');
    }
}

static void onGps(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    (void)h;
    (void)ctx;
    crsf_gps_t g;
    if (crsf_decode_gps(frame->payload, frame->payload_len, &g)) {
        /* groundspeed is km/h * 100; altitude carries a +1000 m offset. */
        Serial.print(F("gps: "));
        Serial.print(g.latitude / 1e7, 7);
        Serial.print(' ');
        Serial.print(g.longitude / 1e7, 7);
        Serial.print(F("  "));
        Serial.print(g.groundspeed / 100.0f, 2);
        Serial.print(F(" km/h  "));
        Serial.print((int32_t)g.altitude - 1000);
        Serial.print(F(" m  "));
        Serial.print(g.satellites);
        Serial.println(F(" sats"));
    }
}

static void onAttitude(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    (void)h;
    (void)ctx;
    crsf_attitude_t a;
    if (crsf_decode_attitude(frame->payload, frame->payload_len, &a)) {
        /* 100 urad per LSB: degrees = counts * 1e-4 * 180/pi. */
        Serial.print(F("attitude: pitch "));
        Serial.print(a.pitch * 0.0057295779f, 1);
        Serial.print(F("  roll "));
        Serial.print(a.roll * 0.0057295779f, 1);
        Serial.print(F("  yaw "));
        Serial.print(a.yaw * 0.0057295779f, 1);
        Serial.println(F(" deg"));
    }
}

static void onFlightMode(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    (void)h;
    (void)ctx;
    crsf_flight_mode_t fm;
    if (crsf_decode_flight_mode(frame->payload, frame->payload_len, &fm)) {
        Serial.print(F("flight mode: "));
        Serial.println(fm.flight_mode);
    }
}

static void onLinkStats(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    (void)h;
    (void)ctx;
    crsf_link_statistics_t ls;
    if (crsf_decode_link_statistics(frame->payload, frame->payload_len, &ls)) {
        /* RSSI travels as dBm * -1 (crsf.md:486). */
        Serial.print(F("link: uplink LQ "));
        Serial.print(ls.up_link_quality);
        Serial.print(F("%  RSSI -"));
        Serial.print(ls.up_rssi_ant1);
        Serial.print(F(" dBm  SNR "));
        Serial.print(ls.up_snr);
        Serial.println(F(" dB"));
    }
}

/* A device that answers our ping identifies itself with 0x29. */
static void onDeviceInfo(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    (void)h;
    (void)ctx;
    crsf_device_info_t di;
    if (crsf_decode_device_info(frame->payload, frame->payload_len, &di)) {
        Serial.print(F("found device 0x"));
        Serial.print(frame->origin, HEX);
        Serial.print(F(": \""));
        Serial.print(di.device_name);
        Serial.print(F("\"  "));
        Serial.print(di.parameters_total);
        Serial.println(F(" parameters"));
    }
}

void setup()
{
    Serial.begin(115200); /* the console */
    crsf.begin(416666, CRSF_ROLE_TX);

    crsf.onFrame(CRSF_TYPE_BATTERY, onBattery);
    crsf.onFrame(CRSF_TYPE_GPS, onGps);
    crsf.onFrame(CRSF_TYPE_ATTITUDE, onAttitude);
    crsf.onFrame(CRSF_TYPE_FLIGHT_MODE, onFlightMode);
    crsf.onFrame(CRSF_TYPE_LINK_STATISTICS, onLinkStats);
    crsf.onFrame(CRSF_TYPE_DEVICE_INFO, onDeviceInfo);

    /* Ask everything on the bus to identify itself (crsf.md:651). */
    crsf_send_ping(crsf, CRSF_ADDR_BROADCAST);
}

void loop()
{
    crsf.loop();

    static uint32_t lastChannels = 0;
    if (millis() - lastChannels < CHANNEL_PERIOD_MS) {
        return;
    }
    lastChannels = millis();

    static uint32_t tick = 0;

    /* Sweep channel 1 back and forth so movement is visible on the far end.
     * Replace this with real gimbal readings. */
    const int32_t span = CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN;
    const int32_t phase = tick % 100;
    const int32_t pos = (phase < 50) ? phase : (100 - phase); /* 0..50..0 */

    crsf_channels_t channels;
    for (uint8_t i = 0; i < CRSF_NUM_CHANNELS; i++) {
        channels.channel[i] = CRSF_CHANNEL_CENTER;
    }
    channels.channel[0] = (uint16_t)(CRSF_CHANNEL_MIN + span * pos / 50);

    /* A two-position switch on channel 5, written in microseconds. */
    channels.channel[4] = (uint16_t)CRSF_US_TO_TICKS(((tick / 50) % 2) ? 2000 : 1000);

    crsf.sendChannels(channels);

    /* Link health once every five seconds. */
    if ((tick % 250) == 0) {
        crsf_parser_stats_t stats;
        if (crsf.parserStats(stats)) {
            Serial.print(F("rx frames "));
            Serial.print(stats.frames_ok);
            Serial.print(F("  bad crc "));
            Serial.print(stats.bad_crc);
            Serial.print(F("  bad len "));
            Serial.print(stats.bad_length);
            Serial.print(F("  bad sync "));
            Serial.println(stats.bad_sync);
        }
    }

    tick++;
}
