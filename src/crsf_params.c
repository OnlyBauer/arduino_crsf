/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_params.c
 * @brief Parameter/settings protocol: entry serialisation, chunking, provider, client.
 *
 * Field orders follow crsf.md:766 (FLOAT), :786 (TEXT_SELECTION), :806 (STRING),
 * :821 (FOLDER), :835 (INFO) and :855 (COMMAND). All multi-byte integers are
 * big-endian per crsf.md:173.
 */

#include "crsf_params.h"
#include "crsf_codec.h"
#include <string.h>

/* ------------------------------------------------------------------------- */
/* small write/read helpers, all bounds-checked                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Bounds-checked append cursor over a caller-owned buffer.
 *
 * The point of the @ref overflow flag is that the serialisers can write a whole
 * entry field by field and check once at the end, instead of testing every call.
 */
typedef struct {
    uint8_t *buf;  /**< destination */
    size_t size;   /**< capacity of @ref buf */
    size_t pos;    /**< bytes written so far */
    bool overflow; /**< set once a write did not fit; sticky */
} wbuf_t;

/**
 * @brief Start writing into @p buf.
 * @param w    Cursor to initialise.
 * @param buf  Destination buffer.
 * @param size Capacity of @p buf.
 */
static void wb_init(wbuf_t *w, uint8_t *buf, size_t size)
{
    w->buf = buf;
    w->size = size;
    w->pos = 0;
    w->overflow = false;
}

/**
 * @brief Append one byte.
 * @param w Cursor.
 * @param v Byte to append. Dropped, and @ref wbuf_t::overflow set, if it does
 *          not fit.
 */
static void wb_u8(wbuf_t *w, uint8_t v)
{
    if (w->pos + 1 > w->size) {
        w->overflow = true;
        return;
    }
    w->buf[w->pos++] = v;
}

/**
 * @brief Append a 32-bit value big-endian (crsf.md:173).
 * @param w Cursor.
 * @param v Value to append. Dropped, and @ref wbuf_t::overflow set, if it does
 *          not fit.
 */
static void wb_be32(wbuf_t *w, uint32_t v)
{
    if (w->pos + 4 > w->size) {
        w->overflow = true;
        return;
    }
    crsf_put_be32(&w->buf[w->pos], v);
    w->pos += 4;
}

/**
 * @brief Append a null-terminated string; NULL is written as an empty string.
 * @param w         Cursor.
 * @param s         String to append, or NULL.
 * @param max_chars Characters to copy at most, excluding the terminator, which
 *                  is always written.
 */
static void wb_str(wbuf_t *w, const char *s, size_t max_chars)
{
    const char *src = s ? s : "";
    size_t l = 0;
    while (l < max_chars && src[l] != '\0') {
        l++;
    }
    if (w->pos + l + 1 > w->size) {
        w->overflow = true;
        return;
    }
    memcpy(&w->buf[w->pos], src, l);
    w->pos += l;
    w->buf[w->pos++] = '\0';
}

/**
 * @brief Bounds-checked read cursor over a received payload.
 *
 * Mirror of @ref wbuf_t. A read that would run past the end yields 0 and sets
 * @ref overflow, so a parser can decode every field and reject the entry once
 * at the end — which is what keeps a truncated payload from being read past.
 */
typedef struct {
    const uint8_t *buf; /**< source, not modified */
    size_t size;        /**< bytes available in @ref buf */
    size_t pos;         /**< bytes consumed so far */
    bool overflow;      /**< set once a read ran past the end; sticky */
} rbuf_t;

/**
 * @brief Start reading from @p buf.
 * @param r    Cursor to initialise.
 * @param buf  Source buffer; borrowed.
 * @param size Bytes available in @p buf.
 */
static void rb_init(rbuf_t *r, const uint8_t *buf, size_t size)
{
    r->buf = buf;
    r->size = size;
    r->pos = 0;
    r->overflow = false;
}

/**
 * @brief Read one byte.
 * @param r Cursor.
 * @return The byte, or 0 on overflow. Test @ref rbuf_t::overflow, since 0 is
 *         also a legal value.
 */
static uint8_t rb_u8(rbuf_t *r)
{
    if (r->pos + 1 > r->size) {
        r->overflow = true;
        return 0;
    }
    return r->buf[r->pos++];
}

/**
 * @brief Read a 32-bit big-endian value (crsf.md:173).
 * @param r Cursor.
 * @return The value, or 0 on overflow. Test @ref rbuf_t::overflow, since 0 is
 *         also a legal value.
 */
static uint32_t rb_be32(rbuf_t *r)
{
    if (r->pos + 4 > r->size) {
        r->overflow = true;
        return 0;
    }
    const uint32_t v = crsf_get_be32(&r->buf[r->pos]);
    r->pos += 4;
    return v;
}

/**
 * @brief Read a null-terminated string into a bounded destination.
 *
 * Sets @ref rbuf_t::overflow when no terminator appears before the end of the
 * payload — crsf.md:177 forbids reading past it.
 *
 * @param r        Cursor.
 * @param dst      Destination; always NUL-terminated, empty on overflow.
 * @param dst_size Capacity of @p dst including the terminator. A longer source
 *                 string is truncated, never overflowed.
 */
static void rb_str(rbuf_t *r, char *dst, size_t dst_size)
{
    size_t n = 0;
    while (r->pos + n < r->size && r->buf[r->pos + n] != '\0') {
        n++;
    }
    if (r->pos + n >= r->size) {
        r->overflow = true;
        if (dst_size) {
            dst[0] = '\0';
        }
        return;
    }
    const size_t copy = n < (dst_size - 1) ? n : (dst_size - 1);
    memcpy(dst, &r->buf[r->pos], copy);
    dst[copy] = '\0';
    r->pos += n + 1;
}

/* ------------------------------------------------------------------------- */
/* Entry serialisation                                                       */
/* ------------------------------------------------------------------------- */

/**
 * @brief Build the wire type byte, hidden flag in bit 7 (crsf.md:731).
 * @param p Parameter.
 * @return Type in bits 0-6, CRSF_PARAM_HIDDEN_MASK set when @c p->hidden.
 */
static uint8_t type_byte(const crsf_param_t *p)
{
    return (uint8_t)((p->type & CRSF_PARAM_TYPE_MASK) |
                     (p->hidden ? CRSF_PARAM_HIDDEN_MASK : 0u));
}

size_t crsf_param_serialize(const crsf_param_t *p, uint8_t *out, size_t out_size)
{
    wbuf_t w;
    wb_init(&w, out, out_size);

    wb_u8(&w, p->parent);
    wb_u8(&w, type_byte(p));
    wb_str(&w, p->name, CRSF_PARAM_NAME_MAX - 1);

    switch (p->type) {
    case CRSF_PARAM_FLOAT:
        wb_be32(&w, (uint32_t)p->u.flt.value);
        wb_be32(&w, (uint32_t)p->u.flt.min);
        wb_be32(&w, (uint32_t)p->u.flt.max);
        wb_be32(&w, (uint32_t)p->u.flt.deflt);
        wb_u8(&w, p->u.flt.dec_point);
        wb_be32(&w, (uint32_t)p->u.flt.step);
        wb_str(&w, p->u.flt.unit, CRSF_PARAM_UNIT_MAX - 1);
        break;

    case CRSF_PARAM_TEXT_SELECTION:
        wb_str(&w, p->u.sel.options, CRSF_PARAM_OPTIONS_MAX - 1);
        wb_u8(&w, p->u.sel.value);
        wb_u8(&w, p->u.sel.min);
        wb_u8(&w, p->u.sel.max);
        wb_u8(&w, p->u.sel.deflt);
        wb_str(&w, p->u.sel.unit, CRSF_PARAM_UNIT_MAX - 1);
        break;

    case CRSF_PARAM_STRING:
        wb_str(&w, p->u.str.value, CRSF_PARAM_STRING_MAX - 1);
        wb_u8(&w, p->u.str.max_len);
        break;

    case CRSF_PARAM_FOLDER:
        /* Children list, terminated by 0xFF (crsf.md:826). */
        for (uint8_t i = 0; i < p->u.folder.child_count; i++) {
            wb_u8(&w, p->u.folder.children[i]);
        }
        wb_u8(&w, CRSF_PARAM_CHILDREN_END);
        break;

    case CRSF_PARAM_INFO:
        wb_str(&w, p->u.info.info, CRSF_PARAM_INFO_MAX - 1);
        break;

    case CRSF_PARAM_COMMAND:
        wb_u8(&w, p->u.cmd.status);
        wb_u8(&w, p->u.cmd.timeout);
        wb_str(&w, p->u.cmd.info, CRSF_PARAM_INFO_MAX - 1);
        break;

    default:
        /* The deprecated integer types are never emitted (crsf.md:756). */
        return 0;
    }

    return w.overflow ? 0 : w.pos;
}

size_t crsf_param_serialize_out_of_range(uint8_t *out, size_t out_size)
{
    wbuf_t w;
    wb_init(&w, out, out_size);
    wb_u8(&w, 0);                       /* parent */
    wb_u8(&w, CRSF_PARAM_OUT_OF_RANGE); /* type 127 */
    wb_str(&w, "", 0);                  /* empty name */
    return w.overflow ? 0 : w.pos;
}

bool crsf_param_parse(const uint8_t *entry, size_t len, uint8_t number,
                      crsf_param_entry_t *out)
{
    rbuf_t r;
    rb_init(&r, entry, len);

    memset(out, 0, sizeof(*out));
    out->number = number;
    out->parent = rb_u8(&r);

    const uint8_t tb = rb_u8(&r);
    out->hidden = (tb & CRSF_PARAM_HIDDEN_MASK) != 0;
    out->type = (crsf_param_type_t)(tb & CRSF_PARAM_TYPE_MASK);

    if (r.overflow) {
        return false;
    }

    /* OUT_OF_RANGE may legitimately carry nothing beyond the type. */
    if (out->type == CRSF_PARAM_OUT_OF_RANGE) {
        return true;
    }

    rb_str(&r, out->name, sizeof(out->name));
    if (r.overflow) {
        return false;
    }

    switch (out->type) {
    case CRSF_PARAM_FLOAT:
        out->u.flt.value = (int32_t)rb_be32(&r);
        out->u.flt.min = (int32_t)rb_be32(&r);
        out->u.flt.max = (int32_t)rb_be32(&r);
        out->u.flt.deflt = (int32_t)rb_be32(&r);
        out->u.flt.dec_point = rb_u8(&r);
        if (out->u.flt.dec_point > CRSF_PARAM_DEC_POINT_MAX) {
            /* Peer-supplied and unbounded by the spec; see the constant. */
            out->u.flt.dec_point = CRSF_PARAM_DEC_POINT_MAX;
        }
        out->u.flt.step = (int32_t)rb_be32(&r);
        /*
         * The numeric block is mandatory; only the trailing unit is optional.
         * Checking r.overflow *before* reading the unit matters: rb_str() sets
         * overflow and writes dst[0] = '\0' together, so testing
         * `!r.overflow || unit[0] == '\0'` afterwards was vacuous -- whenever
         * overflow was set, unit[0] was necessarily '\0' too, and the whole
         * expression collapsed to true. An entry truncated right after the name
         * parsed "successfully" with value/min/max/deflt/step all zero.
         */
        if (r.overflow) {
            return false;
        }
        rb_str(&r, out->u.flt.unit, sizeof(out->u.flt.unit));
        return true; /* trailing unit is optional, as for TEXT_SELECTION above */

    case CRSF_PARAM_TEXT_SELECTION:
        rb_str(&r, out->u.sel.options, sizeof(out->u.sel.options));
        if (r.overflow) {
            return false;
        }
        out->u.sel.value = rb_u8(&r);
        out->u.sel.min = rb_u8(&r);
        out->u.sel.max = rb_u8(&r);
        out->u.sel.deflt = rb_u8(&r);
        if (r.overflow) {
            return false;
        }
        rb_str(&r, out->u.sel.unit, sizeof(out->u.sel.unit));
        return true; /* trailing unit is optional */

    case CRSF_PARAM_STRING:
        rb_str(&r, out->u.str.value, sizeof(out->u.str.value));
        if (r.overflow) {
            return false;
        }
        out->u.str.max_len = rb_u8(&r);
        return true;

    case CRSF_PARAM_FOLDER:
        /* Read child numbers until the 0xFF terminator or the payload ends. */
        while (r.pos < r.size) {
            const uint8_t c = rb_u8(&r);
            if (c == CRSF_PARAM_CHILDREN_END) {
                break;
            }
            if (out->u.folder.child_count < CRSF_PARAM_CHILDREN_MAX) {
                out->u.folder.children[out->u.folder.child_count++] = c;
            }
        }
        return true;

    case CRSF_PARAM_INFO:
        rb_str(&r, out->u.info.info, sizeof(out->u.info.info));
        return !r.overflow;

    case CRSF_PARAM_COMMAND:
        out->u.cmd.status = rb_u8(&r);
        out->u.cmd.timeout = rb_u8(&r);
        if (r.overflow) {
            return false;
        }
        rb_str(&r, out->u.cmd.info, sizeof(out->u.cmd.info));
        return true; /* trailing info is optional */

    default:
        /*
         * Deprecated integer types (0..5) and anything unknown: the header is
         * valid, so report success and let the caller decide. crsf.md:175 asks
         * receivers to tolerate what they do not understand.
         */
        return true;
    }
}

/* ------------------------------------------------------------------------- */
/* Chunking                                                                  */
/* ------------------------------------------------------------------------- */

size_t crsf_param_build_chunk(const uint8_t *entry, size_t entry_len,
                              uint8_t number, uint8_t chunk_index,
                              uint8_t *out, size_t out_size)
{
    const uint8_t total = crsf_param_chunk_count(entry_len);
    if (chunk_index >= total) {
        return 0;
    }
    if (out_size < 2) {
        return 0;
    }

    const size_t offset = (size_t)chunk_index * CRSF_PARAM_CHUNK_MAX;
    size_t slice = entry_len - offset;
    if (slice > CRSF_PARAM_CHUNK_MAX) {
        slice = CRSF_PARAM_CHUNK_MAX;
    }
    if (2 + slice > out_size) {
        return 0;
    }

    out[0] = number;
    /* Chunks still to come after this one (crsf.md:696). */
    out[1] = (uint8_t)(total - 1u - chunk_index);
    memcpy(&out[2], &entry[offset], slice);
    return 2 + slice;
}

/* ------------------------------------------------------------------------- */
/* Provider                                                                  */
/* ------------------------------------------------------------------------- */

bool crsf_param_provider_init(crsf_param_provider_t *prov, crsf_param_t *params,
                              uint8_t count, void *ctx)
{
    if (!prov || !params || count == 0) {
        return false;
    }
    /*
     * crsf.md:709 — parameter 0 defines the root folder structure and its type
     * "always has to be 0x0B FOLDER". Catching this here beats debugging an
     * empty handset menu later.
     */
    if (params[0].type != CRSF_PARAM_FOLDER) {
        return false;
    }

    memset(prov, 0, sizeof(*prov));
    prov->params = params;
    prov->count = count;
    prov->ctx = ctx;
    return true;
}

size_t crsf_param_provider_entry(crsf_param_provider_t *prov, uint8_t number,
                                 uint8_t chunk_index, uint8_t *out, size_t out_size)
{
    size_t entry_len;

    if (number >= prov->count) {
        entry_len = crsf_param_serialize_out_of_range(prov->work, sizeof(prov->work));
    } else {
        entry_len = crsf_param_serialize(&prov->params[number], prov->work,
                                         sizeof(prov->work));
    }
    if (entry_len == 0) {
        return 0;
    }
    return crsf_param_build_chunk(prov->work, entry_len, number, chunk_index,
                                  out, out_size);
}

size_t crsf_param_provider_read(crsf_param_provider_t *prov,
                                const uint8_t *request, size_t request_len,
                                uint8_t *out, size_t out_size)
{
    /* 0x2C payload: parameter number, chunk number (crsf.md:901). */
    if (request_len < 2) {
        return 0;
    }
    return crsf_param_provider_entry(prov, request[0], request[1], out, out_size);
}

size_t crsf_param_provider_write(crsf_param_provider_t *prov,
                                 const uint8_t *payload, size_t payload_len,
                                 uint8_t *out, size_t out_size,
                                 crsf_param_reply_t *out_reply)
{
    *out_reply = CRSF_PARAM_REPLY_NONE;

    /* 0x2D payload: parameter number followed by the new value (crsf.md:910). */
    if (payload_len < 1) {
        return 0;
    }
    const uint8_t number = payload[0];
    const uint8_t *data = &payload[1];
    const size_t data_len = payload_len - 1;

    if (number >= prov->count) {
        /* Nothing to write; tell the host the number does not exist. */
        const size_t n = crsf_param_serialize_out_of_range(prov->work,
                                                           sizeof(prov->work));
        if (n == 0) {
            return 0;
        }
        *out_reply = CRSF_PARAM_REPLY_ENTRY;
        return crsf_param_build_chunk(prov->work, n, number, 0, out, out_size);
    }

    crsf_param_t *p = &prov->params[number];

    switch (p->type) {
    case CRSF_PARAM_FLOAT: {
        /* crsf.md:924 — a FLOAT write carries 4 bytes, the new int32. */
        if (data_len < 4) {
            return 0;
        }
        const int32_t previous = p->u.flt.value;
        int32_t v = (int32_t)crsf_get_be32(data);
        /* Clamp rather than reject: a host slider can overshoot by a step. */
        if (v < p->u.flt.min) {
            v = p->u.flt.min;
        }
        if (v > p->u.flt.max) {
            v = p->u.flt.max;
        }
        p->u.flt.value = v;
        if (prov->on_write && !prov->on_write(number, p, prov->ctx)) {
            p->u.flt.value = previous;
        }
        break;
    }

    case CRSF_PARAM_TEXT_SELECTION: {
        /* crsf.md:924 — a TEXT_SELECTION write carries 1 byte, the new index. */
        if (data_len < 1) {
            return 0;
        }
        const uint8_t previous = p->u.sel.value;
        uint8_t v = data[0];
        if (v < p->u.sel.min) {
            v = p->u.sel.min;
        }
        if (v > p->u.sel.max) {
            v = p->u.sel.max;
        }
        p->u.sel.value = v;
        if (prov->on_write && !prov->on_write(number, p, prov->ctx)) {
            p->u.sel.value = previous;
        }
        break;
    }

    case CRSF_PARAM_STRING: {
        if (!p->u.str.value || p->u.str.max_len == 0) {
            return 0;
        }
        /* The written value is a null-terminated string; bound it either way. */
        size_t n = 0;
        while (n < data_len && data[n] != '\0') {
            n++;
        }
        if (n > p->u.str.max_len) {
            n = p->u.str.max_len;
        }
        memcpy(p->u.str.value, data, n);
        p->u.str.value[n] = '\0';
        if (prov->on_write) {
            (void)prov->on_write(number, p, prov->ctx);
        }
        break;
    }

    case CRSF_PARAM_COMMAND: {
        if (data_len < 1) {
            return 0;
        }
        const crsf_cmd_status_t requested = (crsf_cmd_status_t)data[0];
        /*
         * Only the host-driven statuses are meaningful here (crsf.md:866).
         * POLL is explicitly a request for the current state and must not
         * disturb it (crsf.md:851).
         */
        switch (requested) {
        case CRSF_CMD_STATUS_START:
        case CRSF_CMD_STATUS_CONFIRM:
        case CRSF_CMD_STATUS_CANCEL:
        case CRSF_CMD_STATUS_POLL:
            if (prov->on_command) {
                prov->on_command(number, p, requested, prov->ctx);
            } else if (requested == CRSF_CMD_STATUS_START) {
                /* No handler: report immediate completion rather than hanging. */
                p->u.cmd.status = CRSF_CMD_STATUS_READY;
            }
            break;
        default:
            /* A device-side status written by the host is malformed; ignore. */
            break;
        }

        /*
         * crsf.md:921 — the answer to a COMMAND write is a full 0x2B entry, so
         * the host can display the new status and info text.
         */
        const size_t n = crsf_param_serialize(p, prov->work, sizeof(prov->work));
        if (n == 0) {
            return 0;
        }
        *out_reply = CRSF_PARAM_REPLY_ENTRY;
        return crsf_param_build_chunk(prov->work, n, number, 0, out, out_size);
    }

    default:
        /* FOLDER and INFO are not writable. */
        return 0;
    }

    /*
     * crsf.md:918 — value types are confirmed with a 0x2D carrying the number
     * and the accepted value.
     */
    wbuf_t w;
    wb_init(&w, out, out_size);
    wb_u8(&w, number);
    switch (p->type) {
    case CRSF_PARAM_FLOAT:
        wb_be32(&w, (uint32_t)p->u.flt.value);
        break;
    case CRSF_PARAM_TEXT_SELECTION:
        wb_u8(&w, p->u.sel.value);
        break;
    case CRSF_PARAM_STRING:
        wb_str(&w, p->u.str.value, CRSF_PARAM_STRING_MAX - 1);
        break;
    default:
        return 0;
    }
    if (w.overflow) {
        return 0;
    }
    *out_reply = CRSF_PARAM_REPLY_VALUE;
    return w.pos;
}

/* ------------------------------------------------------------------------- */
/* Client                                                                    */
/* ------------------------------------------------------------------------- */

void crsf_param_client_begin(crsf_param_client_t *cl, uint8_t dest, uint8_t total)
{
    memset(cl, 0, sizeof(*cl));
    cl->dest = dest;
    cl->total = total;
    /* crsf.md:666 — always start with chunk 0 of parameter 0. */
    cl->current = 0;
    cl->next_chunk = 0;
    cl->root_supported = true;
    cl->finished = false;
}

bool crsf_param_client_next_request(const crsf_param_client_t *cl,
                                    uint8_t *out_number, uint8_t *out_chunk)
{
    if (cl->finished) {
        return false;
    }
    *out_number = cl->current;
    *out_chunk = cl->next_chunk;
    return true;
}

/**
 * @brief Move to the next parameter, or finish when the list is exhausted.
 * @param cl Client state; @c cl->finished is set once the end is reached.
 */
static void client_advance(crsf_param_client_t *cl)
{
    cl->entry_len = 0;
    cl->next_chunk = 0;

    if (cl->total != 0 && cl->current >= cl->total) {
        cl->finished = true;
        return;
    }
    if (cl->current == 0xFF) {
        cl->finished = true;
        return;
    }
    cl->current++;

    /* With a known total, stop after the last parameter. */
    if (cl->total != 0 && cl->current > cl->total) {
        cl->finished = true;
    }
}

crsf_param_walk_t crsf_param_client_feed(crsf_param_client_t *cl,
                                         const uint8_t *payload, size_t len)
{
    if (cl->finished) {
        return CRSF_PARAM_WALK_DONE;
    }
    /* 0x2B payload: number, chunks_remaining, slice (crsf.md:694). */
    if (len < 2) {
        return CRSF_PARAM_WALK_ERROR;
    }

    const uint8_t number = payload[0];
    const uint8_t remaining = payload[1];
    const uint8_t *slice = &payload[2];
    const size_t slice_len = len - 2;

    /* A reply about a different parameter means our request was superseded. */
    if (number != cl->current) {
        return CRSF_PARAM_WALK_ERROR;
    }

    if (cl->entry_len + slice_len > sizeof(cl->entry_buf)) {
        /* Entry larger than we can hold: skip it rather than corrupt memory. */
        client_advance(cl);
        return CRSF_PARAM_WALK_ERROR;
    }
    memcpy(&cl->entry_buf[cl->entry_len], slice, slice_len);
    cl->entry_len += slice_len;

    if (remaining > 0) {
        /*
         * Bound the chunk count as well as the byte count. A peer answering
         * every request with a 2-byte payload -- number, "one more to come",
         * no data -- adds nothing to entry_len, so the size guard above never
         * fires, and the walk asks for chunk after chunk forever. The driver
         * clears its timeout and retry counter on each reply, so neither of
         * those stops it either: the walk never completes, never reports, and
         * cannot be restarted because it never goes inactive.
         *
         * An entry cannot legitimately need more chunks than its buffer can
         * hold slices of, so anything past that is a broken or hostile peer.
         */
        if (cl->next_chunk >= CRSF_PARAM_MAX_CHUNKS) {
            client_advance(cl);
            return CRSF_PARAM_WALK_ERROR;
        }
        /* More to come; ask for the following chunk of the same parameter. */
        cl->next_chunk++;
        return CRSF_PARAM_WALK_NEED_MORE;
    }

    /* Entry complete. */
    const bool ok = crsf_param_parse(cl->entry_buf, cl->entry_len, number, &cl->entry);

    if (!ok) {
        client_advance(cl);
        return CRSF_PARAM_WALK_ERROR;
    }

    /*
     * crsf.md:754 — OUT_OF_RANGE marks the end of the list. Believe it even when
     * Parameters_total suggested more.
     */
    if (cl->entry.type == CRSF_PARAM_OUT_OF_RANGE) {
        cl->finished = true;
        return CRSF_PARAM_WALK_DONE;
    }

    client_advance(cl);
    return CRSF_PARAM_WALK_ENTRY;
}

bool crsf_param_client_timeout(crsf_param_client_t *cl, bool give_up)
{
    if (cl->finished) {
        return false;
    }

    /*
     * crsf.md:713 — no answer for parameter 0 means the device's firmware has no
     * root-folder support. Fall back to walking 1..Parameters_total.
     */
    if (cl->current == 0) {
        cl->root_supported = false;
        cl->entry_len = 0;
        cl->next_chunk = 0;
        cl->current = 1;
        if (cl->total == 0) {
            /* Without a total and without a root folder there is nothing to
             * anchor the walk to. */
            cl->finished = true;
            return false;
        }
        return true;
    }

    if (!give_up) {
        /* Re-request the same chunk; 0x2C exists for exactly this (crsf.md:899). */
        return true;
    }

    client_advance(cl);
    return !cl->finished;
}
