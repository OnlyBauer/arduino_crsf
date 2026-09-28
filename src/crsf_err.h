/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_err.h
 * @brief The protocol layer's error type, free of ESP-IDF.
 *
 * @par Why the values are what they are
 * This component shipped 1.0.0 returning @c esp_err_t from roughly 120 public
 * functions, and callers compare those returns against @c ESP_OK, pass them to
 * @c ESP_ERROR_CHECK() and print them with @c esp_err_to_name(). The protocol
 * layer is being moved somewhere that cannot include @c esp_err.h, so it needs
 * an error type of its own -- but changing what those functions return would
 * break every one of those callers.
 *
 * So @ref crsf_err_t is @c int, exactly as @c esp_err_t is, and each constant
 * below is numerically identical to the ESP-IDF constant of the same meaning.
 * Assignment in either direction compiles, @c ESP_ERROR_CHECK() still works, and
 * @c esp_err_to_name() still prints the right name.
 *
 * @par Why this is asserted rather than aliased
 * Defining @c CRSF_OK as @c ESP_OK would make the equality an assumption held by
 * a header that, on STM32 or Arduino, is not even present. Instead esp_crsf.h
 * carries a @c _Static_assert per constant. If Espressif ever renumbers, the
 * build fails at the assertion with the name of the constant that moved, which
 * is the only place the answer can usefully appear.
 *
 * @note The numbering is ESP-IDF's, so it has gaps: it is a global error space
 *       in which this component occupies a handful of slots.
 */

#ifndef CRSF_ERR_H
#define CRSF_ERR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of a CRSF call: ::CRSF_OK or one of the @c CRSF_ERR_ values.
 *
 * Deliberately a plain @c int rather than an enum, so it is layout- and
 * assignment-compatible with ESP-IDF's @c esp_err_t and can carry an error from
 * an underlying driver through unchanged.
 */
typedef int crsf_err_t;

/** Success. */
#define CRSF_OK 0

/** Out of memory, or no free slot in a fixed table. */
#define CRSF_ERR_NO_MEM 0x101

/** A required pointer was NULL, or an argument was out of range. */
#define CRSF_ERR_INVALID_ARG 0x102

/** The port is not in a state where this call means anything. */
#define CRSF_ERR_INVALID_STATE 0x103

/** A length was zero, or past what the frame format allows. */
#define CRSF_ERR_INVALID_SIZE 0x104

/** Nothing matched: no such parameter, callback, route or slot. */
#define CRSF_ERR_NOT_FOUND 0x105

/** Understood, but this build does not implement it. */
#define CRSF_ERR_NOT_SUPPORTED 0x106

/** A bounded wait expired; the transmit queue stayed full. */
#define CRSF_ERR_TIMEOUT 0x107

#ifdef __cplusplus
}
#endif

#endif /* CRSF_ERR_H */
