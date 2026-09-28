/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_params.h
 * @brief The CRSF parameter/settings protocol (0x2B / 0x2C / 0x2D).
 *
 * This is what makes a device appear in a handset's configuration menu
 * (EdgeTX "Agent Lite" and equivalents). Two halves, both implemented here and
 * both free of ESP-IDF so they are host-testable:
 *
 *  - **Provider** (device side) — exposes a parameter tree, answers 0x2C reads
 *    with chunked 0x2B entries, applies 0x2D writes and runs the COMMAND
 *    lifecycle.
 *  - **Client** (host side) — walks a remote device's parameters: request chunk,
 *    reassemble, parse, repeat.
 *
 * Chunking is deliberately **stateless on the provider**. The host asks for an
 * explicit (parameter, chunk) pair (crsf.md:901), so the provider re-serialises
 * the whole entry per request and slices out the requested window. That survives
 * lost or repeated requests with no resynchronisation, which a carried chunk
 * cursor would not.
 *
 * This header deals in payloads. Putting them in frames, and pacing the requests,
 * is the transport's job — see crsf_params_attach_provider() and
 * crsf_params_walk() in esp_crsf.h for the ready-made version.
 */

#ifndef CRSF_PARAMS_H
#define CRSF_PARAMS_H

#include "crsf_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup crsf_params Parameter protocol
 * @brief Device-side provider and host-side client for 0x2B / 0x2C / 0x2D.
 * @{
 */

/**
 * @name Sizing
 * Buffer limits for the decoded (client-side) form. The provider side points at
 * caller-owned strings instead and is not bound by these.
 * @{
 */
#define CRSF_PARAM_NAME_MAX 24     /**< parameter name, including terminator */
#define CRSF_PARAM_UNIT_MAX 12     /**< unit suffix, including terminator */
#define CRSF_PARAM_OPTIONS_MAX 128 /**< semicolon-separated option list */
#define CRSF_PARAM_STRING_MAX 33   /**< 32 characters plus terminator */
#define CRSF_PARAM_INFO_MAX 48     /**< INFO / COMMAND text */
#define CRSF_PARAM_CHILDREN_MAX 16 /**< children of one FOLDER */

/**
 * Largest decimal-point position a parsed FLOAT may report.
 *
 * crsf.md:762 gives `dec_point` its meaning as a display-precision hint but
 * does not bound it, so a peer can send anything up to 255. An int32_t value
 * carries at most ten significant digits, so nothing real is lost by capping
 * it — while an uncapped value invites a consumer to compute 10^dec_point,
 * which reaches exactly 0 in 32-bit arithmetic at dec_point 32 and turns the
 * obvious `value / scale` into a division by zero.
 */
#define CRSF_PARAM_DEC_POINT_MAX 9

/**
 * Serialisation buffer for one entry. A TEXT_SELECTION with a long option list
 * is the worst case, so this must exceed CRSF_PARAM_CHUNK_MAX by a wide margin.
 */
#define CRSF_PARAM_WORK_MAX 256

/**
 * Most chunks the client will accept for one parameter entry.
 *
 * A complete entry fits CRSF_PARAM_WORK_MAX bytes, so it cannot legitimately
 * need more chunks than that many full slices. The cap exists because a peer
 * can answer with a chunk carrying *no* data while still claiming more is
 * coming, which would otherwise loop forever without ever growing the
 * accumulator. One extra chunk is allowed for the final, possibly empty one.
 */
#define CRSF_PARAM_MAX_CHUNKS \
    ((CRSF_PARAM_WORK_MAX / CRSF_PARAM_CHUNK_MAX) + 2)
/** @} */

/**
 * @brief One parameter the device exposes.
 *
 * The application owns the array and may mutate values in place; writes from the
 * host land here. String and option pointers may reference static storage,
 * except @c u.str.value which must be writable.
 *
 * Which member of @ref u applies is decided by @ref type. Only the six usable
 * types have a member — the deprecated integer types cannot be expressed here on
 * purpose, because this implementation never emits them.
 */
typedef struct {
    uint8_t parent;         /**< parameter number of the parent folder, 0 = root */
    crsf_param_type_t type; /**< selects the active member of @ref u */
    bool hidden;            /**< sent as bit 7 of the type byte (crsf.md:731) */
    const char *name;       /**< null-terminated, borrowed */

    /** Type-specific payload, selected by @ref type. */
    union {
        /** CRSF_PARAM_FLOAT — value/min/max/default are int32 (crsf.md:762). */
        struct {
            int32_t value;     /**< current value, scaled by @ref dec_point */
            int32_t min;       /**< lower bound, inclusive */
            int32_t max;       /**< upper bound, inclusive */
            int32_t deflt;     /**< value to offer as the default */
            int32_t step;      /**< recommended increment */
            uint8_t dec_point; /**< digits behind the decimal point */
            const char *unit;  /**< unit suffix, may be NULL */
        } flt;

        /** CRSF_PARAM_TEXT_SELECTION — options are semicolon-separated (crsf.md:782). */
        struct {
            const char *options; /**< e.g. "Off;On;Auto", borrowed */
            uint8_t value;       /**< selected index */
            uint8_t min;         /**< lowest selectable index */
            uint8_t max;         /**< highest selectable index */
            uint8_t deflt;       /**< default index */
            const char *unit;    /**< unit suffix, may be NULL */
        } sel;

        /** CRSF_PARAM_STRING — @c value must point at writable storage. */
        struct {
            char *value;     /**< writable, NUL-terminated buffer */
            uint8_t max_len; /**< capacity of @ref value excluding the terminator */
        } str;

        /** CRSF_PARAM_FOLDER — child parameter numbers (crsf.md:817). */
        struct {
            const uint8_t *children; /**< parameter numbers, borrowed */
            uint8_t child_count;     /**< entries in @ref children */
        } folder;

        /** CRSF_PARAM_INFO — read-only text. */
        struct {
            const char *info; /**< text to display, borrowed */
        } info;

        /** CRSF_PARAM_COMMAND — stateful action (crsf.md:843). */
        struct {
            uint8_t status;   /**< @ref crsf_cmd_status_t, device-owned */
            uint8_t timeout;  /**< units of 100 ms (crsf.md:862) */
            const char *info; /**< status text shown by the host, borrowed */
        } cmd;
    } u;
} crsf_param_t;

/**
 * @brief A 0x2B entry decoded into owned storage.
 *
 * The client-side counterpart of @ref crsf_param_t. Everything is copied into
 * fixed buffers, so an entry stays valid after the frame it came from is gone.
 * Strings longer than the buffers are truncated, never overflowed.
 */
typedef struct {
    uint8_t number;                 /**< parameter number this entry describes */
    uint8_t parent;                 /**< parameter number of the parent folder */
    crsf_param_type_t type;         /**< selects the active member of @ref u */
    bool hidden;                    /**< bit 7 of the received type byte */
    char name[CRSF_PARAM_NAME_MAX]; /**< always NUL-terminated */

    /** Type-specific payload, selected by @ref type. */
    union {
        /** CRSF_PARAM_FLOAT. */
        struct {
            int32_t value;                  /**< current value, scaled by @ref dec_point */
            int32_t min;                    /**< lower bound, inclusive */
            int32_t max;                    /**< upper bound, inclusive */
            int32_t deflt;                  /**< default value */
            int32_t step;                   /**< recommended increment */
            uint8_t dec_point;              /**< digits behind the decimal point */
            char unit[CRSF_PARAM_UNIT_MAX]; /**< unit suffix, may be empty */
        } flt;
        /** CRSF_PARAM_TEXT_SELECTION. */
        struct {
            char options[CRSF_PARAM_OPTIONS_MAX]; /**< semicolon-separated */
            uint8_t value;                        /**< selected index */
            uint8_t min;                          /**< lowest selectable index */
            uint8_t max;                          /**< highest selectable index */
            uint8_t deflt;                        /**< default index */
            char unit[CRSF_PARAM_UNIT_MAX];       /**< unit suffix, may be empty */
        } sel;
        /** CRSF_PARAM_STRING. */
        struct {
            char value[CRSF_PARAM_STRING_MAX]; /**< current text */
            uint8_t max_len;                   /**< device-declared capacity */
        } str;
        /** CRSF_PARAM_FOLDER. */
        struct {
            uint8_t children[CRSF_PARAM_CHILDREN_MAX]; /**< child parameter numbers */
            uint8_t child_count;                       /**< entries in @ref children */
        } folder;
        /** CRSF_PARAM_INFO. */
        struct {
            char info[CRSF_PARAM_INFO_MAX]; /**< text to display */
        } info;
        /** CRSF_PARAM_COMMAND. */
        struct {
            uint8_t status;                 /**< @ref crsf_cmd_status_t */
            uint8_t timeout;                /**< units of 100 ms */
            char info[CRSF_PARAM_INFO_MAX]; /**< status text */
        } cmd;
    } u;
} crsf_param_entry_t;

/**
 * @name Entry serialisation and parsing
 * @{
 */

/**
 * @brief Serialise a parameter into its 0x2B data-type payload.
 *
 * Produces everything after the parameter number and chunk counter, i.e.
 * parent, type byte, name and the type-specific fields. Chunking happens
 * afterwards, on the result.
 *
 * @param p        Parameter to serialise.
 * @param out      Destination buffer, normally CRSF_PARAM_WORK_MAX bytes.
 * @param out_size Capacity of @p out.
 * @return Bytes written, or 0 when the type is unsupported or @p out is too
 *         small. The deprecated integer types are deliberately unsupported.
 */
size_t crsf_param_serialize(const crsf_param_t *p, uint8_t *out, size_t out_size);

/**
 * @brief Serialise an OUT_OF_RANGE entry (crsf.md:752).
 *
 * Sent when the host asks for a parameter number the device does not have, and
 * doubles as the end-of-list marker. Answering rather than staying silent is
 * what lets a host stop walking.
 *
 * @param out      Destination buffer.
 * @param out_size Capacity of @p out.
 * @return Bytes written, or 0 when @p out is too small.
 */
size_t crsf_param_serialize_out_of_range(uint8_t *out, size_t out_size);

/**
 * @brief Parse a reassembled 0x2B data-type payload.
 *
 * @param entry  The concatenated chunks, without the number and chunk-counter
 *               bytes.
 * @param len    Length of @p entry.
 * @param number Parameter number, taken from the frame rather than the payload.
 * @param out    Receives the decoded entry.
 * @retval true  Parsed; @p out is populated.
 * @retval false A mandatory field is missing or runs past @p len.
 */
bool crsf_param_parse(const uint8_t *entry, size_t len, uint8_t number,
                      crsf_param_entry_t *out);

/** @} */

/**
 * @name Chunking
 * @{
 */

/**
 * @brief Number of chunks a serialised entry needs (crsf.md:666).
 *
 * @param entry_len Length of the serialised entry.
 * @return Chunk count, at least 1 — a zero-length entry still needs one chunk to
 *         carry its header bytes.
 */
static inline uint8_t crsf_param_chunk_count(size_t entry_len)
{
    if (entry_len == 0) {
        return 1;
    }
    return (uint8_t)((entry_len + CRSF_PARAM_CHUNK_MAX - 1) / CRSF_PARAM_CHUNK_MAX);
}

/**
 * @brief Build the 0x2B payload for one chunk of a serialised entry.
 *
 * Layout is `[parameter_number][chunks_remaining][slice]` (crsf.md:694), where
 * @c chunks_remaining counts the chunks still to come, so the final chunk
 * carries 0.
 *
 * @param entry       The serialised entry.
 * @param entry_len   Its length.
 * @param number      Parameter number to stamp into the payload.
 * @param chunk_index Which chunk to build, 0-based.
 * @param out         Destination for the payload.
 * @param out_size    Capacity of @p out.
 * @return Payload length, or 0 when @p chunk_index is past the end or @p out is
 *         too small.
 */
size_t crsf_param_build_chunk(const uint8_t *entry, size_t entry_len,
                              uint8_t number, uint8_t chunk_index,
                              uint8_t *out, size_t out_size);

/** @} */

/**
 * @name Provider (device side)
 * @{
 */

/** What the provider wants sent in reply. */
typedef enum {
    CRSF_PARAM_REPLY_NONE = 0, /**< send nothing */
    CRSF_PARAM_REPLY_ENTRY,    /**< send as 0x2B */
    CRSF_PARAM_REPLY_VALUE,    /**< send as 0x2D */
} crsf_param_reply_t;

/**
 * @brief Called after a value write has been applied to the parameter array.
 *
 * Lets the application react — persist to NVS, apply to hardware.
 *
 * @param number Parameter number that was written.
 * @param p      The parameter, already carrying the new value.
 * @param ctx    The context passed to crsf_param_provider_init().
 * @retval true  Accept the write.
 * @retval false Reject it: the stored value is restored and the mandatory
 *               confirmation reports the unchanged value.
 */
typedef bool (*crsf_param_write_cb_t)(uint8_t number, crsf_param_t *p, void *ctx);

/**
 * @brief Called when the host drives a COMMAND parameter (crsf.md:847).
 *
 * @param number    Parameter number.
 * @param p         The parameter; update @c p->u.cmd.status and @c p->u.cmd.info
 *                  to report progress back to the host.
 * @param requested The status the host wrote: START, CONFIRM, CANCEL or POLL.
 *                  POLL must not change state — it only asks.
 * @param ctx       The context passed to crsf_param_provider_init().
 */
typedef void (*crsf_param_cmd_cb_t)(uint8_t number, crsf_param_t *p,
                                    crsf_cmd_status_t requested, void *ctx);

/**
 * @brief Provider instance.
 *
 * Initialise with crsf_param_provider_init(). The callbacks are plain fields:
 * assign them after init and before the port starts serving requests.
 */
typedef struct {
    crsf_param_t *params; /**< index i is parameter number i; borrowed */
    uint8_t count;        /**< number of entries, including parameter 0 */

    crsf_param_write_cb_t on_write; /**< optional, see @ref crsf_param_write_cb_t */
    crsf_param_cmd_cb_t on_command; /**< optional, see @ref crsf_param_cmd_cb_t */
    void *ctx;                      /**< passed to both callbacks */

    uint8_t work[CRSF_PARAM_WORK_MAX]; /**< re-serialisation scratch, private */
} crsf_param_provider_t;

/**
 * @brief Initialise a provider.
 *
 * crsf.md:709 requires parameter 0 to be a FOLDER describing the root; this is
 * checked and reported so the mistake surfaces at startup rather than as a
 * confusing handset menu.
 *
 * @param prov   Provider to initialise.
 * @param params Parameter array; borrowed, must outlive @p prov.
 * @param count  Number of entries in @p params.
 * @param ctx    Opaque pointer handed to both callbacks.
 * @retval true  Initialised.
 * @retval false @p params is NULL, @p count is 0, or parameter 0 is not a
 *               FOLDER.
 */
bool crsf_param_provider_init(crsf_param_provider_t *prov, crsf_param_t *params,
                              uint8_t count, void *ctx);

/**
 * @brief Answer a 0x2C read request (crsf.md:897).
 *
 * An out-of-range parameter number yields an OUT_OF_RANGE entry rather than
 * silence, which is what tells a host it has reached the end of the list.
 *
 * @param prov        Provider.
 * @param request     The 0x2C payload: parameter number, chunk number.
 * @param request_len Its length.
 * @param out         Receives a 0x2B payload.
 * @param out_size    Capacity of @p out.
 * @return Payload length, or 0 when the request is malformed or @p out is too
 *         small.
 */
size_t crsf_param_provider_read(crsf_param_provider_t *prov,
                                const uint8_t *request, size_t request_len,
                                uint8_t *out, size_t out_size);

/**
 * @brief Apply a 0x2D write and build the mandatory confirmation (crsf.md:906).
 *
 * Answering is a MUST, so this always produces a reply for a well-formed write.
 * The reply *type* depends on the parameter type (crsf.md:917): value types echo
 * back a 0x2D, while a COMMAND returns a full 0x2B entry so the host can show
 * the updated status.
 *
 * @param prov        Provider.
 * @param payload     The 0x2D payload: parameter number followed by the value.
 * @param payload_len Its length.
 * @param out         Receives the reply payload.
 * @param out_size    Capacity of @p out.
 * @param out_reply   Receives which frame type to send @p out as.
 * @return Payload length, or 0 when nothing should be sent — in which case
 *         @p out_reply is CRSF_PARAM_REPLY_NONE.
 */
size_t crsf_param_provider_write(crsf_param_provider_t *prov,
                                 const uint8_t *payload, size_t payload_len,
                                 uint8_t *out, size_t out_size,
                                 crsf_param_reply_t *out_reply);

/**
 * @brief Build an unsolicited 0x2B entry, e.g. after a command finishes.
 *
 * crsf.md:889 notes the host must POLL to see progress, but a device that
 * pushes an update anyway is friendlier and still compliant.
 *
 * @param prov        Provider.
 * @param number      Parameter number to report.
 * @param chunk_index Which chunk to build, 0-based.
 * @param out         Receives a 0x2B payload.
 * @param out_size    Capacity of @p out.
 * @return Payload length, or 0 for an unknown number, a chunk index past the
 *         end, or an @p out that is too small.
 */
size_t crsf_param_provider_entry(crsf_param_provider_t *prov, uint8_t number,
                                 uint8_t chunk_index, uint8_t *out, size_t out_size);

/** @} */

/**
 * @name Client (host side)
 * @{
 */

/** Outcome of feeding one 0x2B payload into a walk. */
typedef enum {
    CRSF_PARAM_WALK_NEED_MORE = 0, /**< chunk accepted, entry incomplete */
    CRSF_PARAM_WALK_ENTRY,         /**< an entry is ready in the client */
    CRSF_PARAM_WALK_DONE,          /**< the whole list has been walked */
    CRSF_PARAM_WALK_ERROR,         /**< malformed reply; entry skipped */
} crsf_param_walk_t;

/**
 * @brief Client-side walk state.
 *
 * Initialise with crsf_param_client_begin(). Treat the fields as private except
 * @ref entry, which is the result after CRSF_PARAM_WALK_ENTRY.
 */
typedef struct {
    uint8_t dest;        /**< device being walked */
    uint8_t total;       /**< Parameters_total from 0x29, 0 if unknown */
    uint8_t current;     /**< parameter number being fetched */
    uint8_t next_chunk;  /**< chunk index to request next */
    bool root_supported; /**< false once parameter 0 proved unsupported */
    bool finished;       /**< the walk has ended */

    uint8_t entry_buf[CRSF_PARAM_WORK_MAX]; /**< chunk accumulator, private */
    size_t entry_len;                       /**< bytes accumulated so far */

    crsf_param_entry_t entry; /**< valid after CRSF_PARAM_WALK_ENTRY */
} crsf_param_client_t;

/**
 * @brief Start walking a device's parameters.
 *
 * @param cl    Client state to initialise.
 * @param dest  Address of the device to walk.
 * @param total `Parameters_total` from the device's 0x29 reply. 0 means unknown,
 *              in which case the walk relies on OUT_OF_RANGE to find the end.
 */
void crsf_param_client_begin(crsf_param_client_t *cl, uint8_t dest, uint8_t total);

/**
 * @brief What to request next.
 *
 * Encode the result as a 0x2C payload and send it to @c cl->dest.
 *
 * @param cl         Client state.
 * @param out_number Receives the parameter number to request.
 * @param out_chunk  Receives the chunk index to request.
 * @retval true  A request is outstanding; both outputs are set.
 * @retval false The walk is finished; nothing more to request.
 */
bool crsf_param_client_next_request(const crsf_param_client_t *cl,
                                    uint8_t *out_number, uint8_t *out_chunk);

/**
 * @brief Feed a received 0x2B payload.
 *
 * Concatenates chunks until @c chunks_remaining reaches 0, then parses the entry
 * and reports CRSF_PARAM_WALK_ENTRY. An OUT_OF_RANGE entry ends the walk
 * (crsf.md:754).
 *
 * @param cl      Client state.
 * @param payload The 0x2B payload, parameter number first.
 * @param len     Payload length.
 * @return What happened, see @ref crsf_param_walk_t. On
 *         CRSF_PARAM_WALK_ENTRY read the result from @c cl->entry.
 */
crsf_param_walk_t crsf_param_client_feed(crsf_param_client_t *cl,
                                         const uint8_t *payload, size_t len);

/**
 * @brief Report that the outstanding request timed out.
 *
 * crsf.md:713 makes this meaningful for parameter 0: a device that does not
 * answer it has no root folder, so the walk restarts at parameter 1 and relies
 * on `Parameters_total`. For any other parameter the chunk is simply
 * re-requested until @p give_up, after which the parameter is skipped.
 *
 * @param cl      Client state.
 * @param give_up true once the retry budget for this chunk is exhausted.
 * @retval true  The walk continues; ask crsf_param_client_next_request() again.
 * @retval false The walk has finished.
 */
bool crsf_param_client_timeout(crsf_param_client_t *cl, bool give_up);

/** @} */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CRSF_PARAMS_H */
