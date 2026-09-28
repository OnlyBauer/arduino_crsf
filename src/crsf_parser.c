/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_parser.c
 * @brief Byte-fed CRSF frame parser and frame builders.
 */

#include "crsf_parser.h"
#include "crsf_crc.h"
#include <string.h>

void crsf_parser_init(crsf_parser_t *p, crsf_parser_cb_t cb, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->state = CRSF_PARSE_SYNC;
    p->cb = cb;
    p->ctx = ctx;
}

void crsf_parser_reset(crsf_parser_t *p)
{
    if (p->state != CRSF_PARSE_SYNC) {
        p->stats.resyncs++;
    }
    p->state = CRSF_PARSE_SYNC;
    p->expected = 0;
    p->received = 0;
}

const crsf_parser_stats_t *crsf_parser_stats(const crsf_parser_t *p)
{
    return &p->stats;
}

void crsf_parser_clear_stats(crsf_parser_t *p)
{
    memset(&p->stats, 0, sizeof(p->stats));
}

/**
 * @brief Hand a completed, CRC-checked body to the callback.
 *
 * Body layout is `[type][ext dest][ext origin][payload...][crc]`, with the CRC
 * already verified and excluded here. An extended-header frame too short to hold
 * both address bytes is counted and dropped rather than reported with a payload
 * pointer past its own buffer.
 *
 * @param p Parser instance; its @c body holds the frame and @c cb receives it.
 */
static void emit(crsf_parser_t *p)
{
    const uint8_t type = p->body[0];
    /* Body holds type + payload + crc, so strip one byte at each end. */
    const uint8_t *payload = &p->body[1];
    uint8_t payload_len = (uint8_t)(p->expected - 2);

    crsf_frame_t frame = {
        .sync = p->sync,
        .type = type,
        .is_extended = false,
        .destination = 0,
        .origin = 0,
        .payload = payload,
        .payload_len = payload_len,
    };

    if (crsf_type_has_ext_header(type)) {
        if (payload_len < 2) {
            /* Cannot possibly hold destination + origin. */
            p->stats.short_ext++;
            return;
        }
        frame.is_extended = true;
        frame.destination = payload[0];
        frame.origin = payload[1];
        frame.payload = payload + 2;
        frame.payload_len = (uint8_t)(payload_len - 2);
    }

    p->stats.frames_ok++;
    if (p->cb) {
        p->cb(&frame, p->ctx);
    }
}

void crsf_parser_push_byte(crsf_parser_t *p, uint8_t b)
{
    switch (p->state) {
    case CRSF_PARSE_SYNC:
        /*
         * crsf.md:164 — the leading byte may be 0xC8, 0x00, or any device
         * address, so we cannot hard-match 0xC8 the way many implementations do.
         * Anything unrecognised is dropped and counted.
         */
        if (crsf_is_valid_sync(b)) {
            p->sync = b;
            p->received = 0;
            p->state = CRSF_PARSE_LENGTH;
        } else {
            p->stats.bad_sync++;
        }
        break;

    case CRSF_PARSE_LENGTH:
        /*
         * crsf.md:171 — "Valid range is between 2 and 62. If this uint8 value is
         * out of valid range the frame must be discarded." This is the check
         * whose absence let the old code compute `length - 2` into a uint8
         * underflow and size a stack VLA from it.
         */
        if (b < CRSF_FRAME_LEN_MIN || b > CRSF_FRAME_LEN_MAX) {
            p->stats.bad_length++;
            p->state = CRSF_PARSE_SYNC;
            /*
             * The rejected byte may itself be the start of a real frame — this
             * happens whenever we locked onto a 0xC8 that was actually payload.
             * Re-examine it as a sync candidate rather than dropping it.
             */
            if (crsf_is_valid_sync(b)) {
                p->sync = b;
                p->received = 0;
                p->state = CRSF_PARSE_LENGTH;
            }
            break;
        }
        p->expected = b;
        p->received = 0;
        p->state = CRSF_PARSE_BODY;
        break;

    case CRSF_PARSE_BODY:
        /* Fixed-size buffer; `expected` is already bounded by the check above. */
        p->body[p->received++] = b;
        if (p->received < p->expected) {
            break;
        }

        /*
         * crsf.md:185 — CRC covers Type + Payload, excluding sync and length.
         * That is body[0 .. expected-2], with body[expected-1] holding the CRC.
         * The old receive path never did this at all: any corrupt frame with
         * type 0x16 was accepted as valid channel data.
         */
        if (crsf_crc8(p->body, (size_t)(p->expected - 1)) ==
            p->body[p->expected - 1]) {
            emit(p);
        } else {
            p->stats.bad_crc++;
        }

        /*
         * Resynchronise from the next incoming byte rather than rescanning the
         * body for a sync candidate. On a continuous CRSF stream frames arrive
         * back-to-back at fixed sizes, so alignment recovers within a frame or
         * two; this is also what the wider ecosystem (Betaflight, ArduPilot)
         * does. Rescanning would need an ordered re-feed queue for a marginal
         * gain in recovery latency.
         */
        p->state = CRSF_PARSE_SYNC;
        p->received = 0;
        break;

    default:
        p->state = CRSF_PARSE_SYNC;
        break;
    }
}

void crsf_parser_push(crsf_parser_t *p, const uint8_t *data, size_t len)
{
    if (!data) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        crsf_parser_push_byte(p, data[i]);
    }
}

/* ------------------------------------------------------------------------- */
/* Frame builders                                                            */
/* ------------------------------------------------------------------------- */

size_t crsf_build_frame(uint8_t *out, uint8_t sync, uint8_t type,
                        uint8_t destination, uint8_t origin,
                        const uint8_t *payload, size_t payload_len)
{
    const bool ext = crsf_type_has_ext_header(type);
    const size_t ext_len = ext ? 2u : 0u;

    /*
     * Bound payload_len before it enters the body_len sum below. Without this,
     * a caller-supplied payload_len near SIZE_MAX makes `1 + ext_len +
     * payload_len + 1` wrap around to a small value that passes the range
     * check that follows, after which the memcpy() below would run with the
     * original, enormous payload_len.
     */
    if (payload_len > CRSF_FRAME_LEN_MAX) {
        return 0;
    }

    /* body = type + [dest + origin] + payload + crc */
    const size_t body_len = 1 + ext_len + payload_len + 1;
    if (body_len < CRSF_FRAME_LEN_MIN || body_len > CRSF_FRAME_LEN_MAX) {
        return 0;
    }

    size_t i = 0;
    out[i++] = sync;
    out[i++] = (uint8_t)body_len;

    const size_t body_start = i;
    out[i++] = type;
    if (ext) {
        out[i++] = destination;
        out[i++] = origin;
    }
    if (payload_len) {
        memcpy(&out[i], payload, payload_len);
        i += payload_len;
    }

    /* CRC over type .. last payload byte (crsf.md:185). */
    out[i] = crsf_crc8(&out[body_start], i - body_start);
    i++;

    return i;
}

size_t crsf_build_command(uint8_t *out, uint8_t destination, uint8_t origin,
                          uint8_t command_id, uint8_t sub_id,
                          const uint8_t *args, size_t args_len)
{
    /* Bound args_len first; see the matching comment in crsf_build_frame(). */
    if (args_len > CRSF_FRAME_LEN_MAX) {
        return 0;
    }

    /* body = type + dest + origin + cmd + sub + args + cmd_crc + frame_crc */
    const size_t body_len = 1 + 2 + 1 + 1 + args_len + 1 + 1;
    if (body_len < CRSF_FRAME_LEN_MIN || body_len > CRSF_FRAME_LEN_MAX) {
        return 0;
    }

    size_t i = 0;
    out[i++] = CRSF_SYNC_BYTE;
    out[i++] = (uint8_t)body_len;

    const size_t body_start = i;
    out[i++] = CRSF_TYPE_COMMAND;
    out[i++] = destination;
    out[i++] = origin;
    out[i++] = command_id;
    out[i++] = sub_id;
    if (args_len) {
        memcpy(&out[i], args, args_len);
        i += args_len;
    }

    /*
     * Command CRC (poly 0xBA) over type, destination, origin, command id and
     * payload (crsf.md:936) — i.e. everything from the type up to here.
     */
    out[i] = crsf_crc8_cmd(&out[body_start], i - body_start);
    i++;

    /*
     * Frame CRC (poly 0xD5) over the same region *including* the command CRC:
     * "Command CRC doesn't exclude CRC at the end of each CRSF frame"
     * (crsf.md:944).
     */
    out[i] = crsf_crc8(&out[body_start], i - body_start);
    i++;

    return i;
}

bool crsf_command_crc_ok(const crsf_frame_t *frame)
{
    if (!frame->is_extended || frame->type != CRSF_TYPE_COMMAND) {
        return false;
    }
    /* Need at least a command id and the trailing command CRC. */
    if (frame->payload_len < 2) {
        return false;
    }

    /*
     * The covered region starts at the type byte and runs to just before the
     * command CRC. type, dest and origin are contiguous in the wire frame and
     * sit immediately before frame->payload, so we can reach them by backing up
     * three bytes — the parser handed out a pointer into its own body buffer.
     */
    const uint8_t *start = frame->payload - 3;
    const size_t covered = 3u + (size_t)(frame->payload_len - 1);

    return crsf_crc8_cmd(start, covered) == frame->payload[frame->payload_len - 1];
}
