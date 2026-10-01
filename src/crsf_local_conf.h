/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_local_conf.h
 * @brief Where you turn CRSF features off to save RAM. Edit this file.
 *
 * The protocol core reads this before applying its own defaults, so settings
 * here reach every file in the library. That is the point of it: the Arduino
 * IDE has no way to pass `-D`, and these switches change the layout of a struct
 * the whole library shares, so setting them in a sketch would not work.
 *
 * PlatformIO users can use `build_flags` instead; everything here is
 * `#ifndef`-guarded, so `-D` wins over this file.
 *
 * @see crsf_conf.h for what every switch does and costs.
 *
 * ### The short version
 *
 * Uncomment `CRSF_ARDUINO_MINIMAL` below if you are building a receiver or a
 * transmitter station and do not need the parameter protocol or the tunnels.
 * It is the difference between 3249 bytes of RAM and 542, and it is what makes
 * an ATmega328P — an Arduino Nano or Uno — possible at all.
 *
 * Measured with avr-gcc 7.3.0 at `-Os` for an ATmega328P, which has 2048 bytes:
 *
 * | Configuration | RAM | of 2048 |
 * | --- | --- | --- |
 * | everything on | 3249 B | 159 % |
 * | no tunnels | 1640 B | 80 % |
 * | no tunnels or parameters | 1185 B | 58 % |
 * | `CRSF_ARDUINO_MINIMAL` | **542 B** | **26 %** |
 */

#ifndef CRSF_LOCAL_CONF_H
#define CRSF_LOCAL_CONF_H

/**
 * The receiver / transmitter station profile: channels, telemetry, link state
 * and device discovery, and nothing else.
 *
 * Switches off the MAVLink and MSP tunnels, the parameter protocol and the
 * router, and cuts the telemetry scheduler to four frame types and the frame
 * callbacks to four. Leaves 542 bytes, which fits an ATmega328P.
 *
 * `examples/CrsfRxStation` and `examples/CrsfTxStation` are written against
 * exactly this subset and need no change. `CrsfParameterHost`,
 * `CrsfParameterDevice` and `CrsfCommandsAndTunnel` will not compile with it:
 * a disabled feature's functions are not declared, so you get an error naming
 * the switch rather than a puzzling link failure.
 */
/* #define CRSF_ARDUINO_MINIMAL 1 */

#ifdef CRSF_ARDUINO_MINIMAL
#ifndef CRSF_ENABLE_MAVLINK
#define CRSF_ENABLE_MAVLINK 0
#endif
#ifndef CRSF_ENABLE_MSP
#define CRSF_ENABLE_MSP 0
#endif
#ifndef CRSF_ENABLE_PARAMS
#define CRSF_ENABLE_PARAMS 0
#endif
#ifndef CRSF_ENABLE_ROUTER
#define CRSF_ENABLE_ROUTER 0
#endif
#ifndef CRSF_SCHED_MAX_SLOTS
#define CRSF_SCHED_MAX_SLOTS 4
#endif
#ifndef CRSF_MAX_CALLBACKS
#define CRSF_MAX_CALLBACKS 4
#endif
#endif /* CRSF_ARDUINO_MINIMAL */

/*
 * Or set them one at a time. Uncomment what you do not need; each line says
 * what it saves. See crsf_conf.h for the detail.
 */

/* #define CRSF_ENABLE_MSP        0 */ /* 1038 B — the MSP tunnel */
/* #define CRSF_ENABLE_MAVLINK    0 */ /*  571 B — the MAVLink tunnel */
/* #define CRSF_ENABLE_PARAMS     0 */ /*  455 B — the parameter protocol */
/* #define CRSF_ENABLE_ROUTER     0 */ /*    3 B, and 64 B of stack */
/* #define CRSF_ENABLE_FLOAT_MATH 0 */ /* drops libm, and the 0x09 frame */

/* #define CRSF_SCHED_MAX_SLOTS   4 */ /* 75 B per slot below 12 */
/* #define CRSF_MAX_CALLBACKS     4 */ /*  5 B per entry below 12 */

#endif /* CRSF_LOCAL_CONF_H */
