/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_bits.h
 * @brief Bit-stream reader and writer for CRSF's packed fields.
 *
 * CRSF packs sub-byte fields in two different orders, and the specification is
 * not consistent about which applies where:
 *
 *  - **LSB-first** for the RC channel frames (0x16, 0x17, 0x18). The spec shows
 *    these as C bitfields (crsf.md:527) without stating a bit order, while also
 *    declaring frames big-endian (crsf.md:173). Every shipping implementation
 *    packs them LSB-first across a little-endian bit stream, so that is the
 *    interoperable choice.
 *  - **MSB-first** for the packed command payloads (0x32.0x09 LED HSV,
 *    crsf.md:1026). No ecosystem consensus exists here, so we follow the
 *    protocol's declared big-endian nature. See the note in crsf_codec.h.
 *
 * Both orders are provided so the choice is explicit at every call site rather
 * than hidden in a compiler's bitfield allocation.
 *
 * Bounds are checked: reading or writing past the buffer sets an overflow flag
 * instead of touching memory. Callers check it once at the end rather than per
 * field, which keeps the encoders readable.
 *
 * Everything here is a header-only static inline; there is no crsf_bits.c.
 */

#ifndef CRSF_BITS_H
#define CRSF_BITS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup crsf_bits Bit-stream helpers
 * @brief Bounds-checked bit packing in both bit orders.
 * @{
 */

/**
 * @brief Bit-stream writer state.
 *
 * Initialise with crsf_bw_init(); do not populate the fields by hand. Check
 * @ref crsf_bitwriter_t::overflow once after the last write.
 */
typedef struct {
    uint8_t *buf;    /**< destination buffer, zeroed by crsf_bw_init() */
    size_t capacity; /**< bytes available in @ref buf */
    size_t bit_pos;  /**< bits written so far */
    bool overflow;   /**< set once a write did not fit; sticky */
} crsf_bitwriter_t;

/**
 * @brief Bit-stream reader state.
 *
 * Initialise with crsf_br_init(); do not populate the fields by hand. Check
 * @ref crsf_bitreader_t::overflow once after the last read.
 */
typedef struct {
    const uint8_t *buf; /**< source buffer, not modified */
    size_t size;        /**< bytes available in @ref buf */
    size_t bit_pos;     /**< bits consumed so far */
    bool overflow;      /**< set once a read ran past the end; sticky */
} crsf_bitreader_t;

/**
 * @brief Start writing into @p buf, which is zeroed first.
 *
 * Zeroing matters: both writers OR bits in, so they rely on the destination
 * starting clear.
 *
 * @param w        Writer to initialise.
 * @param buf      Destination buffer of at least @p capacity bytes.
 * @param capacity Size of @p buf in bytes.
 */
static inline void crsf_bw_init(crsf_bitwriter_t *w, uint8_t *buf, size_t capacity)
{
    w->buf = buf;
    w->capacity = capacity;
    w->bit_pos = 0;
    w->overflow = false;
    memset(buf, 0, capacity);
}

/**
 * @brief Start reading from @p buf.
 *
 * @param r    Reader to initialise.
 * @param buf  Source buffer of at least @p size bytes; borrowed, not copied.
 * @param size Size of @p buf in bytes.
 */
static inline void crsf_br_init(crsf_bitreader_t *r, const uint8_t *buf, size_t size)
{
    r->buf = buf;
    r->size = size;
    r->bit_pos = 0;
    r->overflow = false;
}

/**
 * @brief Bytes needed to hold everything written so far, rounded up.
 *
 * @param w Writer to query.
 * @return Byte count, so a trailing partial byte counts as one whole byte.
 */
static inline size_t crsf_bw_bytes(const crsf_bitwriter_t *w)
{
    return (w->bit_pos + 7u) / 8u;
}

/**
 * @brief Bits still unread.
 *
 * @param r Reader to query.
 * @return Remaining bit count, 0 once the buffer is exhausted.
 */
static inline size_t crsf_br_remaining(const crsf_bitreader_t *r)
{
    const size_t total = r->size * 8u;
    return (r->bit_pos >= total) ? 0u : (total - r->bit_pos);
}

/**
 * @brief Write the @p bits low-order bits of @p value, least-significant first.
 *
 * Used by the RC channel frames. Bit n of the stream lands in byte n/8 at bit
 * position n%8, so an 11-bit value starting at bit 0 fills byte 0 and the low
 * three bits of byte 1.
 *
 * @param w     Writer.
 * @param value Source value; bits above @p bits are ignored.
 * @param bits  Number of bits to write, 1..32. Other values are a no-op.
 *
 * @note On overflow nothing is written and
 *       @ref crsf_bitwriter_t::overflow is set.
 */
static inline void crsf_bw_put_lsb(crsf_bitwriter_t *w, uint32_t value, unsigned bits)
{
    if (bits == 0 || bits > 32) {
        return;
    }
    if (w->bit_pos + bits > w->capacity * 8u) {
        w->overflow = true;
        return;
    }
    for (unsigned i = 0; i < bits; i++) {
        if (value & (1u << i)) {
            const size_t p = w->bit_pos + i;
            w->buf[p >> 3] |= (uint8_t)(1u << (p & 7u));
        }
    }
    w->bit_pos += bits;
}

/**
 * @brief Read @p bits as a least-significant-bit-first value.
 *
 * @param r    Reader.
 * @param bits Number of bits to read, 1..32. Other values return 0.
 * @return The value, zero-extended to 32 bits; 0 on overflow.
 *
 * @note On overflow the read position does not advance and
 *       @ref crsf_bitreader_t::overflow is set. Because 0 is also a legal
 *       value, test the flag rather than the result.
 */
static inline uint32_t crsf_br_get_lsb(crsf_bitreader_t *r, unsigned bits)
{
    if (bits == 0 || bits > 32) {
        return 0;
    }
    if (r->bit_pos + bits > r->size * 8u) {
        r->overflow = true;
        return 0;
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < bits; i++) {
        const size_t p = r->bit_pos + i;
        if (r->buf[p >> 3] & (uint8_t)(1u << (p & 7u))) {
            value |= (1u << i);
        }
    }
    r->bit_pos += bits;
    return value;
}

/**
 * @brief Write @p bits of @p value, most-significant bit first.
 *
 * Used by the packed command payloads, where the first field occupies the top
 * bits of the first byte.
 *
 * @param w     Writer.
 * @param value Source value; bits above @p bits are ignored.
 * @param bits  Number of bits to write, 1..32. Other values are a no-op.
 *
 * @note On overflow nothing is written and
 *       @ref crsf_bitwriter_t::overflow is set.
 */
static inline void crsf_bw_put_msb(crsf_bitwriter_t *w, uint32_t value, unsigned bits)
{
    if (bits == 0 || bits > 32) {
        return;
    }
    if (w->bit_pos + bits > w->capacity * 8u) {
        w->overflow = true;
        return;
    }
    for (unsigned i = 0; i < bits; i++) {
        /* Emit the highest requested bit first. */
        if (value & (1u << (bits - 1u - i))) {
            const size_t p = w->bit_pos + i;
            w->buf[p >> 3] |= (uint8_t)(1u << (7u - (p & 7u)));
        }
    }
    w->bit_pos += bits;
}

/**
 * @brief Read @p bits as a most-significant-bit-first value.
 *
 * @param r    Reader.
 * @param bits Number of bits to read, 1..32. Other values return 0.
 * @return The value, zero-extended to 32 bits; 0 on overflow.
 *
 * @note On overflow the read position does not advance and
 *       @ref crsf_bitreader_t::overflow is set. Because 0 is also a legal
 *       value, test the flag rather than the result.
 */
static inline uint32_t crsf_br_get_msb(crsf_bitreader_t *r, unsigned bits)
{
    if (bits == 0 || bits > 32) {
        return 0;
    }
    if (r->bit_pos + bits > r->size * 8u) {
        r->overflow = true;
        return 0;
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < bits; i++) {
        const size_t p = r->bit_pos + i;
        value <<= 1;
        if (r->buf[p >> 3] & (uint8_t)(1u << (7u - (p & 7u)))) {
            value |= 1u;
        }
    }
    r->bit_pos += bits;
    return value;
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CRSF_BITS_H */
