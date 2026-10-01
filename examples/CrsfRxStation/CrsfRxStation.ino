/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @brief CrsfRxStation — the craft end of a Crossfire link.
 *
 * Reads the stick positions a transmitter sends, and sends telemetry back.
 *
 * WIRING
 *   receiver TX -> board RX, receiver RX -> board TX, and a common ground.
 *
 * THE FAILSAFE RULE, WHICH IS NOT OPTIONAL
 *   When the link fails, the stick positions simply stop arriving. There is no
 *   "link lost" message. A sketch that reads channels without checking
 *   linkUp() keeps flying on the last sticks it saw. Always check it.
 */

#include <CRSFv3.h>

/* Which serial port the receiver is on. See README.md for other boards. */
#if defined(ARDUINO_ARCH_AVR)
CRSFv3 crsf(Serial); /* a Nano has only one, so this sketch cannot print */
#elif defined(ARDUINO_ARCH_STM32)
Uart CrsfSerial(PA10, PA9); /* USART1; this board has no usable Serial1 */
CRSFv3 crsf(CrsfSerial);
#else
CRSFv3 crsf(Serial1);
#endif

/* Read the sensors 20 times a second. Reading them every pass would be
 * wasteful, and delay() must never be used -- it would stop crsf.loop(). */
CrsfEvery sensors(50);

void setup()
{
    crsf.begin(416666, CRSF_ROLE_RX);

    /* How often each kind of telemetry goes out. The library does the pacing. */
    crsf.telemetryInterval(CRSF_TYPE_BATTERY, 200);
    crsf.telemetryInterval(CRSF_TYPE_ATTITUDE, 100);

    /* How long the sticks may be missing before linkUp() turns false. */
    crsf.linkUpTimeoutMs(1000);
}

void loop()
{
    /* Must run every pass. Never put a delay() in this loop. */
    crsf.loop();

    if (crsf.linkUp()) {
        const uint16_t throttle = crsf.channelUs(2); /* about 988..2012 */
        const uint16_t aileron = crsf.channelUs(0);

        /* Replace with your own code that drives the motors. */
        (void)throttle;
        (void)aileron;
    } else {
        /* The link is gone. Replace with your own safe behaviour. */
    }

    if (sensors.due()) {
        /* Replace each number with a real reading. */
        crsf_battery_t battery = {};
        battery.voltage = 168;       /* 16.8 V, in tenths */
        battery.current = 42;        /* 4.2 A, in tenths */
        battery.capacity_used = 350; /* mAh */
        battery.remaining = 72;      /* per cent */
        crsf.publishBattery(battery);

        crsf_attitude_t attitude = {};
        attitude.pitch = 0; /* hundredths of a radian */
        attitude.roll = 0;
        attitude.yaw = 0;
        crsf.publishAttitude(attitude);
    }
}
