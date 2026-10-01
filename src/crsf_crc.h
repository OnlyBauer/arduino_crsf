/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_crc.h
 * @brief The two CRC8 variants CRSF uses.
 *
 * Both tables are compile-time constants taken verbatim from the specification,
 * which removes the runtime table generation (and the global namespace leak) the
 * previous implementation had.
 *
 * @see crsf_crc.c for the tables themselves.
 */

#ifndef CRSF_CRC_H
#define CRSF_CRC_H

#include "crsf_conf.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup crsf_crc CRC8
 * @brief The frame CRC and the nested command CRC.
 *
 * CRSF uses two different CRC8 polynomials. Ordinary frames carry one CRC over
 * type and payload; 0x32 Direct Command frames carry a second, inner CRC in
 * addition, and the outer CRC then covers the inner one. Mixing the two up is
 * the single most common interoperability bug in CRSF implementations, so the
 * two functions are deliberately named apart rather than parameterised.
 * @{
 */

/**
 * @brief Frame CRC — polynomial 0xD5 (crsf.md:187).
 *
 * Covers Type + Payload; excludes the sync byte and the length byte
 * (crsf.md:185). For extended frames, destination and origin are part of the
 * covered region because they sit between Type and the payload.
 *
 * Init 0x00, no input/output reflection, no final XOR.
 *
 * @param data Start of the region to cover, i.e. the type byte.
 * @param len  Number of bytes to cover.
 * @return The CRC8 over @p data, or 0 when @p len is 0.
 *
 * @note @p data may be NULL only when @p len is 0.
 */
uint8_t crsf_crc8(const uint8_t *data, size_t len);

/**
 * @brief Command CRC — polynomial 0xBA (crsf.md:938).
 *
 * Used only by 0x32 Direct Commands, in addition to the normal frame CRC.
 * Covers Type + Destination + Origin + Command ID + Payload (crsf.md:936).
 *
 * Init 0x00, no input/output reflection, no final XOR.
 *
 * @param data Start of the region to cover, i.e. the type byte.
 * @param len  Number of bytes to cover.
 * @return The CRC8 over @p data, or 0 when @p len is 0.
 *
 * @note @p data may be NULL only when @p len is 0.
 * @see crsf_build_command() for the order the two CRCs must be applied in.
 */
uint8_t crsf_crc8_cmd(const uint8_t *data, size_t len);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CRSF_CRC_H */
