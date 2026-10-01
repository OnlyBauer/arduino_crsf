/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_arduino_conf.h
 * @brief Build-time configuration, and the architecture check.
 *
 * Edit this file, or define the same symbols in `platformio.ini` under
 * `build_flags`. Everything is `#ifndef`-guarded so a `-D` wins.
 */

#ifndef CRSF_ARDUINO_CONF_H
#define CRSF_ARDUINO_CONF_H

/* For the CRSF_ENABLE_* values the AVR check below reads. */
#include "crsf_conf.h"

/*
 * AVR fits, but only stripped. A full crsf_port_t is 3249 bytes measured and an
 * ATmega328P has 2048; without the tunnels and the parameter protocol it is
 * 1185, and with four scheduler slots and four callbacks 542.
 *
 * So the check is on the configuration, not the architecture: build a Nano or
 * an Uno with CRSF_ARDUINO_MINIMAL (see crsf_local_conf.h) and this is quiet.
 * Leave the big features on and it stops the build here, because
 * library.properties' architectures field is only a hint -- without this the
 * IDE builds anyway and fails inside the linker, unactionably.
 */
#if defined(__AVR__) && !defined(CRSF_ALLOW_AVR)
#if CRSF_ENABLE_MSP || CRSF_ENABLE_MAVLINK || CRSF_ENABLE_PARAMS
#error "This configuration needs more RAM than an AVR has: the MSP tunnel is 1038 bytes, MAVLink 571, the parameter protocol 455, against an ATmega328P's 2048 in total. Define CRSF_ARDUINO_MINIMAL in src/crsf_local_conf.h for a receiver or transmitter station, which is 542 bytes and fits. See README.md, Architecture support. Define CRSF_ALLOW_AVR to build it anyway."
#endif
#endif

/**
 * Bytes read from the stream per pass through loop().
 *
 * A CRSF frame is at most 64 bytes; reading more per pass only costs latency.
 */
#ifndef CRSF_ARDUINO_READ_CHUNK
#define CRSF_ARDUINO_READ_CHUNK 64
#endif

/**
 * Default line rate when begin() is given 0.
 *
 * The CRSF full-duplex default, 416666. A few per cent of clock error is
 * tolerated; further out shows up as CRC failures, not as an obvious fault.
 */
#ifndef CRSF_ARDUINO_DEFAULT_BAUD
#define CRSF_ARDUINO_DEFAULT_BAUD CRSF_BAUD_FULL_DUPLEX_DEFAULT
#endif

#endif /* CRSF_ARDUINO_CONF_H */
