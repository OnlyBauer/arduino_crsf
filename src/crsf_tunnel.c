/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_tunnel.c
 * @brief MAVLink envelope and MSP chunking / reassembly.
 */

#include "crsf_tunnel.h"
#include <string.h>

/** Placeholder: ISO C forbids an empty translation unit, and every function
 *  in this file can be configured out. */
typedef int crsf_tunnel_translation_unit_is_not_empty;

/* ------------------------------------------------------------------------- */
/* 0xAA MAVLink Envelope                                                     */
/* ------------------------------------------------------------------------- */
#if CRSF_ENABLE_MAVLINK

void crsf_mavlink_reasm_reset(crsf_mavlink_reasm_t *ctx)
{
    ctx->len = 0;
    ctx->total_chunks = 0;
    ctx->next_chunk = 0;
    ctx->active = false;
}

crsf_reasm_result_t crsf_mavlink_reasm_push(crsf_mavlink_reasm_t *ctx,
                                            const crsf_mavlink_envelope_t *chunk)
{
    if (chunk->data_size > CRSF_MAVLINK_CHUNK_MAX ||
        chunk->current_chunk > chunk->total_chunks) {
        crsf_mavlink_reasm_reset(ctx);
        return CRSF_REASM_MALFORMED;
    }

    /*
     * Chunk 0 always starts fresh. Without this a lost final chunk would leave
     * the reassembler waiting forever and swallow every following frame.
     */
    if (chunk->current_chunk == 0) {
        crsf_mavlink_reasm_reset(ctx);
        ctx->active = true;
        ctx->total_chunks = chunk->total_chunks;
        ctx->next_chunk = 0;
    } else if (!ctx->active) {
        /* Mid-frame chunk with no frame in progress: we missed the start. */
        return CRSF_REASM_SEQUENCE_ERROR;
    } else if (chunk->current_chunk != ctx->next_chunk ||
               chunk->total_chunks != ctx->total_chunks) {
        crsf_mavlink_reasm_reset(ctx);
        return CRSF_REASM_SEQUENCE_ERROR;
    }

    if ((size_t)ctx->len + chunk->data_size > sizeof(ctx->buf)) {
        crsf_mavlink_reasm_reset(ctx);
        return CRSF_REASM_OVERFLOW;
    }

    memcpy(&ctx->buf[ctx->len], chunk->data, chunk->data_size);
    ctx->len = (uint16_t)(ctx->len + chunk->data_size);

    if (chunk->current_chunk == ctx->total_chunks) {
        ctx->active = false;
        ctx->next_chunk = 0;
        /*
         * A "complete" frame of zero bytes is meaningless — no MAVLink frame is
         * empty — and handing an empty buffer to a consumer invites it to index
         * into nothing. Treat it as malformed instead. Reachable from a peer
         * sending data_size = 0 on a single-chunk envelope.
         */
        if (ctx->len == 0) {
            return CRSF_REASM_MALFORMED;
        }
        return CRSF_REASM_COMPLETE;
    }

    ctx->next_chunk = (uint8_t)(chunk->current_chunk + 1);
    return CRSF_REASM_NEED_MORE;
}

bool crsf_mavlink_split_init_chunked(crsf_mavlink_split_t *ctx, const uint8_t *data,
                                     size_t len, uint8_t chunk_max)
{
    if (len == 0 || data == NULL) {
        return false;
    }
    if (chunk_max == 0 || chunk_max > CRSF_MAVLINK_CHUNK_MAX) {
        return false;
    }
    /*
     * Deliberately not capped at CRSF_MAVLINK_FRAME_MAX here. The 4-bit index
     * is the wire's own limit and this is the primitive that expresses it; a
     * caller with a larger reassembly buffer than ours is entitled to use it,
     * and test_mavlink_reassembly_overflow() relies on being able to build a
     * 928-byte split to exercise the receiver's overflow path. The cap against
     * what *we* can reassemble belongs to crsf_mavlink_send(), which is where
     * esp_crsf.h documents it.
     */
    const size_t chunks = (len + chunk_max - 1) / chunk_max;
    /* current_chunk and total_chunks are 4 bits each (crsf.md:1305). */
    if (chunks > 16) {
        return false;
    }

    ctx->data = data;
    ctx->len = len;
    ctx->chunk_max = chunk_max;
    ctx->total_chunks = (uint8_t)(chunks - 1); /* zero-based last index */
    ctx->next = 0;
    ctx->done = false;
    return true;
}

bool crsf_mavlink_split_init(crsf_mavlink_split_t *ctx, const uint8_t *data, size_t len)
{
    return crsf_mavlink_split_init_chunked(ctx, data, len, CRSF_MAVLINK_CHUNK_MAX);
}

bool crsf_mavlink_split_next(crsf_mavlink_split_t *ctx, crsf_mavlink_envelope_t *out)
{
    if (ctx->done || ctx->next > ctx->total_chunks) {
        return false;
    }

    const size_t offset = (size_t)ctx->next * ctx->chunk_max;
    size_t remaining = ctx->len - offset;
    if (remaining > ctx->chunk_max) {
        remaining = ctx->chunk_max;
    }

    memset(out, 0, sizeof(*out));
    out->total_chunks = ctx->total_chunks;
    out->current_chunk = ctx->next;
    out->data_size = (uint8_t)remaining;
    memcpy(out->data, &ctx->data[offset], remaining);

    if (ctx->next == ctx->total_chunks) {
        ctx->done = true;
    } else {
        ctx->next++;
    }
    return true;
}

#endif /* CRSF_ENABLE_MAVLINK */

/* ------------------------------------------------------------------------- */
/* 0x7A / 0x7B MSP                                                           */
/* ------------------------------------------------------------------------- */
#if CRSF_ENABLE_MSP

bool crsf_msp_body_length(const uint8_t *body, size_t have, uint8_t version,
                          size_t *out_total)
{
    if (version == 2) {
        /* [flag][function:2][size:2][payload] — crsf.md:1226 calls this 5 bytes. */
        if (have < 5) {
            return false;
        }
        const size_t size = (size_t)body[3] | ((size_t)body[4] << 8);
        *out_total = 5u + size;
        return true;
    }

    /* MSP v1. */
    if (have < 1) {
        return false;
    }
    if (body[0] == 0xFF) {
        /* Jumbo: [0xFF][function][size:2][payload]. */
        if (have < 4) {
            return false;
        }
        const size_t size = (size_t)body[2] | ((size_t)body[3] << 8);
        *out_total = 4u + size;
        return true;
    }
    /* Plain: [size][function][payload]. */
    if (have < 2) {
        return false;
    }
    *out_total = 2u + (size_t)body[0];
    return true;
}

void crsf_msp_reasm_reset(crsf_msp_reasm_t *ctx)
{
    ctx->len = 0;
    ctx->expected = 0;
    ctx->version = 0;
    ctx->error = false;
    ctx->next_seq = 0;
    ctx->active = false;
}

crsf_reasm_result_t crsf_msp_reasm_push(crsf_msp_reasm_t *ctx,
                                        const uint8_t *payload, size_t len)
{
    if (len < 1) {
        return CRSF_REASM_MALFORMED;
    }

    const uint8_t status = payload[0];
    const uint8_t *data = &payload[1];
    size_t data_len = len - 1;

    if (data_len > CRSF_MSP_CHUNK_MAX) {
        /* A frame longer than the spec's chunk cap; trust the cap, not the frame. */
        data_len = CRSF_MSP_CHUNK_MAX;
    }

    if (crsf_msp_status_is_start(status)) {
        crsf_msp_reasm_reset(ctx);
        ctx->active = true;
        ctx->version = crsf_msp_status_version(status);
        ctx->error = crsf_msp_status_is_error(status);
        ctx->next_seq = crsf_msp_status_seq(status);

        if (ctx->version != 1 && ctx->version != 2) {
            crsf_msp_reasm_reset(ctx);
            return CRSF_REASM_MALFORMED;
        }
    } else if (!ctx->active) {
        return CRSF_REASM_SEQUENCE_ERROR;
    } else if (crsf_msp_status_seq(status) != ctx->next_seq) {
        crsf_msp_reasm_reset(ctx);
        return CRSF_REASM_SEQUENCE_ERROR;
    }

    if (ctx->len + data_len > sizeof(ctx->body)) {
        crsf_msp_reasm_reset(ctx);
        return CRSF_REASM_OVERFLOW;
    }
    memcpy(&ctx->body[ctx->len], data, data_len);
    ctx->len += data_len;

    /* The sequence number is cyclic over 4 bits (crsf.md:1220). */
    ctx->next_seq = (uint8_t)((ctx->next_seq + 1u) & CRSF_MSP_STATUS_SEQ_MASK);

    if (ctx->expected == 0) {
        size_t total = 0;
        if (crsf_msp_body_length(ctx->body, ctx->len, ctx->version, &total)) {
            if (total > sizeof(ctx->body)) {
                crsf_msp_reasm_reset(ctx);
                return CRSF_REASM_OVERFLOW;
            }
            ctx->expected = total;
        }
    }

    if (ctx->expected != 0 && ctx->len >= ctx->expected) {
        /*
         * crsf.md:1225 — the last chunk may be longer than needed, and the extra
         * bytes must be ignored. Trim to the declared length.
         */
        ctx->len = ctx->expected;
        ctx->active = false;
        return CRSF_REASM_COMPLETE;
    }

    return CRSF_REASM_NEED_MORE;
}

bool crsf_msp_split_init(crsf_msp_split_t *ctx, const uint8_t *body, size_t len,
                         uint8_t version, bool error, uint8_t seq)
{
    /*
     * The cap is the receiving reassembler's buffer size: a body longer than
     * CRSF_MSP_BODY_MAX cannot be put back together at the far end, so splitting
     * it would only produce chunks that are guaranteed to end in CRSF_REASM_OVERFLOW.
     */
    if (body == NULL || len == 0 || len > CRSF_MSP_BODY_MAX ||
        (version != 1 && version != 2)) {
        return false;
    }
    ctx->body = body;
    ctx->len = len;
    ctx->sent = 0;
    ctx->version = version;
    ctx->error = error;
    ctx->seq = (uint8_t)(seq & CRSF_MSP_STATUS_SEQ_MASK);
    ctx->first = true;
    return true;
}

size_t crsf_msp_split_next(crsf_msp_split_t *ctx, uint8_t *out, size_t out_size)
{
    if (ctx->sent >= ctx->len || out_size < 2) {
        return 0;
    }

    size_t chunk = ctx->len - ctx->sent;
    const size_t room = out_size - 1;
    if (chunk > CRSF_MSP_CHUNK_MAX) {
        chunk = CRSF_MSP_CHUNK_MAX;
    }
    if (chunk > room) {
        chunk = room;
    }

    out[0] = crsf_msp_status(ctx->seq, ctx->first, ctx->version, ctx->error);
    memcpy(&out[1], &ctx->body[ctx->sent], chunk);

    ctx->sent += chunk;
    ctx->seq = (uint8_t)((ctx->seq + 1u) & CRSF_MSP_STATUS_SEQ_MASK);
    ctx->first = false;

    return chunk + 1;
}

#endif /* CRSF_ENABLE_MSP */
