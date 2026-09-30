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

/*
 * AVR is not supported, and the numbers are not close: a crsf_port_t is ~3552
 * bytes measured, an ATmega328P has 2048 in total. Stripping the tunnels and
 * the parameter protocol reaches ~1499, which is 73 % of the part's RAM and
 * leaves the popular subset this project exists not to be.
 *
 * library.properties' architectures field is only a hint -- without this the
 * IDE builds anyway and fails inside the linker, unactionably.
 */
#if defined(__AVR__) && !defined(CRSF_ALLOW_AVR)
#error "CRSFv3 needs about 3.5 KB of RAM; an ATmega328P has 2 KB. AVR boards are not supported -- see README.md, Architecture support. Use an ESP32, STM32, RP2040 or SAMD board. Define CRSF_ALLOW_AVR to attempt it anyway."
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
