/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @brief CrsfRxStation — the craft end of a Crossfire link.
 *
 * Reads the stick positions a transmitter sends and answers with telemetry,
 * paced by the library rather than by this sketch.
 *
 * Wiring: the receiver's TX to the board's RX, and its RX to the board's TX.
 * CRSF runs at 416666 baud by default, which not every board can produce
 * exactly; a few per cent of error is fine, a long way out shows up as CRC
 * failures rather than as an obvious fault.
 *
 * THE FAILSAFE RULE, WHICH IS NOT OPTIONAL
 *
 * When the link fails, 0x16 simply stops arriving. There is no "link lost"
 * message. A sketch that reads channels without checking linkUp() will happily
 * keep flying on the last stick positions it saw, which is the single most
 * dangerous thing this library can be made to do. Check it. This example does.
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

/* Stand-ins for this sketch's own flight-control and sensor code. Replace
 * each with a real read; they exist here only so the example compiles. */
void applyControls(uint16_t throttleUs, uint16_t aileronUs)
{
    (void)throttleUs;
    (void)aileronUs;
}

void enterFailsafe() {}

int16_t readPackDecivolts()
{
    return 0;
}
int16_t readPackDeciamps()
{
    return 0;
}
uint32_t readMahUsed()
{
    return 0;
}
uint8_t readPercentRemaining()
{
    return 0;
}

int16_t readPitchCentiradians()
{
    return 0;
}
int16_t readRollCentiradians()
{
    return 0;
}
int16_t readYawCentiradians()
{
    return 0;
}

void setup()
{
    Serial.begin(115200); /* the console */
    crsf.begin(416666, CRSF_ROLE_RX);

    /* Publish whenever you have new data; the cadence is set once, here. */
    crsf.telemetryInterval(CRSF_TYPE_BATTERY, 200);
    crsf.telemetryInterval(CRSF_TYPE_ATTITUDE, 100);
}

void loop()
{
    /* Service the port every pass. The telemetry scheduler cannot resolve finer
     * than the interval between these calls, so avoid long delay() calls. */
    crsf.loop();

    static uint32_t last = 0;
    if (millis() - last < 50) {
        return;
    }
    last = millis();

    if (crsf.linkUp()) {
        const uint16_t throttle = crsf.channelUs(2); /* about 988..2012 us */
        const uint16_t aileron = crsf.channelUs(0);
        applyControls(throttle, aileron);
    } else {
        /* No channels, or they are stale. Do the safe thing. */
        enterFailsafe();
    }

    crsf_battery_t battery = {};
    battery.voltage = readPackDecivolts();
    battery.current = readPackDeciamps();
    battery.capacity_used = readMahUsed();
    battery.remaining = readPercentRemaining();
    crsf.publishBattery(battery);

    crsf_attitude_t attitude = {};
    attitude.pitch = readPitchCentiradians();
    attitude.roll = readRollCentiradians();
    attitude.yaw = readYawCentiradians();
    crsf.publishAttitude(attitude);
}
