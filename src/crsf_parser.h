/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_parser.h
 * @brief Byte-fed CRSF frame parser and frame builders.
 *
 * Reentrant and free of any RTOS / UART dependency: feed it bytes from wherever
 * they come from and it emits validated frames through a callback.
 *
 * Replaces the previous "one UART event == one complete frame" assumption,
 * which mis-parsed fragmented and coalesced reads, and which derived a stack VLA
 * size from an unvalidated length byte.
 *
 * @see crsf_codec.h to turn a reported frame's payload into a typed struct.
 */

#ifndef CRSF_PARSER_H
#define CRSF_PARSER_H

#include "crsf_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup crsf_parser Frame parser and builders
 * @brief Turning a byte stream into frames, and structs back into frames.
 * @{
 */

/**
 * @brief Parser health counters.
 *
 * Worth logging periodically: a rising @ref crsf_parser_stats_t::bad_crc with a
 * steady @ref crsf_parser_stats_t::frames_ok is the classic signature of a
 * marginal wire or a baudrate mismatch, and on a half-duplex link it usually
 * means echo suppression is not working.
 */
typedef struct {
    uint32_t frames_ok;  /**< frames passed to the callback */
    uint32_t bad_length; /**< length field outside 2..62 (crsf.md:171) */
    uint32_t bad_crc;    /**< frame CRC mismatch */
    uint32_t bad_sync;   /**< bytes dropped while hunting for a sync byte */
    uint32_t resyncs;    /**< how often we restarted mid-frame */
    uint32_t short_ext;  /**< extended frame too short to hold dest+origin */
} crsf_parser_stats_t;

/**
 * @brief Called for each validated frame.
 *
 * @param frame Validated frame. @p frame->payload points into the parser's
 *              internal buffer and is only valid during this call — copy it if
 *              you need it afterwards.
 * @param ctx   Opaque pointer supplied to crsf_parser_init().
 */
typedef void (*crsf_parser_cb_t)(const crsf_frame_t *frame, void *ctx);

/**
 * @brief Parser instance.
 *
 * Allocate one per CRSF port, initialise with crsf_parser_init() and treat the
 * fields as private. Read the counters through crsf_parser_stats().
 */
typedef struct {
    /** Where in a frame the byte stream currently sits. */
    enum {
        CRSF_PARSE_SYNC = 0, /**< hunting for a valid sync byte */
        CRSF_PARSE_LENGTH,   /**< next byte is the length field */
        CRSF_PARSE_BODY,     /**< collecting type .. crc */
    } state;

    uint8_t sync;                      /**< sync byte of the frame in progress */
    uint8_t expected;                  /**< value of the length field */
    uint8_t received;                  /**< bytes of body collected */
    uint8_t body[CRSF_MAX_FRAME_BODY]; /**< type .. crc, fixed size */

    crsf_parser_cb_t cb;       /**< frame callback, may be NULL */
    void *ctx;                 /**< opaque context for @ref cb */
    crsf_parser_stats_t stats; /**< health counters */
} crsf_parser_t;

/**
 * @brief Initialise a parser.
 *
 * @param p   Parser instance.
 * @param cb  Frame callback. May be NULL, in which case frames are counted and
 *            discarded (useful for measuring link health only).
 * @param ctx Passed back to @p cb unchanged.
 */
void crsf_parser_init(crsf_parser_t *p, crsf_parser_cb_t cb, void *ctx);

/**
 * @brief Drop any partially-collected frame and hunt for a fresh sync byte.
 *
 * Call this after a UART FIFO overflow or a break condition: the byte stream has
 * a hole in it, so whatever is half-collected is meaningless.
 *
 * @param p Parser instance. Counters are preserved; use
 *          crsf_parser_clear_stats() to zero those.
 */
void crsf_parser_reset(crsf_parser_t *p);

/**
 * @brief Feed one byte.
 *
 * @param p Parser instance.
 * @param b The byte. May complete a frame, in which case the callback runs
 *          before this function returns.
 */
void crsf_parser_push_byte(crsf_parser_t *p, uint8_t b);

/**
 * @brief Feed a buffer. Frames may span calls.
 *
 * @param p    Parser instance.
 * @param data Bytes to consume; ignored when NULL.
 * @param len  Number of bytes in @p data. The callback may run several times.
 */
void crsf_parser_push(crsf_parser_t *p, const uint8_t *data, size_t len);

/**
 * @brief Read the health counters.
 *
 * @param p Parser instance.
 * @return Pointer to the live counters, owned by @p p. Valid as long as @p p is.
 */
const crsf_parser_stats_t *crsf_parser_stats(const crsf_parser_t *p);

/**
 * @brief Zero the health counters.
 *
 * @param p Parser instance. The parse position is untouched, so this is safe to
 *          call mid-stream.
 */
void crsf_parser_clear_stats(crsf_parser_t *p);

/**
 * @brief Build a complete frame into @p out.
 *
 * Writes sync, length, type, the optional extended header, the payload and the
 * frame CRC. Does not append a command CRC — crsf_build_command() does that.
 *
 * Whether the extended header is emitted follows crsf_type_has_ext_header(), so
 * @p destination and @p origin are silently unused for short-header types.
 *
 * @param out         Destination, must hold at least CRSF_MAX_FRAME_SIZE bytes.
 * @param sync        Leading byte, normally CRSF_SYNC_BYTE.
 * @param type        Frame type, see @ref crsf_frame_type_t.
 * @param destination Extended-header destination; ignored for short-header types.
 * @param origin      Extended-header origin; ignored for short-header types.
 * @param payload     Payload bytes, may be NULL when @p payload_len is 0.
 * @param payload_len Payload length.
 * @return Total frame size in bytes including sync and CRC, or 0 if it would not
 *         fit in CRSF_MAX_FRAME_SIZE.
 */
size_t crsf_build_frame(uint8_t *out, uint8_t sync, uint8_t type,
                        uint8_t destination, uint8_t origin,
                        const uint8_t *payload, size_t payload_len);

/**
 * @brief Build a 0x32 Direct Command frame with both CRCs.
 *
 * The order matters and is the part implementations habitually get wrong
 * (crsf.md:936-944):
 *   1. command CRC (poly 0xBA) over type, dest, origin, command id, sub id, args
 *   2. frame CRC (poly 0xD5) over that same region *including* the command CRC
 *
 * @param out         Destination, at least CRSF_MAX_FRAME_SIZE bytes.
 * @param destination Target device address.
 * @param origin      Our own device address.
 * @param command_id  Command set, see @ref crsf_command_id_t.
 * @param sub_id      Sub-command within that set.
 * @param args        Argument bytes, may be NULL when @p args_len is 0.
 * @param args_len    Argument length.
 * @return Total frame size in bytes, or 0 if it would not fit.
 *
 * @see crsf_command_crc_ok() for the receiving side.
 */
size_t crsf_build_command(uint8_t *out, uint8_t destination, uint8_t origin,
                          uint8_t command_id, uint8_t sub_id,
                          const uint8_t *args, size_t args_len);

/**
 * @brief Validate the trailing command CRC of a received 0x32 payload.
 *
 * The parser only checks the outer frame CRC, because that is all it can do
 * generically. A command frame must additionally pass this check before its
 * arguments are acted on.
 *
 * @param frame A frame the parser reported with type CRSF_TYPE_COMMAND.
 * @retval true  The 0xBA CRC checks out.
 * @retval false Mismatch, wrong frame type, or payload too short to hold one.
 */
bool crsf_command_crc_ok(const crsf_frame_t *frame);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CRSF_PARSER_H */
