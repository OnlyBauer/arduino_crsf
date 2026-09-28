/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_core.c
 * @brief The protocol layer: scheduler, parameters, tunnels, commands, routing.
 *
 * No platform here. Everything this file needs from the machine underneath it
 * arrives through crsf_io_ops_t -- a frame sink, a clock, a lock, a baudrate
 * setter, a receive flush and an event hook -- so the same object code serves
 * an ESP32, an STM32 and an Arduino.
 *
 * @par Locking
 * One lock per port, taken through lock()/unlock(), guards the shared snapshots
 * and tables. It is never held across a transmit and never across an
 * application callback: the lock is not recursive, and a callback that calls
 * back into the API would deadlock its own caller permanently. Where a callback
 * has to run, the state it needs is copied out first and the lock released.
 */

#include "crsf_core.h"
#include "crsf_crc.h"

#include <string.h>
#include <stddef.h>

/** How long a new baudrate must carry traffic before it is considered good. */
#define CRSF_BAUD_CONFIRM_MS 500

/** Pacing between successive 0x2C parameter requests. */
#define CRSF_PARAM_REQUEST_MS 100

/** How long to wait for a 0x2B before retrying the request. */
#define CRSF_PARAM_TIMEOUT_MS 500

/** Retries per parameter chunk before the parameter is skipped. */
#define CRSF_PARAM_MAX_RETRIES 3

/** Failsafe hold when crsf_port_config_t::link_timeout_ms is 0 (crsf.md:519). */
#define CRSF_DEFAULT_LINK_TIMEOUT_MS 1000

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

/**
 * @brief Monotonic microsecond clock.
 *
 * Reached through the port hooks rather than called directly, so the code above
 * this line has no idea what a timer is.
 *
 * @param h Port handle.
 * @return Microseconds since boot.
 */
static inline int64_t now_us(crsf_handle_t h)
{
    return h->ops->now_us(h->io_ctx);
}

/**
 * @brief Age of a timestamp in milliseconds.
 *
 * @param h        Port handle.
 * @param stamp_us A timestamp from now_us(), or 0 for "never happened".
 * @return Milliseconds elapsed, or UINT32_MAX when @p stamp_us is 0. The
 *         sentinel is what lets callers tell "no channel frame ever arrived"
 *         apart from "one arrived just now".
 */
static inline uint32_t age_ms_from(crsf_handle_t h, int64_t stamp_us)
{
    if (stamp_us == 0) {
        return UINT32_MAX;
    }
    return (uint32_t)((now_us(h) - stamp_us) / 1000);
}

/**
 * @brief Take the port mutex.
 *
 * Waits indefinitely, which is safe because the mutex is never held across a
 * UART call, a queue send or an application callback.
 *
 * @param h Port handle; must be non-NULL.
 */
static inline void lock(crsf_handle_t h)
{
    if (h->ops->lock) {
        h->ops->lock(h->io_ctx);
    }
}

/**
 * @brief Release the port mutex.
 * @param h Port handle; must be non-NULL.
 */
static inline void unlock(crsf_handle_t h)
{
    if (h->ops->unlock) {
        h->ops->unlock(h->io_ctx);
    }
}

/**
 * @brief Take the router lock, if it has one.
 * @param r Router instance.
 */
static inline void router_lock(crsf_router_t *r)
{
    if (r->lock) {
        r->lock(r->lock_ctx);
    }
}

/**
 * @brief Release the router lock.
 * @param r Router instance.
 */
static inline void router_unlock(crsf_router_t *r)
{
    if (r->unlock) {
        r->unlock(r->lock_ctx);
    }
}

/**
 * @brief Report an event, if the port cares about them.
 * @param h  Port handle.
 * @param ev What happened.
 * @param a  First argument, per crsf_event_t.
 * @param b  Second argument, per crsf_event_t.
 */
static inline void event(crsf_handle_t h, crsf_event_t ev, uint32_t a, uint32_t b)
{
    if (h->ops->event) {
        h->ops->event(h->io_ctx, ev, a, b);
    }
}

/* ------------------------------------------------------------------------- */
/* transmit path                                                             */
/* ------------------------------------------------------------------------- */

/**
 * @brief Hand a fully built frame to the TX task.
 *
 * The single funnel for everything that goes on the wire, which is what makes
 * the API safe to call from several tasks without a transmit lock.
 *
 * @param h     Port handle.
 * @param frame Complete frame including sync and CRC; copied into the queue.
 * @param len   Frame length.
 * @retval CRSF_OK                Queued.
 * @retval CRSF_ERR_INVALID_STATE @p h is NULL or the port is shutting down.
 * @retval CRSF_ERR_INVALID_SIZE  @p len is 0 or above CRSF_MAX_FRAME_SIZE.
 * @retval CRSF_ERR_TIMEOUT       The queue stayed full for 20 ms.
 */
static crsf_err_t tx_enqueue(crsf_handle_t h, const uint8_t *frame, size_t len)
{
    if (!h || !h->running) {
        return CRSF_ERR_INVALID_STATE;
    }
    if (len == 0 || len > CRSF_MAX_FRAME_SIZE) {
        return CRSF_ERR_INVALID_SIZE;
    }

    if (!h->ops->tx_push(h->io_ctx, frame, len)) {
        return CRSF_ERR_TIMEOUT;
    }
    return CRSF_OK;
}

crsf_err_t crsf_send_frame(crsf_handle_t h, uint8_t type, uint8_t destination,
                          const void *payload, size_t payload_len)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    uint8_t frame[CRSF_MAX_FRAME_SIZE];
    const size_t n = crsf_build_frame(frame, CRSF_SYNC_BYTE, type,
                                      destination, h->self_address,
                                      (const uint8_t *)payload, payload_len);
    if (n == 0) {
        return CRSF_ERR_INVALID_SIZE;
    }
    return tx_enqueue(h, frame, n);
}

/**
 * @brief Queue a short-header frame addressed to everyone.
 *
 * Telemetry is broadcast: it concerns every node on the bus, and the short
 * header saves the two address bytes.
 *
 * @param h       Port handle.
 * @param type    Frame type.
 * @param payload Encoded payload.
 * @param len     Payload length.
 * @return As crsf_send_frame().
 */
static crsf_err_t send_broadcast(crsf_handle_t h, uint8_t type,
                                const uint8_t *payload, size_t len)
{
    return crsf_send_frame(h, type, CRSF_ADDR_BROADCAST, payload, len);
}

crsf_err_t crsf_send_command(crsf_handle_t h, uint8_t destination,
                            uint8_t command_id, uint8_t sub_id,
                            const void *args, size_t args_len)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    uint8_t frame[CRSF_MAX_FRAME_SIZE];
    const size_t n = crsf_build_command(frame, destination, h->self_address,
                                        command_id, sub_id,
                                        (const uint8_t *)args, args_len);
    if (n == 0) {
        return CRSF_ERR_INVALID_SIZE;
    }
    return tx_enqueue(h, frame, n);
}

crsf_err_t crsf_send_command_ack(crsf_handle_t h, uint8_t destination,
                                uint8_t command_id, uint8_t sub_id,
                                bool acted, const char *info)
{
    /* Payload of 0x32.0xFF: acked command, sub id, action flag, optional string
     * (crsf.md:968). */
    uint8_t args[CRSF_MAX_EXT_PAYLOAD_SIZE];
    size_t n = 0;
    args[n++] = command_id;
    args[n++] = sub_id;
    args[n++] = acted ? 1 : 0;

    if (info) {
        /*
         * Budget against what a 0x32 frame can actually carry, not against
         * sizeof(args). crsf_build_command() forms a body of 7 + args_len and
         * rejects anything over CRSF_FRAME_LEN_MAX, so args_len must stay
         * within CRSF_FRAME_LEN_MAX - 7 = 55 -- three bytes less than this
         * 58-byte buffer. Sizing against the buffer let a long info string push
         * args_len to 58, whereupon crsf_build_command() returned 0 and no ACK
         * was sent at all, silently skipping the acknowledgement crsf.md:966
         * requires.
         */
        const size_t args_cap = CRSF_FRAME_LEN_MAX - 7u;
        const size_t max = (args_cap < sizeof(args) ? args_cap : sizeof(args)) - n - 1;
        size_t l = 0;
        while (l < max && info[l] != '\0') {
            args[n + l] = (uint8_t)info[l];
            l++;
        }
        n += l;
    }
    args[n++] = '\0';

    return crsf_send_command(h, destination, CRSF_CMD_ACK, 0xFF, args, n);
}

/* --- typed telemetry senders and publishers ------------------------------ */

/**
 * @brief Define the send-now and publish-to-scheduler pair for one frame type.
 *
 * Both halves are the same four steps — reject NULL, encode into a local buffer,
 * refuse a zero-length result, hand the bytes on — and differ only in where the
 * payload goes: straight onto the wire, or into the scheduler slot for @p type_id.
 *
 * They are generated from one line per frame type rather than written out, and
 * from the *same* line rather than two parallel lists. A separate publisher list
 * would drift: sooner or later a frame type gains a sender and never gets its
 * publisher. This way it cannot happen.
 *
 * Both names are spelled out instead of pasted together from a stem, because
 * a `crsf_send_gps` assembled with `##` would no longer be greppable.
 *
 * @param send_fn    Name of the immediate sender to define.
 * @param publish_fn Name of the scheduler publisher to define.
 * @param type_id    Frame type.
 * @param ctype      Payload struct type.
 * @param encoder    Encoder from crsf_codec.h for @p ctype.
 */
#define DEFINE_TELEMETRY(send_fn, publish_fn, type_id, ctype, encoder) \
    crsf_err_t send_fn(crsf_handle_t h, const ctype *in)                \
    {                                                                  \
        if (!h || !in) {                                               \
            return CRSF_ERR_INVALID_ARG;                                \
        }                                                              \
        uint8_t pl[CRSF_MAX_PAYLOAD_SIZE];                             \
        const size_t n = encoder(pl, in);                              \
        if (n == 0) {                                                  \
            return CRSF_ERR_INVALID_ARG;                                \
        }                                                              \
        return send_broadcast(h, (type_id), pl, n);                    \
    }                                                                  \
                                                                       \
    crsf_err_t publish_fn(crsf_handle_t h, const ctype *in)             \
    {                                                                  \
        if (!h || !in) {                                               \
            return CRSF_ERR_INVALID_ARG;                                \
        }                                                              \
        uint8_t pl[CRSF_MAX_PAYLOAD_SIZE];                             \
        const size_t n = encoder(pl, in);                              \
        if (n == 0) {                                                  \
            return CRSF_ERR_INVALID_ARG;                                \
        }                                                              \
        return crsf_telemetry_publish(h, (type_id), pl, n);            \
    }

DEFINE_TELEMETRY(crsf_send_gps, crsf_publish_gps, CRSF_TYPE_GPS, crsf_gps_t, crsf_encode_gps)
DEFINE_TELEMETRY(crsf_send_gps_time, crsf_publish_gps_time, CRSF_TYPE_GPS_TIME, crsf_gps_time_t, crsf_encode_gps_time)
DEFINE_TELEMETRY(crsf_send_gps_extended, crsf_publish_gps_extended, CRSF_TYPE_GPS_EXTENDED, crsf_gps_extended_t, crsf_encode_gps_extended)
DEFINE_TELEMETRY(crsf_send_vario, crsf_publish_vario, CRSF_TYPE_VARIO, crsf_vario_t, crsf_encode_vario)
DEFINE_TELEMETRY(crsf_send_battery, crsf_publish_battery, CRSF_TYPE_BATTERY, crsf_battery_t, crsf_encode_battery)
DEFINE_TELEMETRY(crsf_send_baro_altitude, crsf_publish_baro_altitude, CRSF_TYPE_BARO_ALTITUDE, crsf_baro_altitude_t, crsf_encode_baro_altitude)
DEFINE_TELEMETRY(crsf_send_airspeed, crsf_publish_airspeed, CRSF_TYPE_AIRSPEED, crsf_airspeed_t, crsf_encode_airspeed)
DEFINE_TELEMETRY(crsf_send_rpm, crsf_publish_rpm, CRSF_TYPE_RPM, crsf_rpm_t, crsf_encode_rpm)
DEFINE_TELEMETRY(crsf_send_temp, crsf_publish_temp, CRSF_TYPE_TEMP, crsf_temp_t, crsf_encode_temp)
DEFINE_TELEMETRY(crsf_send_voltages, crsf_publish_voltages, CRSF_TYPE_VOLTAGES, crsf_voltages_t, crsf_encode_voltages)
DEFINE_TELEMETRY(crsf_send_vtx_telemetry, crsf_publish_vtx_telemetry, CRSF_TYPE_VTX_TELEMETRY, crsf_vtx_telemetry_t, crsf_encode_vtx_telemetry)
DEFINE_TELEMETRY(crsf_send_barometer, crsf_publish_barometer, CRSF_TYPE_BAROMETER, crsf_barometer_t, crsf_encode_barometer)
DEFINE_TELEMETRY(crsf_send_magnetometer, crsf_publish_magnetometer, CRSF_TYPE_MAGNETOMETER, crsf_magnetometer_t, crsf_encode_magnetometer)
DEFINE_TELEMETRY(crsf_send_accel_gyro, crsf_publish_accel_gyro, CRSF_TYPE_ACCEL_GYRO, crsf_accel_gyro_t, crsf_encode_accel_gyro)
DEFINE_TELEMETRY(crsf_send_attitude, crsf_publish_attitude, CRSF_TYPE_ATTITUDE, crsf_attitude_t, crsf_encode_attitude)
DEFINE_TELEMETRY(crsf_send_mavlink_fc, crsf_publish_mavlink_fc, CRSF_TYPE_MAVLINK_FC, crsf_mavlink_fc_t, crsf_encode_mavlink_fc)
DEFINE_TELEMETRY(crsf_send_mavlink_status, crsf_publish_mavlink_status, CRSF_TYPE_MAVLINK_SYS_STATUS, crsf_mavlink_status_t, crsf_encode_mavlink_status)
DEFINE_TELEMETRY(crsf_send_esp_now, crsf_publish_esp_now, CRSF_TYPE_ESP_NOW, crsf_esp_now_t, crsf_encode_esp_now)
DEFINE_TELEMETRY(crsf_send_logging, crsf_publish_logging, CRSF_TYPE_LOGGING, crsf_logging_t, crsf_encode_logging)
DEFINE_TELEMETRY(crsf_send_game, crsf_publish_game, CRSF_TYPE_GAME, crsf_game_t, crsf_encode_game)

DEFINE_TELEMETRY(crsf_send_link_statistics, crsf_publish_link_statistics, CRSF_TYPE_LINK_STATISTICS, crsf_link_statistics_t, crsf_encode_link_statistics)
DEFINE_TELEMETRY(crsf_send_link_statistics_rx, crsf_publish_link_statistics_rx, CRSF_TYPE_LINK_STATISTICS_RX, crsf_link_statistics_rx_t, crsf_encode_link_statistics_rx)
DEFINE_TELEMETRY(crsf_send_link_statistics_tx, crsf_publish_link_statistics_tx, CRSF_TYPE_LINK_STATISTICS_TX, crsf_link_statistics_tx_t, crsf_encode_link_statistics_tx)

#undef DEFINE_TELEMETRY

/*
 * The remaining two broadcast frames do not fit the macro: 0x0B carries no
 * caller-supplied payload at all, and 0x21 takes a string rather than a struct.
 * Their send/publish pairs are written out, and share a helper so the encoding
 * still exists in one place each.
 */

/**
 * @brief Encode our own 0x0B Heartbeat payload.
 * @param h  Port handle, read for its address.
 * @param pl Destination, at least CRSF_PAYLOAD_SIZE_HEARTBEAT bytes.
 * @return Payload length.
 */
static size_t encode_own_heartbeat(crsf_handle_t h, uint8_t *pl)
{
    const crsf_heartbeat_t hb = {.origin_address = (int16_t)h->self_address};
    return crsf_encode_heartbeat(pl, &hb);
}

/**
 * @brief Encode a 0x21 Flight Mode payload from a C string.
 * @param mode Null-terminated name, truncated to the field width.
 * @param pl   Destination, at least CRSF_FLIGHT_MODE_MAX_LEN bytes.
 * @return Payload length, terminator included.
 */
static size_t encode_flight_mode_str(const char *mode, uint8_t *pl)
{
    crsf_flight_mode_t fm = {0};
    size_t i = 0;
    while (i < CRSF_FLIGHT_MODE_MAX_LEN - 1 && mode[i] != '\0') {
        fm.flight_mode[i] = mode[i];
        i++;
    }
    fm.flight_mode[i] = '\0';
    return crsf_encode_flight_mode(pl, &fm);
}

crsf_err_t crsf_send_heartbeat(crsf_handle_t h)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    uint8_t pl[CRSF_PAYLOAD_SIZE_HEARTBEAT];
    const size_t n = encode_own_heartbeat(h, pl);
    if (n == 0) {
        return CRSF_ERR_INVALID_ARG;
    }
    return send_broadcast(h, CRSF_TYPE_HEARTBEAT, pl, n);
}

crsf_err_t crsf_publish_heartbeat(crsf_handle_t h)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    uint8_t pl[CRSF_PAYLOAD_SIZE_HEARTBEAT];
    return crsf_telemetry_publish(h, CRSF_TYPE_HEARTBEAT, pl,
                                  encode_own_heartbeat(h, pl));
}

crsf_err_t crsf_send_flight_mode(crsf_handle_t h, const char *mode)
{
    if (!h || !mode) {
        return CRSF_ERR_INVALID_ARG;
    }
    uint8_t pl[CRSF_FLIGHT_MODE_MAX_LEN];
    const size_t n = encode_flight_mode_str(mode, pl);
    if (n == 0) {
        return CRSF_ERR_INVALID_ARG;
    }
    return send_broadcast(h, CRSF_TYPE_FLIGHT_MODE, pl, n);
}

crsf_err_t crsf_publish_flight_mode(crsf_handle_t h, const char *mode)
{
    if (!h || !mode) {
        return CRSF_ERR_INVALID_ARG;
    }
    uint8_t pl[CRSF_FLIGHT_MODE_MAX_LEN];
    return crsf_telemetry_publish(h, CRSF_TYPE_FLIGHT_MODE, pl,
                                  encode_flight_mode_str(mode, pl));
}

crsf_err_t crsf_send_channels(crsf_handle_t h, const crsf_channels_t *channels)
{
    if (!h || !channels) {
        return CRSF_ERR_INVALID_ARG;
    }
    /*
     * Only the sending station drives the channel stream; letting a receiving
     * station emit 0x16 would put two masters on the same wire.
     */
    if (h->role != CRSF_ROLE_TX) {
        return CRSF_ERR_INVALID_STATE;
    }
    uint8_t pl[CRSF_CHANNELS_PAYLOAD_SIZE];
    const size_t n = crsf_encode_channels(pl, channels);
    if (n == 0) {
        return CRSF_ERR_INVALID_ARG;
    }
    return send_broadcast(h, CRSF_TYPE_RC_CHANNELS_PACKED, pl, n);
}

crsf_err_t crsf_send_subset_channels(crsf_handle_t h, const crsf_subset_channels_t *ch)
{
    if (!h || !ch) {
        return CRSF_ERR_INVALID_ARG;
    }
    if (h->role != CRSF_ROLE_TX) {
        return CRSF_ERR_INVALID_STATE;
    }
    uint8_t pl[CRSF_MAX_PAYLOAD_SIZE];
    const size_t n = crsf_encode_subset_channels(pl, ch);
    if (n == 0) {
        return CRSF_ERR_INVALID_ARG;
    }
    return send_broadcast(h, CRSF_TYPE_SUBSET_RC_CHANNELS, pl, n);
}

crsf_err_t crsf_send_ping(crsf_handle_t h, uint8_t destination)
{
    return crsf_send_frame(h, CRSF_TYPE_DEVICE_PING, destination, NULL, 0);
}

crsf_err_t crsf_send_device_info(crsf_handle_t h, uint8_t destination)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    uint8_t pl[CRSF_MAX_EXT_PAYLOAD_SIZE];
    lock(h);
    /* Report the attached provider's parameter count, if any. */
    h->device_info.parameters_total = h->provider ? h->provider->count : 0;
    const size_t n = crsf_encode_device_info(pl, &h->device_info);
    unlock(h);
    if (n == 0) {
        return CRSF_ERR_INVALID_ARG;
    }
    return crsf_send_frame(h, CRSF_TYPE_DEVICE_INFO, destination, pl, n);
}

crsf_err_t crsf_send_timing_correction(crsf_handle_t h, uint8_t destination,
                                      const crsf_timing_correction_t *t)
{
    if (!h || !t) {
        return CRSF_ERR_INVALID_ARG;
    }
    uint8_t pl[CRSF_PAYLOAD_SIZE_TIMING_CORR];
    return crsf_send_frame(h, CRSF_TYPE_REMOTE, destination, pl,
                           crsf_encode_timing_correction(pl, t));
}

/* ------------------------------------------------------------------------- */
/* telemetry scheduler                                                       */
/* ------------------------------------------------------------------------- */

/**
 * @brief Find an existing scheduler slot for @p type, or claim a free one.
 *
 * @param h      Port handle.
 * @param type   Frame type to look up.
 * @param create true to claim a free slot when @p type has none yet.
 * @return The slot, or NULL when @p type is absent and either @p create is false
 *         or every slot is taken.
 *
 * @warning Call with the port mutex held; the returned pointer is only valid
 *          while it stays held.
 */
static crsf_sched_slot_t *slot_for(crsf_handle_t h, uint8_t type, bool create)
{
    for (int i = 0; i < CRSF_SCHED_MAX_SLOTS; i++) {
        if (h->slots[i].type == type) {
            return &h->slots[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < CRSF_SCHED_MAX_SLOTS; i++) {
        if (h->slots[i].type == 0) {
            memset(&h->slots[i], 0, sizeof(h->slots[i]));
            h->slots[i].type = type;
            return &h->slots[i];
        }
    }
    return NULL;
}

crsf_err_t crsf_telemetry_publish(crsf_handle_t h, uint8_t type,
                                 const void *payload, size_t len)
{
    /*
     * A NULL payload would reach memcpy(); reject it here rather than letting it
     * become undefined behaviour.
     *
     * A zero length is rejected too, which the typed publishers above rely on.
     * Every crsf_encode_* returns 0 to mean "could not represent this", and the
     * old call pattern fed that straight in as the length — after which the
     * scheduler emitted an empty frame on every interval, silently and forever.
     * A repeating empty frame is never what anyone wanted; a single one is still
     * available through crsf_send_frame().
     */
    if (!h || type == 0 || !payload || len == 0 || len > CRSF_MAX_PAYLOAD_SIZE) {
        return CRSF_ERR_INVALID_ARG;
    }

    lock(h);
    crsf_sched_slot_t *slot = slot_for(h, type, true);
    if (!slot) {
        unlock(h);
        return CRSF_ERR_NO_MEM;
    }
    memcpy(slot->payload, payload, len);
    slot->len = (uint8_t)len;
    slot->valid = true;
    unlock(h);
    return CRSF_OK;
}

crsf_err_t crsf_telemetry_set_interval(crsf_handle_t h, uint8_t type,
                                      uint32_t interval_ms)
{
    if (!h || type == 0) {
        return CRSF_ERR_INVALID_ARG;
    }
    lock(h);
    crsf_sched_slot_t *slot = slot_for(h, type, interval_ms > 0);
    if (!slot) {
        unlock(h);
        return (interval_ms == 0) ? CRSF_OK : CRSF_ERR_NO_MEM;
    }
    if (interval_ms == 0) {
        /*
         * Release the slot, as the header promises. Leaving type set kept it
         * matched by slot_for() forever, so it was never reclaimed -- and
         * because handle_command() drives this from a peer's 0x32 Flow Control
         * with a wire-supplied frame type, a peer could subscribe then
         * unsubscribe CRSF_SCHED_MAX_SLOTS distinct types and permanently
         * exhaust the scheduler, after which every publish for a new type
         * failed with CRSF_ERR_NO_MEM for the life of the port.
         */
        memset(slot, 0, sizeof(*slot));
    } else {
        slot->interval_ms = interval_ms;
    }
    unlock(h);
    return CRSF_OK;
}

uint32_t crsf_link_byte_budget(crsf_handle_t h)
{
    /* 8N1 costs 10 bit-times per byte. */
    return h ? h->baudrate / 10 : 0;
}

/**
 * @brief Send every scheduled telemetry payload whose interval has elapsed.
 *
 * Called once per TX task iteration. The mutex is taken and released per slot,
 * and the payload is copied out before it is sent, so a publisher on another
 * task never blocks behind a UART write.
 *
 * @param h Port handle.
 *
 * @note Resolution is bounded by CRSF_TX_WAIT_TICKS, i.e. by the FreeRTOS tick
 *       period — see the note on crsf_telemetry_set_interval().
 */
static void scheduler_tick(crsf_handle_t h)
{
    const int64_t t = now_us(h);

    for (int i = 0; i < CRSF_SCHED_MAX_SLOTS; i++) {
        uint8_t type, len;
        uint8_t payload[CRSF_MAX_PAYLOAD_SIZE];

        lock(h);
        crsf_sched_slot_t *slot = &h->slots[i];
        if (slot->type == 0 || !slot->valid || slot->interval_ms == 0) {
            unlock(h);
            continue;
        }
        const int64_t due = slot->last_sent_us + (int64_t)slot->interval_ms * 1000;
        if (slot->last_sent_us != 0 && t < due) {
            unlock(h);
            continue;
        }
        type = slot->type;
        len = slot->len;
        memcpy(payload, slot->payload, len);
        slot->last_sent_us = t;
        unlock(h);

        (void)send_broadcast(h, type, payload, len);
    }
}

/* ------------------------------------------------------------------------- */
/* tunnels                                                                   */
/* ------------------------------------------------------------------------- */

/**
 * @brief Send one 0xAA MAVLink envelope chunk.
 *
 * The 0xAA header form is where the spec contradicts itself (crsf.md:647 versus
 * the diagram at crsf.md:1316), so the frame is built by hand here rather than
 * through crsf_send_frame(): that lets
 * crsf_config_t::mavlink_envelope_extended_header pick the form per port instead
 * of it being fixed by crsf_type_has_ext_header().
 *
 * @param h           Port handle.
 * @param destination Target address; only used with the extended header.
 * @param chunk       The chunk to send.
 * @return As crsf_send_frame().
 */
static crsf_err_t send_mavlink_chunk(crsf_handle_t h, uint8_t destination,
                                    const crsf_mavlink_envelope_t *chunk)
{
    uint8_t pl[CRSF_MAX_PAYLOAD_SIZE];
    const size_t n = crsf_encode_mavlink_envelope(pl, chunk);
    if (n == 0) {
        return CRSF_ERR_INVALID_ARG;
    }

    if (h->mavlink_envelope_extended_header) {
        /* Hand-build so the extended header is present even though
         * crsf_type_has_ext_header() reports 0xAA as short-header. */
        uint8_t frame[CRSF_MAX_FRAME_SIZE];
        const size_t body_len = 1 + 2 + n + 1;
        if (body_len > CRSF_FRAME_LEN_MAX) {
            return CRSF_ERR_INVALID_SIZE;
        }
        size_t i = 0;
        frame[i++] = CRSF_SYNC_BYTE;
        frame[i++] = (uint8_t)body_len;
        const size_t body = i;
        frame[i++] = CRSF_TYPE_MAVLINK_ENVELOPE;
        frame[i++] = destination;
        frame[i++] = h->self_address;
        memcpy(&frame[i], pl, n);
        i += n;
        frame[i] = crsf_crc8(&frame[body], i - body);
        i++;
        return tx_enqueue(h, frame, i);
    }

    return crsf_send_frame(h, CRSF_TYPE_MAVLINK_ENVELOPE, destination, pl, n);
}

crsf_err_t crsf_mavlink_send(crsf_handle_t h, uint8_t destination,
                            const uint8_t *frame, size_t len)
{
    if (!h || !frame || len == 0) {
        return CRSF_ERR_INVALID_ARG;
    }
    /*
     * Refuse what the far end cannot put back together. The 4-bit chunk index
     * would allow 16 * 58 = 928 bytes, but crsf_mavlink_reasm_t::buf is
     * CRSF_MAVLINK_FRAME_MAX (281), so a longer frame went out as envelopes the
     * peer was guaranteed to abandon with CRSF_REASM_OVERFLOW partway through
     * -- wasted airtime, and CRSF_OK returned to the caller. esp_crsf.h has
     * always documented the limit as CRSF_MAVLINK_FRAME_MAX; enforce it.
     */
    if (len > CRSF_MAVLINK_FRAME_MAX) {
        return CRSF_ERR_INVALID_SIZE;
    }

    /*
     * An extended header costs two of the frame's payload bytes, so the chunk
     * budget shrinks with it. Splitting at the full CRSF_MAVLINK_CHUNK_MAX in
     * that mode produced a 60-byte envelope against a 62-byte body limit, and
     * send_mavlink_chunk() rejected chunk 0 -- so with the flag set, no frame
     * longer than one chunk could be sent at all.
     */
    const uint8_t chunk_max = h->mavlink_envelope_extended_header
                                  ? (uint8_t)(CRSF_MAVLINK_CHUNK_MAX - 2)
                                  : (uint8_t)CRSF_MAVLINK_CHUNK_MAX;

    crsf_mavlink_split_t sp;
    if (!crsf_mavlink_split_init_chunked(&sp, frame, len, chunk_max)) {
        return CRSF_ERR_INVALID_SIZE;
    }

    crsf_mavlink_envelope_t chunk;
    while (crsf_mavlink_split_next(&sp, &chunk)) {
        const crsf_err_t err = send_mavlink_chunk(h, destination, &chunk);
        if (err != CRSF_OK) {
            return err;
        }
    }
    return CRSF_OK;
}

crsf_err_t crsf_mavlink_on_frame(crsf_handle_t h, crsf_mavlink_cb_t cb, void *ctx)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    lock(h);
    h->mavlink_cb = cb;
    h->mavlink_ctx = ctx;
    unlock(h);
    return CRSF_OK;
}

crsf_err_t crsf_msp_send(crsf_handle_t h, uint8_t destination, const uint8_t *body,
                        size_t len, uint8_t version, bool is_response)
{
    if (!h || !body || len == 0) {
        return CRSF_ERR_INVALID_ARG;
    }

    crsf_msp_split_t sp;
    if (!crsf_msp_split_init(&sp, body, len, version, false, 0)) {
        return CRSF_ERR_INVALID_ARG;
    }

    const uint8_t type = is_response ? CRSF_TYPE_MSP_RESPONSE : CRSF_TYPE_MSP_REQUEST;
    uint8_t pl[1 + CRSF_MSP_CHUNK_MAX];
    size_t n;
    while ((n = crsf_msp_split_next(&sp, pl, sizeof(pl))) > 0) {
        const crsf_err_t err = crsf_send_frame(h, type, destination, pl, n);
        if (err != CRSF_OK) {
            return err;
        }
    }
    return CRSF_OK;
}

crsf_err_t crsf_msp_on_frame(crsf_handle_t h, crsf_msp_cb_t cb, void *ctx)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    lock(h);
    h->msp_cb = cb;
    h->msp_ctx = ctx;
    unlock(h);
    return CRSF_OK;
}

/* ------------------------------------------------------------------------- */
/* parameters                                                                */
/* ------------------------------------------------------------------------- */

crsf_err_t crsf_params_attach_provider(crsf_handle_t h, crsf_param_provider_t *provider)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    lock(h);
    h->provider = provider;
    unlock(h);
    return CRSF_OK;
}

crsf_err_t crsf_params_walk(crsf_handle_t h, uint8_t device, uint8_t total,
                           crsf_param_entry_cb_t on_entry,
                           crsf_param_done_cb_t on_done, void *ctx)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    lock(h);
    if (h->walk_active) {
        unlock(h);
        return CRSF_ERR_INVALID_STATE;
    }
    crsf_param_client_begin(&h->walk, device, total);
    h->walk_active = true;
    h->walk_entry_cb = on_entry;
    h->walk_done_cb = on_done;
    h->walk_ctx = ctx;
    h->walk_sent_us = 0;
    h->walk_retries = 0;
    unlock(h);
    return CRSF_OK;
}

/**
 * @brief Send the next 0x2C request of an active walk, or time the current one out.
 *
 * Runs on the TX task once per iteration. Requests are paced by
 * CRSF_PARAM_REQUEST_MS and retried up to CRSF_PARAM_MAX_RETRIES times; the
 * unanswered-parameter-0 case is what crsf.md:713 turns into "this firmware has
 * no root folder", handled inside crsf_param_client_timeout().
 *
 * @param h Port handle. Does nothing when no walk is active.
 */
static void walk_tick(crsf_handle_t h)
{
    uint8_t number, chunk, device;
    bool finished = false;
    /*
     * Distinguish "the list ended" from "we gave up". Both set finished, but
     * done_cb used to be handed a hardcoded true, so a walk against a device
     * that never answered reported success with zero parameters -- the
     * opposite of what esp_crsf.h documents.
     */
    bool walked_to_end = false;

    lock(h);
    if (!h->walk_active) {
        unlock(h);
        return;
    }
    device = h->walk.dest;

    const int64_t sent = h->walk_sent_us;
    const bool have_request = crsf_param_client_next_request(&h->walk, &number, &chunk);

    if (!have_request) {
        finished = true;
        walked_to_end = true;
        h->walk_active = false;
    } else if (sent != 0 && (now_us(h) - sent) < (int64_t)CRSF_PARAM_TIMEOUT_MS * 1000) {
        /* Still waiting for the outstanding reply. */
        unlock(h);
        return;
    } else if (sent != 0) {
        /* Timed out. crsf.md:713 gives parameter 0 special meaning; otherwise
         * re-request until we give up on this parameter. */
        h->walk_retries++;
        const bool give_up = h->walk_retries >= CRSF_PARAM_MAX_RETRIES;
        if (!crsf_param_client_timeout(&h->walk, give_up)) {
            /* The client could not continue at all: a genuine failure. */
            finished = true;
            h->walk_active = false;
        } else {
            h->walk_retries = give_up ? 0 : h->walk_retries;
            if (!crsf_param_client_next_request(&h->walk, &number, &chunk)) {
                /*
                 * The list is exhausted, but we only got here by abandoning at
                 * least one parameter to its retry budget, so the walk is
                 * incomplete. Leave walked_to_end false: the caller has had
                 * every entry that did arrive through on_entry, and ok = false
                 * tells it some are missing.
                 */
                finished = true;
                h->walk_active = false;
            }
        }
    }

    crsf_param_done_cb_t done_cb = h->walk_done_cb;
    void *walk_ctx = h->walk_ctx;
    if (!finished) {
        h->walk_sent_us = now_us(h);
    }
    unlock(h);

    if (finished) {
        if (done_cb) {
            done_cb(h, device, walked_to_end, walk_ctx);
        }
        return;
    }

    const uint8_t req[2] = {number, chunk};
    (void)crsf_send_frame(h, CRSF_TYPE_PARAM_READ, device, req, sizeof(req));
}

/**
 * @brief Feed a received 0x2B into an active walk.
 *
 * Ignores frames from a device other than the one being walked, so a chatty bus
 * cannot corrupt the chunk accumulator.
 *
 * @param h     Port handle.
 * @param frame The received 0x2B frame.
 */
static void walk_feed(crsf_handle_t h, const crsf_frame_t *frame)
{
    crsf_param_entry_cb_t entry_cb = NULL;
    crsf_param_done_cb_t done_cb = NULL;
    void *ctx = NULL;
    crsf_param_entry_t entry;
    uint8_t device = 0;
    bool have_entry = false, finished = false;

    lock(h);
    if (!h->walk_active || frame->origin != h->walk.dest) {
        unlock(h);
        return;
    }
    device = h->walk.dest;
    const crsf_param_walk_t r = crsf_param_client_feed(&h->walk, frame->payload,
                                                       frame->payload_len);
    h->walk_sent_us = 0; /* reply received, allow the next request immediately */
    h->walk_retries = 0;

    if (r == CRSF_PARAM_WALK_ENTRY) {
        entry = h->walk.entry;
        have_entry = true;
        entry_cb = h->walk_entry_cb;
    } else if (r == CRSF_PARAM_WALK_DONE) {
        finished = true;
        h->walk_active = false;
        done_cb = h->walk_done_cb;
    }
    ctx = h->walk_ctx;
    unlock(h);

    if (have_entry && entry_cb) {
        entry_cb(h, device, &entry, ctx);
    }
    if (finished && done_cb) {
        done_cb(h, device, true, ctx);
    }
}

crsf_err_t crsf_params_write_float(crsf_handle_t h, uint8_t device, uint8_t number,
                                  int32_t value)
{
    uint8_t pl[5];
    pl[0] = number;
    crsf_put_be32(&pl[1], (uint32_t)value);
    return crsf_send_frame(h, CRSF_TYPE_PARAM_WRITE, device, pl, sizeof(pl));
}

crsf_err_t crsf_params_write_selection(crsf_handle_t h, uint8_t device,
                                      uint8_t number, uint8_t index)
{
    const uint8_t pl[2] = {number, index};
    return crsf_send_frame(h, CRSF_TYPE_PARAM_WRITE, device, pl, sizeof(pl));
}

crsf_err_t crsf_params_write_string(crsf_handle_t h, uint8_t device, uint8_t number,
                                   const char *value)
{
    if (!value) {
        return CRSF_ERR_INVALID_ARG;
    }
    uint8_t pl[CRSF_MAX_EXT_PAYLOAD_SIZE];
    size_t n = 0;
    pl[n++] = number;
    while (n < sizeof(pl) - 1 && *value) {
        pl[n++] = (uint8_t)*value++;
    }
    pl[n++] = '\0';
    return crsf_send_frame(h, CRSF_TYPE_PARAM_WRITE, device, pl, n);
}

crsf_err_t crsf_params_command(crsf_handle_t h, uint8_t device, uint8_t number,
                              crsf_cmd_status_t status)
{
    const uint8_t pl[2] = {number, (uint8_t)status};
    return crsf_send_frame(h, CRSF_TYPE_PARAM_WRITE, device, pl, sizeof(pl));
}

/**
 * @brief Serve an inbound 0x2C read or 0x2D write using the attached provider.
 *
 * Answering a write is a MUST (crsf.md:908), and the reply's frame type depends
 * on the parameter type (crsf.md:917), which is why the provider returns the type
 * to use rather than this function assuming one.
 *
 * @param h     Port handle. Does nothing when no provider is attached.
 * @param frame The received 0x2C or 0x2D frame.
 */
static void provider_handle(crsf_handle_t h, const crsf_frame_t *frame)
{
    uint8_t out[CRSF_MAX_EXT_PAYLOAD_SIZE];
    size_t n = 0;
    uint8_t reply_type = 0;

    /*
     * Take only the provider pointer under the lock and release it before
     * serving the request. crsf_param_provider_write() calls the application's
     * on_write / on_command, and h->lock is a non-recursive mutex taken with
     * portMAX_DELAY: a callback that touches any locking API (crsf_publish_*,
     * crsf_telemetry_set_interval, crsf_get_channels, ...) would deadlock the
     * RX task against itself, killing the link permanently. This is the same
     * rule dispatch_callbacks() follows, and the invariant stated at the top of
     * this file -- the mutex is never held across an application callback.
     *
     * The provider itself is owned by the application, which attaches and
     * detaches it; serving it unlocked is no weaker than the callback contract
     * already is.
     */
    lock(h);
    crsf_param_provider_t *prov = h->provider;
    unlock(h);
    if (!prov) {
        return;
    }

    if (frame->type == CRSF_TYPE_PARAM_READ) {
        n = crsf_param_provider_read(prov, frame->payload, frame->payload_len,
                                     out, sizeof(out));
        reply_type = CRSF_TYPE_PARAM_ENTRY;
    } else if (frame->type == CRSF_TYPE_PARAM_WRITE) {
        crsf_param_reply_t reply = CRSF_PARAM_REPLY_NONE;
        n = crsf_param_provider_write(prov, frame->payload, frame->payload_len,
                                      out, sizeof(out), &reply);
        /* crsf.md:917 — value types answer with 0x2D, COMMAND with 0x2B. */
        if (reply == CRSF_PARAM_REPLY_ENTRY) {
            reply_type = CRSF_TYPE_PARAM_ENTRY;
        } else if (reply == CRSF_PARAM_REPLY_VALUE) {
            reply_type = CRSF_TYPE_PARAM_WRITE;
        }
    }

    if (n > 0 && reply_type != 0) {
        (void)crsf_send_frame(h, reply_type, frame->origin, out, n);
    }
}

/* ------------------------------------------------------------------------- */
/* receive path                                                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Run the application callbacks registered for this frame.
 *
 * Both the type-specific callback and the type-0 catch-all fire, in that order.
 * The mutex is not held while a callback runs, so a callback may call back into
 * the API.
 *
 * @param h     Port handle.
 * @param frame The received frame.
 */
static void dispatch_callbacks(crsf_handle_t h, const crsf_frame_t *frame)
{
    /* Snapshot under the lock, call outside it, so a callback that calls back
     * into this component cannot deadlock. */
    crsf_cb_entry_t local[CRSF_MAX_CALLBACKS];
    lock(h);
    memcpy(local, h->callbacks, sizeof(local));
    unlock(h);

    for (int i = 0; i < CRSF_MAX_CALLBACKS; i++) {
        if (local[i].cb && (local[i].type == 0 || local[i].type == frame->type)) {
            local[i].cb(h, frame, local[i].ctx);
        }
    }
}

/**
 * @brief Act on an inbound 0x32 Direct Command.
 *
 * Verifies the nested 0xBA CRC first and drops the frame if it fails — the outer
 * frame CRC is not sufficient for a command (crsf.md:936). Two sets concern the
 * transport itself and are acted on here: baudrate negotiation (0x0A) and flow
 * control (0x20). An inbound 0xFF ACK is ignored, since acknowledging an
 * acknowledgement would not terminate. Every other set is answered with an ACK
 * carrying @c acted = false, which crsf.md:972 defines as "arrived but not
 * implemented here".
 *
 * The application still sees the frame through its own 0x32 callback either way.
 *
 * @param h     Port handle.
 * @param frame The received command frame.
 */
static void handle_command(crsf_handle_t h, const crsf_frame_t *frame)
{
    /* crsf.md:936 — reject anything whose 0xBA CRC does not check out. */
    if (!crsf_command_crc_ok(frame)) {
        event(h, CRSF_EV_COMMAND_BAD_CRC, 0, 0);
        return;
    }
    if (frame->payload_len < 3) {
        return;
    }

    const uint8_t cmd = frame->payload[0];
    const uint8_t sub = frame->payload[1];
    const uint8_t *args = &frame->payload[2];
    const size_t args_len = (size_t)(frame->payload_len - 3);

    switch (cmd) {
    case CRSF_CMD_GENERAL:
        if (sub == CRSF_CMD_GEN_SPEED_PROPOSAL && args_len >= 5) {
            const uint8_t port = args[0];
            const uint32_t baud = crsf_get_be32(&args[1]);
            const bool accept = (baud >= 9600 && baud <= 5250000);
            const uint8_t resp[2] = {port, accept ? 1u : 0u};
            /*
             * Only commit to the switch if the acceptance was actually queued.
             * tx_enqueue() returns CRSF_ERR_TIMEOUT when the 12-deep TX queue
             * stays full for 20 ms, which a busy telemetry schedule can do.
             * Committing regardless meant the peer never saw the acceptance,
             * stayed at the old rate, and our end switched anyway on the next
             * frame drained -- a one-sided change that kills the link until
             * reboot.
             */
            const crsf_err_t sent_resp =
                crsf_send_command(h, frame->origin, CRSF_CMD_GENERAL,
                                  CRSF_CMD_GEN_SPEED_PROPOSAL_RESPONSE,
                                  resp, sizeof(resp));
            if (accept && sent_resp == CRSF_OK) {
                /* Switch only once the response has physically left the wire;
                 * the TX task does that when it drains the queue. */
                lock(h);
                h->pending_baudrate = baud;
                unlock(h);
                event(h, CRSF_EV_BAUD_PROPOSAL_ACCEPTED, baud, 0);
            } else if (accept) {
                event(h, CRSF_EV_BAUD_ACCEPT_SEND_FAILED, (uint32_t)sent_resp,
                      h->baudrate);
            }
        } else if (sub == CRSF_CMD_GEN_SPEED_PROPOSAL_RESPONSE && args_len >= 2) {
            event(h, CRSF_EV_BAUD_PEER_REPLY, args[1] ? 1u : 0u, 0);
            if (!args[1]) {
                lock(h);
                h->pending_baudrate = 0;
                unlock(h);
            }
        } else {
            /*
             * A sub-command of a set we do handle, but not this one -- or one
             * whose arguments are too short. crsf.md:972's "arrived but not
             * implemented here" applies just as much as it does to an entirely
             * unknown set, which the default arm below already ACKs. Falling
             * out silently left a probing peer unable to tell the difference
             * between unimplemented and a lost frame.
             */
            crsf_send_command_ack(h, frame->origin, cmd, sub, false, NULL);
        }
        break;

    case CRSF_CMD_FLOW_CONTROL:
        /* crsf.md:1097 — a peer throttling or subscribing to a frame type. */
        /* ACK what actually happened: set_interval refuses frame type 0 and
         * runs out of slots, and telling the peer "done" either way leaves it
         * believing in a subscription it does not have. */
        if (sub == CRSF_CMD_FLOW_SUBSCRIBE && args_len >= 3) {
            const crsf_err_t err =
                crsf_telemetry_set_interval(h, args[0], crsf_get_be16(&args[1]));
            crsf_send_command_ack(h, frame->origin, cmd, sub, err == CRSF_OK, NULL);
        } else if (sub == CRSF_CMD_FLOW_UNSUBSCRIBE && args_len >= 1) {
            const crsf_err_t err = crsf_telemetry_set_interval(h, args[0], 0);
            crsf_send_command_ack(h, frame->origin, cmd, sub, err == CRSF_OK, NULL);
        } else {
            /* See the note in CRSF_CMD_GENERAL: answer, do not go quiet. */
            crsf_send_command_ack(h, frame->origin, cmd, sub, false, NULL);
        }
        break;

    case CRSF_CMD_ACK:
        /* Someone acknowledged one of our commands; nothing to do here, but the
         * application may have registered a 0x32 callback. */
        break;

    default:
        /*
         * Unhandled command sets still get an ACK with action = 0, which tells
         * the sender the command arrived but does nothing here (crsf.md:972).
         */
        crsf_send_command_ack(h, frame->origin, cmd, sub, false, NULL);
        break;
    }
}

/**
 * @brief Feed an inbound 0xAA chunk into the MAVLink reassembler.
 *
 * Fires the application callback once a whole frame is complete. The reassembly
 * buffer is copied out under the lock before the callback runs, so the next chunk
 * cannot overwrite a frame the application is still reading.
 *
 * @param h     Port handle.
 * @param frame The received envelope frame.
 */
static void handle_mavlink_envelope(crsf_handle_t h, const crsf_frame_t *frame)
{
    crsf_mavlink_envelope_t chunk;
    if (!crsf_decode_mavlink_envelope(frame->payload, frame->payload_len, &chunk)) {
        return;
    }

    lock(h);
    const crsf_reasm_result_t r = crsf_mavlink_reasm_push(&h->mavlink_reasm, &chunk);
    crsf_mavlink_cb_t cb = h->mavlink_cb;
    void *ctx = h->mavlink_ctx;
    size_t len = 0;
    if (r == CRSF_REASM_COMPLETE) {
        len = h->mavlink_reasm.len;
        memcpy(h->mavlink_assembled, h->mavlink_reasm.buf, len);
    }
    unlock(h);

    if (r == CRSF_REASM_COMPLETE && cb) {
        cb(h, h->mavlink_assembled, len, ctx);
    }
}

/**
 * @brief Feed an inbound 0x7A / 0x7B payload into the MSP reassembler.
 *
 * Fires the application callback once a whole body is complete, handing over the
 * origin address so a response can be routed back with the addresses swapped as
 * crsf.md:1228 requires.
 *
 * @param h     Port handle.
 * @param frame The received MSP frame.
 */
static void handle_msp(crsf_handle_t h, const crsf_frame_t *frame)
{
    const bool is_response = (frame->type == CRSF_TYPE_MSP_RESPONSE);

    lock(h);
    /*
     * One reassembler per port, but chunks can interleave from several senders
     * on the same bus. The 4-bit sequence numbers then line up by chance about
     * one time in sixteen per chunk, and two unrelated bodies get spliced into
     * one "complete" frame. Start over whenever the sender changes, so a
     * partial body from another origin is discarded rather than blended in.
     *
     * msp_origin and msp_is_response were previously written here and read
     * nowhere; this is the check they were evidently meant for.
     */
    if (h->msp_reasm.active &&
        (frame->origin != h->msp_origin || is_response != h->msp_is_response)) {
        crsf_msp_reasm_reset(&h->msp_reasm);
    }
    const crsf_reasm_result_t r = crsf_msp_reasm_push(&h->msp_reasm, frame->payload,
                                                      frame->payload_len);
    h->msp_origin = frame->origin;
    h->msp_is_response = is_response;

    crsf_msp_cb_t cb = h->msp_cb;
    void *ctx = h->msp_ctx;
    size_t len = 0;
    uint8_t version = 0;
    bool error = false;
    if (r == CRSF_REASM_COMPLETE) {
        len = h->msp_reasm.len;
        version = h->msp_reasm.version;
        error = h->msp_reasm.error;
        memcpy(h->msp_assembled, h->msp_reasm.body, len);
    }
    unlock(h);

    if (r == CRSF_REASM_COMPLETE && cb) {
        cb(h, frame->origin, is_response, h->msp_assembled, len, version, error, ctx);
    }
}

/**
 * @brief Decide where a received frame goes, and repeat it there (crsf.md:181).
 *
 * With no router attached the frame is simply consumed locally, which is the
 * single-port case. Otherwise the routing decision comes from crsf_router_route()
 * and the **original bytes** are queued unchanged on each target port. Repeating
 * the raw frame rather than re-encoding it matters: a frame of a type we do not
 * decode still forwards correctly, and a re-encode could not reproduce one
 * byte-for-byte anyway.
 *
 * @param h        Port handle the frame arrived on.
 * @param frame    The received frame, for its type and addresses.
 * @param raw      The frame exactly as received, sync through CRC.
 * @param raw_len  Length of @p raw.
 * @param decision Receives the decision; @c consume_locally tells the caller
 *                 whether to go on processing the frame itself.
 */
static void route_frame(crsf_handle_t h, const crsf_frame_t *frame,
                        const uint8_t *raw, size_t raw_len,
                        crsf_route_decision_t *decision)
{
    lock(h);
    crsf_router_t *router = h->router;
    const uint8_t my_port = h->router_port;
    unlock(h);

    if (!router) {
        decision->consume_locally = true;
        decision->port_count = 0;
        return;
    }

    /*
     * One router is shared by every port on the node, so its table and counters
     * are touched by each port's RX task. h->lock cannot protect it -- that is
     * per port -- so the cross-port registry lock covers the router too.
     *
     * Without it, two RX tasks learning new origins at the same time both scan
     * for the first free slot, both pick the same one, and one write is lost or
     * the entry ends up holding one frame's address against the other's port,
     * mis-routing traffic from then on. The counters lose updates the same way.
     *
     * The lock is also held across tx_enqueue() rather than released with the
     * peer pointer in hand: otherwise a concurrent crsf_close(peer) could free
     * the handle in that window, and tx_enqueue() would read peer->running out
     * of freed memory. tx_enqueue() does not take this lock, so holding it here
     * cannot deadlock.
     */
    router_lock(router);

    crsf_router_route(router, frame, my_port, decision);

    for (uint8_t i = 0; i < decision->port_count; i++) {
        const uint8_t target = decision->ports[i];
        crsf_handle_t peer = NULL;

        if (target < CRSF_ROUTER_MAX_PORTS) {
            peer = router->ports[target];
        }

        if (peer && peer != h) {
            /* Forward the frame verbatim: re-encoding could alter a payload we
             * do not understand, which crsf.md:175 warns against. */
            (void)tx_enqueue(peer, raw, raw_len);
        }
    }

    router_unlock(router);
}

/**
 * @brief Parser callback: snapshots, protocol services, routing, then the app.
 *
 * The single entry point for every validated frame, and the order matters.
 * Forwarding happens before local processing so a frame addressed elsewhere is
 * not delayed by our own handling of it, and the application callbacks run last
 * so they see state the protocol services have already updated.
 *
 * @param frame The validated frame.
 * @param ctx   The crsf_handle_t, given to crsf_parser_init().
 */
static void on_frame(const crsf_frame_t *frame, void *ctx)
{
    crsf_handle_t h = (crsf_handle_t)ctx;

    /*
     * Rebuild the frame verbatim for forwarding. The parser hands out a view into
     * its own buffer, and the sync/length bytes are not part of it.
     */
    crsf_route_decision_t decision = {.consume_locally = true, .port_count = 0};
    if (h->router) {
        uint8_t raw[CRSF_MAX_FRAME_SIZE];
        const size_t raw_len = crsf_build_frame(raw, frame->sync, frame->type,
                                                frame->destination, frame->origin,
                                                frame->payload, frame->payload_len);
        if (raw_len > 0) {
            route_frame(h, frame, raw, raw_len, &decision);
        }
    }

    if (!decision.consume_locally) {
        return;
    }

    switch (frame->type) {
    case CRSF_TYPE_RC_CHANNELS_PACKED: {
        crsf_channels_t ch;
        if (crsf_decode_channels(frame->payload, frame->payload_len, &ch)) {
            lock(h);
            h->channels = ch;
            h->channels_us = now_us(h);
            unlock(h);
        }
        break;
    }

    case CRSF_TYPE_LINK_STATISTICS: {
        crsf_link_statistics_t ls;
        if (crsf_decode_link_statistics(frame->payload, frame->payload_len, &ls)) {
            lock(h);
            h->link_stats = ls;
            h->link_stats_us = now_us(h);
            unlock(h);
        }
        break;
    }

    case CRSF_TYPE_DEVICE_PING:
        /* crsf.md:651 — answer a ping addressed to us or broadcast with 0x29. */
        if (h->auto_respond_ping &&
            (frame->destination == h->self_address ||
             frame->destination == CRSF_ADDR_BROADCAST)) {
            crsf_send_device_info(h, frame->origin);
        }
        break;

    case CRSF_TYPE_COMMAND:
        handle_command(h, frame);
        break;

    case CRSF_TYPE_PARAM_READ:
    case CRSF_TYPE_PARAM_WRITE:
        provider_handle(h, frame);
        break;

    case CRSF_TYPE_PARAM_ENTRY:
        walk_feed(h, frame);
        break;

    case CRSF_TYPE_MAVLINK_ENVELOPE:
        handle_mavlink_envelope(h, frame);
        break;

    case CRSF_TYPE_MSP_REQUEST:
    case CRSF_TYPE_MSP_RESPONSE:
        handle_msp(h, frame);
        break;

    default:
        break;
    }

    dispatch_callbacks(h, frame);
}


/**
 * @brief Switch to a negotiated baudrate once the reply has left the wire.
 *
 * Called from the TX task right after a frame was written, which is the only
 * point at which the 0x71 response is known to have been transmitted. Switching
 * any earlier would change the line rate underneath our own reply.
 *
 * @param h Port handle.
 */
static void apply_pending_baudrate(crsf_handle_t h)
{
    lock(h);
    const uint32_t target = h->pending_baudrate;
    h->pending_baudrate = 0;
    unlock(h);

    if (target == 0 || target == h->baudrate) {
        return;
    }
    if (h->ops->set_baudrate && h->ops->set_baudrate(h->io_ctx, target)) {
        lock(h);
        h->previous_baudrate = h->baudrate;
        h->baudrate = target;
        h->baud_switch_us = now_us(h);
        h->baud_switch_frames_ok = h->parser.stats.frames_ok;
        unlock(h);
        if (h->ops->flush_rx) {
            h->ops->flush_rx(h->io_ctx);
        }
        /* Ask the RX task to resync; the parser is not ours to touch. */
        h->parser_reset_pending = true;
        event(h, CRSF_EV_BAUD_APPLIED, target, 0);
    }
}

/**
 * @brief Roll back when nothing valid arrives after a baudrate switch.
 *
 * A wrong baudrate is otherwise unrecoverable without a reboot: nothing decodes,
 * so the peer cannot be told to change back. After CRSF_BAUD_CONFIRM_MS we check
 * for real traffic and revert if there is none.
 *
 * @param h Port handle. Does nothing unless a switch is being evaluated.
 *
 * @note The spec defines no fallback at all (crsf.md:1052-1062); this is our
 *       policy, not a requirement.
 */
static void check_baudrate_fallback(crsf_handle_t h)
{
    lock(h);
    const int64_t switched = h->baud_switch_us;
    const uint32_t previous = h->previous_baudrate;
    unlock(h);

    if (switched == 0 || previous == 0) {
        return;
    }
    if ((now_us(h) - switched) < (int64_t)CRSF_BAUD_CONFIRM_MS * 1000) {
        return;
    }

    /* Read the 64-bit timestamp under the lock: not atomic on a 32-bit core. */
    lock(h);
    const uint32_t frames_ok = h->parser.stats.frames_ok;
    const uint32_t frames_at_switch = h->baud_switch_frames_ok;
    h->baud_switch_us = 0;
    unlock(h);

    /*
     * "Did anything at all decode since we switched?" -- not "did an 0x16
     * arrive?". Gating on channel frames declared a perfectly good link dead
     * whenever it does not carry them: a CRSF_ROLE_TX port receives telemetry
     * and never 0x16, so channels_us stayed 0, age_ms_from(0) returned
     * UINT32_MAX, and the fallback reverted our end while the peer stayed at
     * the new rate -- producing exactly the one-sided, reboot-only-recoverable
     * state this function exists to prevent.
     *
     * The subtraction is unsigned and wraps correctly if the counter rolls over.
     */
    const bool healthy = (uint32_t)(frames_ok - frames_at_switch) > 0;

    if (!healthy) {
        event(h, CRSF_EV_BAUD_REVERTING, previous, 0);
        const bool ok =
            h->ops->set_baudrate && h->ops->set_baudrate(h->io_ctx, previous);
        lock(h);
        if (ok) {
            h->baudrate = previous;
        } else {
            /*
             * Clear previous_baudrate even so. Leaving it set with
             * baud_switch_us already zeroed wedged this function: the guard
             * above returns early forever, so no further attempt was ever made.
             */
            event(h, CRSF_EV_BAUD_REVERT_FAILED, (uint32_t)CRSF_ERR_INVALID_STATE, 0);
        }
        h->previous_baudrate = 0;
        unlock(h);
        if (ok) {
            if (h->ops->flush_rx) {
                h->ops->flush_rx(h->io_ctx);
            }
            /* Ask the RX task to resync; the parser is not ours to touch. */
            h->parser_reset_pending = true;
        }
    } else {
        lock(h);
        h->previous_baudrate = 0;
        unlock(h);
    }
}

/* ------------------------------------------------------------------------- */
/* accessors                                                                 */
/* ------------------------------------------------------------------------- */

crsf_err_t crsf_get_channels(crsf_handle_t h, crsf_channels_t *out, uint32_t *age_ms)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    lock(h);
    const int64_t stamp = h->channels_us;
    if (out && stamp != 0) {
        *out = h->channels;
    }
    unlock(h);

    if (age_ms) {
        *age_ms = age_ms_from(h, stamp);
    }
    return (stamp == 0) ? CRSF_ERR_NOT_FOUND : CRSF_OK;
}

bool crsf_link_is_up(crsf_handle_t h)
{
    if (!h) {
        return false;
    }
    lock(h);
    const int64_t stamp = h->channels_us;
    const uint32_t timeout = h->link_timeout_ms;
    unlock(h);

    if (stamp == 0) {
        return false;
    }
    return age_ms_from(h, stamp) < timeout;
}

crsf_err_t crsf_set_link_timeout(crsf_handle_t h, uint32_t timeout_ms)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    lock(h);
    h->link_timeout_ms = timeout_ms;
    unlock(h);
    return CRSF_OK;
}

crsf_err_t crsf_get_link_statistics(crsf_handle_t h, crsf_link_statistics_t *out,
                                   uint32_t *age_ms)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    lock(h);
    const int64_t stamp = h->link_stats_us;
    if (out && stamp != 0) {
        *out = h->link_stats;
    }
    unlock(h);

    if (age_ms) {
        *age_ms = age_ms_from(h, stamp);
    }
    return (stamp == 0) ? CRSF_ERR_NOT_FOUND : CRSF_OK;
}

crsf_err_t crsf_get_parser_stats(crsf_handle_t h, crsf_parser_stats_t *out)
{
    if (!h || !out) {
        return CRSF_ERR_INVALID_ARG;
    }
    lock(h);
    *out = h->parser.stats;
    unlock(h);
    return CRSF_OK;
}

crsf_err_t crsf_on_frame(crsf_handle_t h, uint8_t type, crsf_frame_cb_t cb, void *ctx)
{
    if (!h || !cb) {
        return CRSF_ERR_INVALID_ARG;
    }
    lock(h);
    for (int i = 0; i < CRSF_MAX_CALLBACKS; i++) {
        if (h->callbacks[i].cb && h->callbacks[i].type == type) {
            h->callbacks[i].cb = cb;
            h->callbacks[i].ctx = ctx;
            unlock(h);
            return CRSF_OK;
        }
    }
    for (int i = 0; i < CRSF_MAX_CALLBACKS; i++) {
        if (!h->callbacks[i].cb) {
            h->callbacks[i].type = type;
            h->callbacks[i].cb = cb;
            h->callbacks[i].ctx = ctx;
            unlock(h);
            return CRSF_OK;
        }
    }
    unlock(h);
    return CRSF_ERR_NO_MEM;
}

crsf_err_t crsf_off_frame(crsf_handle_t h, uint8_t type)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    lock(h);
    for (int i = 0; i < CRSF_MAX_CALLBACKS; i++) {
        if (h->callbacks[i].cb && h->callbacks[i].type == type) {
            memset(&h->callbacks[i], 0, sizeof(h->callbacks[i]));
            unlock(h);
            return CRSF_OK;
        }
    }
    unlock(h);
    return CRSF_ERR_NOT_FOUND;
}

uint32_t crsf_get_baudrate(crsf_handle_t h)
{
    return h ? h->baudrate : 0;
}

uint8_t crsf_self_address(crsf_handle_t h)
{
    return h ? h->self_address : 0;
}

crsf_err_t crsf_propose_baudrate(crsf_handle_t h, uint8_t destination,
                                uint32_t baudrate)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    uint8_t args[5];
    args[0] = 0; /* port id */
    crsf_put_be32(&args[1], baudrate);

    lock(h);
    h->pending_baudrate = 0; /* set only once the peer accepts */
    unlock(h);

    return crsf_send_command(h, destination, CRSF_CMD_GENERAL,
                             CRSF_CMD_GEN_SPEED_PROPOSAL, args, sizeof(args));
}

/* ------------------------------------------------------------------------- */
/* routing attachment                                                        */
/* ------------------------------------------------------------------------- */

crsf_err_t crsf_router_attach(crsf_handle_t h, crsf_router_t *router,
                             uint8_t port_index)
{
    if (!h || !router || port_index >= CRSF_ROUTER_MAX_PORTS) {
        return CRSF_ERR_INVALID_ARG;
    }
    if (port_index >= router->port_count) {
        return CRSF_ERR_INVALID_ARG;
    }
    /*
     * The router decides "addressed to us" from its own self_address, while
     * auto_respond_ping and the local handlers use this port's. If the two
     * disagree, a frame addressed to the port is not consumed locally: it gets
     * looked up and forwarded out the other port instead, so a 0x28 ping is
     * never answered and the device simply never appears in a handset's device
     * list. Refuse the attach rather than let that be debugged from silence.
     */
    if (router->self_address != h->self_address) {
        event(h, CRSF_EV_ROUTER_ADDRESS_MISMATCH, router->self_address,
              h->self_address);
        return CRSF_ERR_INVALID_ARG;
    }

    /*
     * Install the lock before the first write to the table. Idempotent, and
     * done here rather than in crsf_router_init() because the router is the
     * application's object and knows nothing about FreeRTOS.
     */
    crsf_router_set_lock(router, h->ops->router_lock, h->ops->router_unlock,
                         h->io_ctx);

    router_lock(router);
    if (router->ports[port_index] && router->ports[port_index] != h) {
        router_unlock(router);
        return CRSF_ERR_INVALID_STATE;
    }
    /*
     * Clear any slot this handle already occupies. Re-attaching at a different
     * index used to leave the old entry pointing here, and crsf_router_detach()
     * only clears h->router_port -- the latest one. crsf_close() would then
     * free the handle with the earlier slot still holding it, and another port
     * forwarding to that index would tx_enqueue() into freed memory.
     */
    for (uint8_t i = 0; i < CRSF_ROUTER_MAX_PORTS; i++) {
        if (i != port_index && router->ports[i] == h) {
            router->ports[i] = NULL;
        }
    }
    router->ports[port_index] = h;
    router_unlock(router);

    lock(h);
    h->router = router;
    h->router_port = port_index;
    unlock(h);
    return CRSF_OK;
}

crsf_err_t crsf_router_detach(crsf_handle_t h)
{
    if (!h) {
        return CRSF_ERR_INVALID_ARG;
    }
    lock(h);
    const uint8_t port = h->router_port;
    crsf_router_t *router = h->router;
    h->router = NULL;
    unlock(h);

    if (router) {
        router_lock(router);
        if (port < CRSF_ROUTER_MAX_PORTS && router->ports[port] == h) {
            router->ports[port] = NULL;
        }
        router_unlock(router);
    }
    return CRSF_OK;
}

/* ------------------------------------------------------------------------- */
/* what a platform port drives                                               */
/* ------------------------------------------------------------------------- */

crsf_err_t crsf_port_init(crsf_handle_t h, const crsf_io_ops_t *ops, void *ctx,
                          const crsf_port_config_t *cfg)
{
    if (!h || !ops || !cfg || !ops->tx_push || !ops->now_us) {
        return CRSF_ERR_INVALID_ARG;
    }

    memset(h, 0, sizeof(*h));
    h->ops = ops;
    h->io_ctx = ctx;

    h->role = cfg->role;
    h->auto_respond_ping = cfg->auto_respond_ping;
    h->mavlink_envelope_extended_header = cfg->mavlink_envelope_extended_header;

    h->baudrate = cfg->baudrate
                      ? cfg->baudrate
                      : (cfg->wiring == CRSF_WIRING_HALF_DUPLEX_SINGLE_WIRE
                             ? CRSF_BAUD_HALF_DUPLEX_DEFAULT
                             : CRSF_BAUD_FULL_DUPLEX_DEFAULT);
    h->self_address = cfg->self_address
                          ? cfg->self_address
                          : (cfg->role == CRSF_ROLE_TX ? CRSF_ADDR_TRANSMITTER
                                                       : CRSF_ADDR_RECEIVER);
    h->link_timeout_ms = cfg->link_timeout_ms ? cfg->link_timeout_ms
                                              : CRSF_DEFAULT_LINK_TIMEOUT_MS;

    /* Identity for the 0x29 reply. Copied, so the caller's string may go away. */
    const char *name = cfg->device_name ? cfg->device_name : "crsf";
    size_t i = 0;
    while (i < CRSF_DEVICE_NAME_MAX_LEN - 1 && name[i] != '\0') {
        h->device_info.device_name[i] = name[i];
        i++;
    }
    h->device_info.device_name[i] = '\0';
    h->device_info.serial_number = cfg->serial_number;
    h->device_info.hardware_id = cfg->hardware_id;
    h->device_info.firmware_id = cfg->firmware_id;
    h->device_info.parameters_total = 0;
    h->device_info.parameter_version = 1;

    crsf_mavlink_reasm_reset(&h->mavlink_reasm);
    crsf_msp_reasm_reset(&h->msp_reasm);
    crsf_parser_init(&h->parser, on_frame, h);

    h->running = true;
    return CRSF_OK;
}

void crsf_port_feed(crsf_handle_t h, const uint8_t *data, size_t len)
{
    if (!h || !data || len == 0) {
        return;
    }
    crsf_parser_push(&h->parser, data, len);
}

void crsf_port_reset_parser(crsf_handle_t h)
{
    if (h) {
        crsf_parser_reset(&h->parser);
    }
}

bool crsf_port_poll_reset(crsf_handle_t h)
{
    if (!h || !h->parser_reset_pending) {
        return false;
    }
    h->parser_reset_pending = false;
    crsf_parser_reset(&h->parser);
    return true;
}

void crsf_port_tick(crsf_handle_t h)
{
    if (!h) {
        return;
    }
    scheduler_tick(h);
    walk_tick(h);
    check_baudrate_fallback(h);
}

void crsf_port_leave_router(crsf_handle_t h)
{
    if (!h) {
        return;
    }

    /* Read the router before detaching, which is what clears it. */
    lock(h);
    crsf_router_t *was_on = h->router;
    unlock(h);

    crsf_router_detach(h);

    /*
     * Sweep that router for this handle regardless of what detach thought our
     * port index was. Nothing should be left after crsf_router_detach(), but a
     * stale entry here becomes a dangling pointer the moment the port is freed,
     * and another port forwarding to that index would transmit into it.
     */
    if (was_on) {
        router_lock(was_on);
        for (uint8_t i = 0; i < CRSF_ROUTER_MAX_PORTS; i++) {
            if (was_on->ports[i] == h) {
                was_on->ports[i] = NULL;
            }
        }
        router_unlock(was_on);
    }
}

void crsf_port_after_tx(crsf_handle_t h)
{
    if (h) {
        apply_pending_baudrate(h);
    }
}
