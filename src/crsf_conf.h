/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_conf.h
 * @brief Compile-time configuration: which features are built, and how big they are.
 *
 * A full `crsf_port_t` is 3249 bytes on an ATmega328P, which has 2048 in total.
 * The switches here trade features for bytes, so a receiver or transmitter that
 * only needs channels and telemetry fits where the whole library does not.
 *
 * Every default is `#ifndef`-guarded, so `-D` on the command line wins and this
 * file need not be edited. A toolchain that cannot pass `-D` — the Arduino IDE
 * is the one that matters — can instead put a `crsf_local_conf.h` anywhere on
 * the include path; it is included below, before the defaults, and the port
 * repositories ship one.
 *
 * @warning These macros change the layout of @ref crsf_port, which is a public
 *          struct the application places in its own storage. **Every**
 *          translation unit in a program must therefore see the same values.
 *          Set them for the whole build, never in one `.c` file.
 *
 * @see crsf_port for what the sizing macros actually cost.
 */

#ifndef CRSF_CONF_H
#define CRSF_CONF_H

/*
 * Local overrides first, so the #ifndef defaults below see them. __has_include
 * is C2x, but every compiler this library targets has supported it as an
 * extension for years; the defaults still apply if it is missing.
 */
#if defined(__has_include)
#if __has_include("crsf_local_conf.h")
#include "crsf_local_conf.h"
#endif
#endif

/**
 * @defgroup crsf_conf Configuration
 * @brief Compile-time feature and size switches.
 *
 * The savings below are measured on an ATmega328P with avr-gcc 7.3.0 at `-Os`,
 * as bytes of RAM removed from @ref crsf_port.
 *
 * | Switch | Default | Saves |
 * | --- | --- | --- |
 * | `CRSF_ENABLE_MAVLINK` | 1 | 571 B |
 * | `CRSF_ENABLE_MSP` | 1 | 1038 B |
 * | `CRSF_ENABLE_PARAMS` | 1 | 455 B |
 * | `CRSF_ENABLE_ROUTER` | 1 | 3 B, plus 64 B of stack |
 * | `CRSF_ENABLE_FLOAT_MATH` | 1 | drops the libm dependency |
 * | `CRSF_SCHED_MAX_SLOTS` | 12 | 75 B each |
 * | `CRSF_MAX_CALLBACKS` | 12 | 5 B each |
 *
 * All of them off, with four slots and four callbacks, leaves 542 bytes.
 * @{
 */

/**
 * Build the 0xAA MAVLink tunnel.
 *
 * Costs 571 bytes: the reassembly state and a buffer for one assembled frame.
 * With this off, `crsf_mavlink_send()` and `crsf_mavlink_on_frame()` are not
 * declared, and an inbound 0xAA is delivered to an `crsf_on_frame()` callback
 * as an ordinary frame rather than reassembled.
 */
#ifndef CRSF_ENABLE_MAVLINK
#define CRSF_ENABLE_MAVLINK 1
#endif

/**
 * Build the 0x7A / 0x7B MSP tunnel.
 *
 * Costs 1038 bytes, the most of any single feature, because an MSP body may be
 * @ref CRSF_MSP_BODY_MAX long and one is buffered per port. Lowering that
 * constant is the alternative to switching the tunnel off entirely.
 */
#ifndef CRSF_ENABLE_MSP
#define CRSF_ENABLE_MSP 1
#endif

/**
 * Build the 0x2B / 0x2C / 0x2D parameter protocol, both directions.
 *
 * Costs 455 bytes, nearly all of it the host-side walk state. With this off a
 * device cannot be configured over CRSF and cannot configure anything else; it
 * still answers 0x28 with 0x29, so it remains discoverable.
 */
#ifndef CRSF_ENABLE_PARAMS
#define CRSF_ENABLE_PARAMS 1
#endif

/**
 * Build multi-port routing.
 *
 * Costs only 3 bytes of @ref crsf_port, but also a 64-byte frame buffer on the
 * receive path's stack, which is worth more than it sounds on a part with 2 KB
 * of RAM. Switch it off unless the application really does bridge two links.
 */
#ifndef CRSF_ENABLE_ROUTER
#define CRSF_ENABLE_ROUTER 1
#endif

/**
 * Use floating-point maths for the 0x09 logarithmic vertical speed.
 *
 * The only place the library needs libm. With this off, `crsf_vspeed_pack()`
 * and `crsf_vspeed_unpack()` are not declared and nothing links against `-lm`.
 */
#ifndef CRSF_ENABLE_FLOAT_MATH
#define CRSF_ENABLE_FLOAT_MATH 1
#endif

/**
 * How many telemetry frame types can be scheduled at once.
 *
 * Each slot holds one whole payload, so a slot is 75 bytes and the default of
 * twelve is 900 — the largest single item in @ref crsf_port, and the first
 * thing to reduce on a small part. A receiver publishing battery, attitude,
 * GPS and flight mode needs four.
 */
#ifndef CRSF_SCHED_MAX_SLOTS
#define CRSF_SCHED_MAX_SLOTS 12
#endif

/**
 * How many frame callbacks can be registered at once.
 *
 * Five bytes each, so this is worth changing only once everything else is off.
 */
#ifndef CRSF_MAX_CALLBACKS
#define CRSF_MAX_CALLBACKS 12
#endif

/** @} */

#endif /* CRSF_CONF_H */
