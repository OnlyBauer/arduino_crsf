/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_tunnel.h
 * @brief Chunking and reassembly for the CRSF tunnel frames.
 *
 * Two protocols ride inside CRSF frames and both need splitting on the way out
 * and reassembly on the way in, because their frames are larger than CRSF's
 * 64-byte limit:
 *
 *  - **0xAA MAVLink Envelope** (crsf.md:1295) — up to 281 bytes of MAVLink2,
 *    58 bytes of data per chunk, chunk indices in one byte's two nibbles.
 *  - **0x7A / 0x7B MSP** (crsf.md:1200) — a status byte carrying a cyclic
 *    sequence number, a start-of-frame flag, the MSP version and an error bit,
 *    followed by up to 57 bytes of MSP body.
 *
 * Deliberately free of ESP-IDF: this is pure state-machine work and belongs in
 * the host-testable core.
 *
 * Each direction has its own type. A *reassembler* is fed chunk after chunk and
 * reports when a frame is whole; a *splitter* borrows a frame and hands out
 * chunks until it is exhausted. Both are plain structs with no allocation, so a
 * port owns them by value.
 */

#ifndef CRSF_TUNNEL_H
#define CRSF_TUNNEL_H

#include "crsf_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup crsf_tunnel Tunnels
 * @brief MAVLink and MSP chunking and reassembly.
 * @{
 */

/** Outcome of feeding one chunk into a reassembler. */
typedef enum {
    CRSF_REASM_NEED_MORE = 0,  /**< chunk accepted, frame incomplete */
    CRSF_REASM_COMPLETE,       /**< frame complete and available */
    CRSF_REASM_SEQUENCE_ERROR, /**< out-of-order or missing chunk; state reset */
    CRSF_REASM_OVERFLOW,       /**< frame larger than the reassembly buffer */
    CRSF_REASM_MALFORMED,      /**< header/length inconsistent */
} crsf_reasm_result_t;

#if CRSF_ENABLE_MAVLINK
/**
 * @name 0xAA MAVLink Envelope
 * @{
 */

/**
 * @brief Reassembly state for inbound MAVLink envelopes.
 *
 * Initialise by zeroing, or with crsf_mavlink_reasm_reset(). Treat the fields as
 * read-only; @ref buf and @ref len are the result on completion.
 */
typedef struct {
    uint8_t buf[CRSF_MAVLINK_FRAME_MAX]; /**< collected frame */
    uint16_t len;                        /**< bytes collected so far */
    uint8_t total_chunks;                /**< index of the last chunk, from the first chunk */
    uint8_t next_chunk;                  /**< index we expect next */
    bool active;                         /**< a frame is in progress */
} crsf_mavlink_reasm_t;

/**
 * @brief Discard any partial frame.
 *
 * @param ctx Reassembler to clear. Safe to call on a zeroed struct.
 */
void crsf_mavlink_reasm_reset(crsf_mavlink_reasm_t *ctx);

/**
 * @brief Feed one decoded envelope chunk.
 *
 * On CRSF_REASM_COMPLETE the frame is in `ctx->buf` with length `ctx->len`,
 * valid until the next push. A chunk with index 0 always starts a fresh frame,
 * so a lost tail does not wedge the reassembler.
 *
 * @param ctx   Reassembler.
 * @param chunk One decoded envelope, see crsf_decode_mavlink_envelope().
 * @return What happened, see @ref crsf_reasm_result_t.
 *
 * @note A frame that declares completion with zero total bytes is rejected as
 *       CRSF_REASM_MALFORMED rather than reported complete — an empty "frame"
 *       is meaningless and would hand a consumer a buffer it may index.
 */
crsf_reasm_result_t crsf_mavlink_reasm_push(crsf_mavlink_reasm_t *ctx,
                                            const crsf_mavlink_envelope_t *chunk);

/**
 * @brief Splitter state for outbound MAVLink frames.
 *
 * Initialise with crsf_mavlink_split_init(); treat the fields as private.
 */
typedef struct {
    const uint8_t *data;  /**< borrowed source frame, not copied */
    size_t len;           /**< length of @ref data */
    uint8_t chunk_max;    /**< data bytes per chunk */
    uint8_t total_chunks; /**< index of the last chunk */
    uint8_t next;         /**< index of the chunk to emit next */
    bool done;            /**< every chunk has been produced */
} crsf_mavlink_split_t;

/**
 * @brief Prepare to split @p data into envelope chunks.
 *
 * @p data must stay valid until splitting finishes; nothing is copied.
 *
 * Equivalent to crsf_mavlink_split_init_chunked() with the full
 * CRSF_MAVLINK_CHUNK_MAX budget, which is what a short-header 0xAA frame
 * allows.
 *
 * @note The only length limit here is the wire's own 16-chunk index. That
 *       allows more than crsf_mavlink_reasm_t can hold; capping against our own
 *       reassembly buffer is crsf_mavlink_send()'s job.
 *
 * @param ctx  Splitter to initialise.
 * @param data MAVLink frame to send.
 * @param len  Length of @p data.
 * @retval true  Ready; call crsf_mavlink_split_next() until it returns false.
 * @retval false @p len is 0 or would need more than 16 chunks, which is all a
 *               4-bit index can express (crsf.md:1305).
 */
bool crsf_mavlink_split_init(crsf_mavlink_split_t *ctx, const uint8_t *data, size_t len);

/**
 * @brief Prepare to split @p data using a reduced per-chunk budget.
 *
 * Needed when the envelope is sent with an extended header, which costs two of
 * the frame's payload bytes and so leaves only CRSF_MAVLINK_CHUNK_MAX - 2 for
 * chunk data. Splitting at the full budget in that mode produces a chunk that
 * cannot fit its own frame, and the send fails on chunk 0.
 *
 * @param ctx       Splitter to initialise.
 * @param data      MAVLink frame to send.
 * @param len       Length of @p data.
 * @param chunk_max Data bytes per chunk, 1 .. CRSF_MAVLINK_CHUNK_MAX.
 * @retval true  Ready; call crsf_mavlink_split_next() until it returns false.
 * @retval false @p len is 0, @p chunk_max is out of range, or the split would
 *               need more than 16 chunks.
 */
bool crsf_mavlink_split_init_chunked(crsf_mavlink_split_t *ctx, const uint8_t *data,
                                     size_t len, uint8_t chunk_max);

/**
 * @brief Produce the next chunk.
 *
 * @param ctx Splitter.
 * @param out Receives the chunk, ready for crsf_encode_mavlink_envelope().
 * @retval true  @p out was filled in.
 * @retval false Every chunk has already been produced.
 */
bool crsf_mavlink_split_next(crsf_mavlink_split_t *ctx, crsf_mavlink_envelope_t *out);

/** @} */
#endif /* CRSF_ENABLE_MAVLINK */
#if CRSF_ENABLE_MSP

/**
 * @name 0x7A / 0x7B MSP
 * @{
 */

/**
 * @brief Largest MSP body we reassemble.
 *
 * MSP itself allows up to 65535 payload bytes, which no CRSF link would carry
 * sensibly; larger frames are rejected with CRSF_REASM_OVERFLOW rather than
 * silently truncated.
 */
#ifndef CRSF_MSP_BODY_MAX
#define CRSF_MSP_BODY_MAX 512
#endif

/**
 * @brief Build an MSP status byte (crsf.md:1219).
 *
 * @param seq     Sequence number, low 4 bits used.
 * @param start   Start-of-frame flag; set on the first chunk only.
 * @param version MSP version, 1 or 2, low 2 bits used.
 * @param error   Error flag, meaningful on responses (crsf.md:1223).
 * @return The assembled status byte.
 */
static inline uint8_t crsf_msp_status(uint8_t seq, bool start, uint8_t version,
                                      bool error)
{
    return (uint8_t)((seq & CRSF_MSP_STATUS_SEQ_MASK) |
                     (start ? CRSF_MSP_STATUS_START : 0u) |
                     ((uint8_t)((version & 0x03u) << CRSF_MSP_STATUS_VER_SHIFT)) |
                     (error ? CRSF_MSP_STATUS_ERROR : 0u));
}

/**
 * @brief Extract the sequence number from a status byte.
 * @param s Status byte.
 * @return Sequence number, 0..15.
 */
static inline uint8_t crsf_msp_status_seq(uint8_t s)
{
    return (uint8_t)(s & CRSF_MSP_STATUS_SEQ_MASK);
}

/**
 * @brief Test the start-of-frame flag.
 * @param s Status byte.
 * @return true when this chunk begins a new MSP frame.
 */
static inline bool crsf_msp_status_is_start(uint8_t s)
{
    return (s & CRSF_MSP_STATUS_START) != 0;
}

/**
 * @brief Extract the MSP version from a status byte.
 * @param s Status byte.
 * @return MSP version, normally 1 or 2.
 */
static inline uint8_t crsf_msp_status_version(uint8_t s)
{
    return (uint8_t)((s & CRSF_MSP_STATUS_VER_MASK) >> CRSF_MSP_STATUS_VER_SHIFT);
}

/**
 * @brief Test the error flag.
 * @param s Status byte.
 * @return true when the peer is reporting an error for this response.
 */
static inline bool crsf_msp_status_is_error(uint8_t s)
{
    return (s & CRSF_MSP_STATUS_ERROR) != 0;
}

/**
 * @brief Total MSP body length, derived from the body's own header.
 *
 * crsf.md:1224 requires the length to come from the body rather than the frame
 * size, honouring the MSP version and the v1 jumbo form. The body here is the
 * MSP frame stripped of its `$M<`-style header and trailing CRC (crsf.md:1216).
 *
 * Layouts, all little-endian because that is MSP's own convention and not
 * CRSF's big-endian:
 *  - v1:       `[size][function][payload]`            -> 2 + size
 *  - v1 jumbo: `[0xFF][function][size:2][payload]`    -> 4 + size
 *  - v2:       `[flag][function:2][size:2][payload]`  -> 5 + size
 *
 * @param body      Bytes collected so far.
 * @param have      Number of bytes in @p body.
 * @param version   1 or 2, from the status byte.
 * @param out_total Receives the total body length when known; untouched
 *                  otherwise.
 * @retval true  @p out_total was set.
 * @retval false Too few bytes are present to decide yet.
 */
bool crsf_msp_body_length(const uint8_t *body, size_t have, uint8_t version,
                          size_t *out_total);

/**
 * @brief Reassembly state for inbound MSP frames.
 *
 * Initialise by zeroing, or with crsf_msp_reasm_reset(). On completion the frame
 * is @ref body with length @ref len.
 */
typedef struct {
    uint8_t body[CRSF_MSP_BODY_MAX]; /**< collected MSP body */
    size_t len;                      /**< bytes collected */
    size_t expected;                 /**< total length once derivable, else 0 */
    uint8_t version;                 /**< 1 or 2, from the first chunk */
    bool error;                      /**< error bit from the first chunk */
    uint8_t next_seq;                /**< sequence number expected next */
    bool active;                     /**< a frame is in progress */
} crsf_msp_reasm_t;

/**
 * @brief Discard any partial MSP frame.
 *
 * @param ctx Reassembler to clear. Safe to call on a zeroed struct.
 */
void crsf_msp_reasm_reset(crsf_msp_reasm_t *ctx);

/**
 * @brief Feed one 0x7A/0x7B payload, status byte included.
 *
 * A chunk with the start flag set always begins a new frame. Once the declared
 * length is reached the frame is complete and any surplus bytes in the final
 * chunk are ignored, as crsf.md:1225 requires.
 *
 * @param ctx     Reassembler.
 * @param payload Frame payload, status byte first.
 * @param len     Payload length including the status byte.
 * @return What happened, see @ref crsf_reasm_result_t.
 */
crsf_reasm_result_t crsf_msp_reasm_push(crsf_msp_reasm_t *ctx,
                                        const uint8_t *payload, size_t len);

/**
 * @brief Splitter state for outbound MSP frames.
 *
 * Initialise with crsf_msp_split_init(); treat the fields as private.
 */
typedef struct {
    const uint8_t *body; /**< borrowed source body, not copied */
    size_t len;          /**< length of @ref body */
    size_t sent;         /**< bytes handed out so far */
    uint8_t version;     /**< MSP version to stamp into each status byte */
    bool error;          /**< error bit to stamp into each status byte */
    uint8_t seq;         /**< sequence number of the next chunk */
    bool first;          /**< next chunk still carries the start flag */
} crsf_msp_split_t;

/**
 * @brief Prepare to split an MSP body into 0x7A/0x7B payloads.
 *
 * @p body must stay valid until splitting finishes; nothing is copied.
 *
 * @param ctx     Splitter to initialise.
 * @param body    MSP body, header and CRC already stripped.
 * @param len     Length of @p body.
 * @param version 1 or 2.
 * @param error   Error bit, meaningful on responses only (crsf.md:1223).
 * @param seq     Starting sequence number; increments per chunk, wrapping at 16.
 * @retval true  Ready; call crsf_msp_split_next() until it returns 0.
 * @retval false @p body is NULL, @p len is 0 or exceeds CRSF_MSP_BODY_MAX, or
 *               @p version is neither 1 nor 2.
 */
bool crsf_msp_split_init(crsf_msp_split_t *ctx, const uint8_t *body, size_t len,
                         uint8_t version, bool error, uint8_t seq);

/**
 * @brief Produce the next payload, status byte included.
 *
 * @param ctx      Splitter.
 * @param out      Destination, at least 1 + CRSF_MSP_CHUNK_MAX bytes.
 * @param out_size Capacity of @p out.
 * @return Payload length including the status byte, or 0 once finished or when
 *         @p out_size is too small for even one body byte.
 */
size_t crsf_msp_split_next(crsf_msp_split_t *ctx, uint8_t *out, size_t out_size);

/** @} */
#endif /* CRSF_ENABLE_MSP */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CRSF_TUNNEL_H */
