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
 * AVR is not supported, and the numbers are not close.
 *
 * A crsf_port_t is about 3552 bytes, measured. An ATmega328P has 2048 bytes of
 * RAM in total -- before HardwareSerial's two 64-byte rings, before the stack,
 * before the sketch. Turning off the tunnels and the parameter protocol brings
 * it to roughly 1499, which is 73 % of all the RAM on the part, and produces
 * something that can no longer do the parameter protocol or the tunnels: that
 * is, the popular subset this project exists not to be.
 *
 * The architectures field in library.properties is only a hint -- the IDE will
 * still attempt the build and fail somewhere inside the linker, with a message
 * nobody can act on. This says it plainly instead.
 *
 * If you want to try anyway, the switches exist and this check can be defeated.
 * It will not fit.
 */
#if defined(__AVR__) && !defined(CRSF_ALLOW_AVR)
#error "CrsfPort needs about 3.5 KB of RAM; an ATmega328P has 2 KB. AVR boards are not supported -- see README.md, Architecture support. Use an ESP32, STM32, RP2040 or SAMD board. Define CRSF_ALLOW_AVR to attempt it anyway."
#endif

/**
 * Bytes read from the stream per pass through loop().
 *
 * A CRSF frame is at most 64 bytes. Reading more per pass costs latency in the
 * sketch and gains nothing.
 */
#ifndef CRSF_ARDUINO_READ_CHUNK
#define CRSF_ARDUINO_READ_CHUNK 64
#endif

/**
 * Default line rate when begin() is given 0.
 *
 * 416666 is the CRSF full-duplex default. Not every board can produce it
 * exactly; a few per cent of error is tolerated, but a long way out shows up as
 * CRC failures rather than as an obvious fault.
 */
#ifndef CRSF_ARDUINO_DEFAULT_BAUD
#define CRSF_ARDUINO_DEFAULT_BAUD CRSF_BAUD_FULL_DUPLEX_DEFAULT
#endif

#endif /* CRSF_ARDUINO_CONF_H */
