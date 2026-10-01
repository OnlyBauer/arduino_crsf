/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_core.h
 * @brief The CRSF protocol, and the whole public API, free of any platform.
 *
 * Everything here compiles with a plain C11 compiler and libm. It knows nothing
 * about UARTs, tasks or timers: what it needs from underneath, it asks for
 * through the seven hooks in crsf_io.h.
 *
 * A platform port is therefore small. It opens a serial device, hands received
 * bytes to crsf_port_feed(), calls crsf_port_tick() periodically, and supplies
 * the hooks. The telemetry scheduler, the parameter protocol in both
 * directions, the MAVLink and MSP tunnels, 0x32 Direct Commands, routing and
 * CRSFv3 baudrate negotiation all come with this file and are identical on
 * every platform.
 *
 * @par Context rules
 * crsf_port_feed() runs the parser, the protocol services and the application's
 * frame callbacks, and it transmits. It is **not** callable from an interrupt.
 * A driver that receives in an ISR must buffer the bytes and let its main loop
 * or task hand them over.
 *
 * @par Allocation
 * None. A crsf_port_t is an ordinary struct the caller places wherever it
 * likes, exactly like crsf_parser_t or crsf_router_t.
 */

#ifndef CRSF_CORE_H
#define CRSF_CORE_H

#include "crsf_protocol.h"
#include "crsf_codec.h"
#include "crsf_parser.h"
#include "crsf_tunnel.h"
#include "crsf_params.h"
#include "crsf_router.h"

#include "crsf_err.h"
#include "crsf_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup crsf_core Protocol core
 * @brief The frame layer, telemetry, parameters, tunnels, routing and the API.
 * @{
 */

/** Opaque handle to one open CRSF port. */
typedef struct crsf_port *crsf_handle_t;

/**
 * @brief One CRSF port, as a type you can declare.
 *
 * The same object a crsf_handle_t points at. Named so a port can be placed
 * in static storage or on a stack without an allocator -- the library itself
 * never calls one. Its fields are visible for that reason alone; treat every
 * one of them as private.
 */
typedef struct crsf_port crsf_port_t;

/**
 * @name Roles and wiring
 * @{
 */

/** Physical wiring, which sets the default baudrate and turnaround handling. */
typedef enum {
    /**
     * Two wires, non-inverted only, default 416666 baud (crsf.md:136).
     * The usual arrangement between a receiver and a flight controller.
     */
    CRSF_WIRING_FULL_DUPLEX = 0,
    /**
     * One wire shared by both directions, default 400000 baud, inversion
     * permitted (crsf.md:130-132). The usual arrangement between a handset and a
     * transmitter module. Set tx_pin and rx_pin to the same GPIO.
     */
    CRSF_WIRING_HALF_DUPLEX_SINGLE_WIRE,
} crsf_wiring_t;

/** Which end of the link this port is. */
typedef enum {
    CRSF_ROLE_TX = 0, /**< sending station: sends channels, receives telemetry */
    CRSF_ROLE_RX,     /**< receiving station: receives channels, sends telemetry */
} crsf_role_t;

/** @} */

/**
 * @name Receiving
 * @{
 */

/**
 * @brief Callback for a received frame.
 *
 * Runs on the port's RX task. @p frame->payload is only valid for the duration
 * of the call — copy anything you need to keep. Keep the work short.
 *
 * @param h     The port the frame arrived on.
 * @param frame The validated frame.
 * @param ctx   The context given to crsf_on_frame().
 */
typedef void (*crsf_frame_cb_t)(crsf_handle_t h, const crsf_frame_t *frame, void *ctx);

/**
 * @brief Register a callback for one frame type.
 *
 * @param h    Port handle.
 * @param type Frame type to watch, or 0 to receive every frame — including types
 *             this component does not decode itself.
 * @param cb   Callback; replaces any previous one for @p type.
 * @param ctx  Passed back to @p cb unchanged.
 * @retval CRSF_OK              Registered.
 * @retval CRSF_ERR_INVALID_ARG @p h or @p cb is NULL.
 * @retval CRSF_ERR_NO_MEM      All CRSF_MAX_CALLBACKS slots are taken.
 */
crsf_err_t crsf_on_frame(crsf_handle_t h, uint8_t type, crsf_frame_cb_t cb, void *ctx);

/**
 * @brief Remove the callback registered for @p type.
 *
 * @param h    Port handle.
 * @param type The type passed to crsf_on_frame(), 0 for the catch-all.
 * @retval CRSF_OK              Removed.
 * @retval CRSF_ERR_NOT_FOUND   Nothing was registered for @p type.
 * @retval CRSF_ERR_INVALID_ARG @p h is NULL.
 */
crsf_err_t crsf_off_frame(crsf_handle_t h, uint8_t type);

/**
 * @brief Latest received channel values.
 *
 * @param h      Port handle.
 * @param out    Receives the channels; may be NULL if only the age is wanted.
 *               Left untouched when nothing has arrived yet.
 * @param age_ms Milliseconds since the last 0x16, or UINT32_MAX if none has ever
 *               arrived; may be NULL.
 * @retval CRSF_OK              Values are present.
 * @retval CRSF_ERR_NOT_FOUND   No channel frame has arrived yet.
 * @retval CRSF_ERR_INVALID_ARG @p h is NULL.
 *
 * @note Always check the age or crsf_link_is_up(). Stale stick positions are
 *       indistinguishable from live ones otherwise.
 */
crsf_err_t crsf_get_channels(crsf_handle_t h, crsf_channels_t *out, uint32_t *age_ms);

/**
 * @brief Whether channel frames are arriving.
 *
 * True when a 0x16 frame arrived within the failsafe hold time. crsf.md:519
 * notes 0x16 simply stops on failsafe and recommends waiting about a second
 * before acting, which is the default hold.
 *
 * @param h Port handle.
 * @return true while the link is considered up; false for a NULL handle.
 */
bool crsf_link_is_up(crsf_handle_t h);

/**
 * @brief Set the hold time used by crsf_link_is_up().
 *
 * @param h          Port handle.
 * @param timeout_ms Hold time in milliseconds. Default 1000.
 * @retval CRSF_OK              Applied.
 * @retval CRSF_ERR_INVALID_ARG @p h is NULL.
 */
crsf_err_t crsf_set_link_timeout(crsf_handle_t h, uint32_t timeout_ms);

/**
 * @brief Latest received 0x14 Link Statistics.
 *
 * @param h      Port handle.
 * @param out    Receives the statistics; may be NULL if only the age is wanted.
 * @param age_ms Milliseconds since the last 0x14, or UINT32_MAX if none has
 *               arrived; may be NULL.
 * @retval CRSF_OK              Values are present.
 * @retval CRSF_ERR_NOT_FOUND   No 0x14 frame has arrived yet.
 * @retval CRSF_ERR_INVALID_ARG @p h is NULL.
 */
crsf_err_t crsf_get_link_statistics(crsf_handle_t h, crsf_link_statistics_t *out,
                                    uint32_t *age_ms);

/**
 * @brief Parser health counters: bad CRC, bad length, resyncs and so on.
 *
 * @param h   Port handle.
 * @param out Receives a snapshot of the counters.
 * @retval CRSF_OK              Copied.
 * @retval CRSF_ERR_INVALID_ARG @p h or @p out is NULL.
 */
crsf_err_t crsf_get_parser_stats(crsf_handle_t h, crsf_parser_stats_t *out);

/** @} */

/**
 * @name Sending — generic
 * @{
 */

/**
 * @brief Queue a raw frame.
 *
 * Adds sync, length and CRC, plus the extended header when @p type requires one
 * (with our own address as origin). Frames are serialised through the port's TX
 * task, so this is safe to call from several tasks at once.
 *
 * @param h           Port handle.
 * @param type        Frame type.
 * @param destination Extended-header destination; ignored for short-header types.
 * @param payload     Payload bytes; may be NULL when @p payload_len is 0.
 * @param payload_len Payload length.
 * @retval CRSF_OK                 Queued.
 * @retval CRSF_ERR_INVALID_SIZE   The frame would exceed 64 bytes.
 * @retval CRSF_ERR_TIMEOUT        The transmit queue stayed full.
 * @retval CRSF_ERR_INVALID_STATE  The port is closing.
 * @retval CRSF_ERR_INVALID_ARG    @p h is NULL.
 *
 * @note Queued, not sent: a successful return means the frame reached the TX
 *       queue, not the wire.
 */
crsf_err_t crsf_send_frame(crsf_handle_t h, uint8_t type, uint8_t destination,
                           const void *payload, size_t payload_len);

/**
 * @brief Queue a 0x32 Direct Command, including the extra CRC8 (poly 0xBA).
 *
 * @param h           Port handle.
 * @param destination Target device address.
 * @param command_id  Command set, see @ref crsf_command_id_t.
 * @param sub_id      Sub-command within that set.
 * @param args        Argument bytes; may be NULL when @p args_len is 0.
 * @param args_len    Argument length.
 * @return As crsf_send_frame().
 */
crsf_err_t crsf_send_command(crsf_handle_t h, uint8_t destination,
                             uint8_t command_id, uint8_t sub_id,
                             const void *args, size_t args_len);

/**
 * @brief Queue a 0x32.0xFF Command ACK (crsf.md:966).
 *
 * @param h           Port handle.
 * @param destination Where to send the acknowledgement.
 * @param command_id  Command set being acknowledged.
 * @param sub_id      Sub-command being acknowledged.
 * @param acted       true if the command was carried out, false if unsupported —
 *                    crsf.md:972 defines the latter as "arrived but not
 *                    implemented here".
 * @param info        Optional null-terminated info string, may be NULL.
 * @return As crsf_send_frame().
 */
crsf_err_t crsf_send_command_ack(crsf_handle_t h, uint8_t destination,
                                 uint8_t command_id, uint8_t sub_id,
                                 bool acted, const char *info);

/** @} */

/**
 * @name Sending — typed telemetry
 *
 * One function per frame type. Each encodes the struct and queues a broadcast
 * frame, takes a const pointer and leaves the caller's struct untouched. All
 * share crsf_send_frame()'s return values, plus CRSF_ERR_INVALID_ARG when the
 * payload pointer is NULL or the struct cannot be encoded (a count above the
 * frame's maximum, typically).
 *
 * For anything sent repeatedly, prefer crsf_telemetry_publish() so the scheduler
 * paces it against the link budget instead of the application's own timing.
 * @{
 */

/** @brief Send 0x02 GPS. @param h Port handle. @param in Position and velocity. @return As crsf_send_frame(). */
crsf_err_t crsf_send_gps(crsf_handle_t h, const crsf_gps_t *in);

/** @brief Send 0x03 GPS Time. @param h Port handle. @param in UTC timestamp. @return As crsf_send_frame(). */
crsf_err_t crsf_send_gps_time(crsf_handle_t h, const crsf_gps_time_t *in);

/** @brief Send 0x06 GPS Extended. @param h Port handle. @param in Accuracy and velocity detail. @return As crsf_send_frame(). */
crsf_err_t crsf_send_gps_extended(crsf_handle_t h, const crsf_gps_extended_t *in);

/** @brief Send 0x07 Variometer. @param h Port handle. @param in Vertical speed. @return As crsf_send_frame(). */
crsf_err_t crsf_send_vario(crsf_handle_t h, const crsf_vario_t *in);

/** @brief Send 0x08 Battery Sensor. @param h Port handle. @param in Battery state; note the units at @ref crsf_battery_t. @return As crsf_send_frame(). */
crsf_err_t crsf_send_battery(crsf_handle_t h, const crsf_battery_t *in);

/** @brief Send 0x09 Baro Altitude and Vertical Speed. @param h Port handle. @param in Altitude and vertical speed; packed lossily. @return As crsf_send_frame(). */
#if CRSF_ENABLE_FLOAT_MATH
crsf_err_t crsf_send_baro_altitude(crsf_handle_t h, const crsf_baro_altitude_t *in);
#endif

/** @brief Send 0x0A Airspeed. @param h Port handle. @param in Airspeed. @return As crsf_send_frame(). */
crsf_err_t crsf_send_airspeed(crsf_handle_t h, const crsf_airspeed_t *in);

/**
 * @brief Send 0x0B Heartbeat, announcing our own address.
 * @param h Port handle.
 * @return As crsf_send_frame().
 */
crsf_err_t crsf_send_heartbeat(crsf_handle_t h);

/** @brief Send 0x0C RPM. @param h Port handle. @param in Source id and values. @return As crsf_send_frame(). */
crsf_err_t crsf_send_rpm(crsf_handle_t h, const crsf_rpm_t *in);

/** @brief Send 0x0D Temperature. @param h Port handle. @param in Source id and values. @return As crsf_send_frame(). */
crsf_err_t crsf_send_temp(crsf_handle_t h, const crsf_temp_t *in);

/** @brief Send 0x0E Voltages. @param h Port handle. @param in Source id and cell voltages. @return As crsf_send_frame(). */
crsf_err_t crsf_send_voltages(crsf_handle_t h, const crsf_voltages_t *in);

/** @brief Send 0x10 VTX Telemetry. @param h Port handle. @param in VTX state. @return As crsf_send_frame(). */
crsf_err_t crsf_send_vtx_telemetry(crsf_handle_t h, const crsf_vtx_telemetry_t *in);

/** @brief Send 0x11 Barometer. @param h Port handle. @param in Pressure and temperature. @return As crsf_send_frame(). */
crsf_err_t crsf_send_barometer(crsf_handle_t h, const crsf_barometer_t *in);

/** @brief Send 0x12 Magnetometer. @param h Port handle. @param in Field vector. @return As crsf_send_frame(). */
crsf_err_t crsf_send_magnetometer(crsf_handle_t h, const crsf_magnetometer_t *in);

/** @brief Send 0x13 Accel Gyro. @param h Port handle. @param in IMU sample. @return As crsf_send_frame(). */
crsf_err_t crsf_send_accel_gyro(crsf_handle_t h, const crsf_accel_gyro_t *in);

/** @brief Send 0x14 Link Statistics. @param h Port handle. @param in Link quality figures. @return As crsf_send_frame(). */
crsf_err_t crsf_send_link_statistics(crsf_handle_t h, const crsf_link_statistics_t *in);

/** @brief Send 0x1C Link Statistics RX. @param h Port handle. @param in Receiver-side figures. @return As crsf_send_frame(). */
crsf_err_t crsf_send_link_statistics_rx(crsf_handle_t h, const crsf_link_statistics_rx_t *in);

/** @brief Send 0x1D Link Statistics TX. @param h Port handle. @param in Transmitter-side figures. @return As crsf_send_frame(). */
crsf_err_t crsf_send_link_statistics_tx(crsf_handle_t h, const crsf_link_statistics_tx_t *in);

/** @brief Send 0x1E Attitude. @param h Port handle. @param in Angles; keep within ±180°, not clamped. @return As crsf_send_frame(). */
crsf_err_t crsf_send_attitude(crsf_handle_t h, const crsf_attitude_t *in);

/** @brief Send 0x1F MAVLink FC. @param h Port handle. @param in Autopilot mode summary. @return As crsf_send_frame(). */
crsf_err_t crsf_send_mavlink_fc(crsf_handle_t h, const crsf_mavlink_fc_t *in);

/** @brief Send 0xAC MAVLink System Status. @param h Port handle. @param in Sensor bitmasks. @return As crsf_send_frame(). */
crsf_err_t crsf_send_mavlink_status(crsf_handle_t h, const crsf_mavlink_status_t *in);

/**
 * @brief Send 0x21 Flight Mode.
 * @param h    Port handle.
 * @param mode Null-terminated mode name, truncated to
 *             CRSF_FLIGHT_MODE_MAX_LEN - 1 characters.
 * @return As crsf_send_frame().
 */
crsf_err_t crsf_send_flight_mode(crsf_handle_t h, const char *mode);

/** @brief Send 0x22 ESP_NOW. @param h Port handle. @param in Race message. @return As crsf_send_frame(). */
crsf_err_t crsf_send_esp_now(crsf_handle_t h, const crsf_esp_now_t *in);

/** @brief Send 0x34 Logging. @param h Port handle. @param in Event id, timestamp and values. @return As crsf_send_frame(). */
crsf_err_t crsf_send_logging(crsf_handle_t h, const crsf_logging_t *in);

/** @brief Send 0x3C Game. @param h Port handle. @param in Sub-type and its value. @return As crsf_send_frame(). */
crsf_err_t crsf_send_game(crsf_handle_t h, const crsf_game_t *in);

/**
 * @brief Send 0x16 RC channels.
 *
 * @param h        Port handle.
 * @param channels The 16 channel values, in ticks.
 * @retval CRSF_ERR_INVALID_STATE The port is a CRSF_ROLE_RX port — only the
 *                               sending station may drive the channel stream, or
 *                               two masters end up on one wire.
 * @return Otherwise as crsf_send_frame().
 */
crsf_err_t crsf_send_channels(crsf_handle_t h, const crsf_channels_t *channels);

/**
 * @brief Send 0x17 Subset RC Channels.
 *
 * @param h  Port handle.
 * @param ch Channels, resolution and starting channel number.
 * @retval CRSF_ERR_INVALID_STATE The port is a CRSF_ROLE_RX port.
 * @retval CRSF_ERR_INVALID_ARG   The channel count or resolution will not fit one
 *                               frame.
 * @return Otherwise as crsf_send_frame().
 *
 * @warning The spec discourages this frame and has a revision in progress
 *          (crsf.md:550). Prefer crsf_send_channels().
 */
crsf_err_t crsf_send_subset_channels(crsf_handle_t h, const crsf_subset_channels_t *ch);

/**
 * @brief Send a 0x28 Device Ping.
 * @param h           Port handle.
 * @param destination Target address, or 0x00 to broadcast and discover.
 * @return As crsf_send_frame().
 */
crsf_err_t crsf_send_ping(crsf_handle_t h, uint8_t destination);

/**
 * @brief Send our 0x29 Device Info.
 *
 * Built from the identity fields in @ref crsf_port_config_t, with
 * `parameters_total` taken from the attached provider if there is one.
 *
 * @param h           Port handle.
 * @param destination Where to send it.
 * @return As crsf_send_frame().
 */
crsf_err_t crsf_send_device_info(crsf_handle_t h, uint8_t destination);

/**
 * @brief Send a 0x3A.0x10 Timing Correction frame (crsf.md:1173).
 * @param h           Port handle.
 * @param destination Target address.
 * @param t           Update interval and offset, both in units of 100 ns.
 * @return As crsf_send_frame().
 */
crsf_err_t crsf_send_timing_correction(crsf_handle_t h, uint8_t destination,
                                       const crsf_timing_correction_t *t);

/** @} */

/**
 * @name Telemetry scheduler
 *
 * crsf.md:132 requires the frame rate to suit the baudrate, so that a full
 * 64-byte frame fits inside one frame period. Publishing a value and letting the
 * TX task pace it keeps application timing away from link timing, and lets a
 * peer throttle us with 0x32.0x20 Flow Control (crsf.md:1097).
 * @{
 */

/* CRSF_SCHED_MAX_SLOTS is configuration; see crsf_conf.h. It sizes
 * crsf_port::slots, at 75 bytes a slot. */

/**
 * @brief Publish the latest payload for @p type, to be sent on its interval.
 *
 * Replaces any previous payload for that type: the scheduler always sends the
 * newest value rather than a backlog. Publishing does not send anything by
 * itself — set an interval with crsf_telemetry_set_interval() as well.
 *
 * This is the raw form, for frame types this component has no encoder for —
 * 0x80 ArduPilot passthrough, 0x81/0x82 mLRS, or a vendor frame of your own. For
 * anything with a struct, prefer the typed publishers below: they encode for you
 * and the compiler checks that the struct matches the frame type.
 *
 * @param h       Port handle.
 * @param type    Frame type this payload belongs to.
 * @param payload Encoded payload, e.g. from crsf_encode_battery(); copied.
 * @param len     Payload length, 1 .. CRSF_MAX_PAYLOAD_SIZE.
 * @retval CRSF_OK              Stored.
 * @retval CRSF_ERR_INVALID_ARG @p h is NULL, @p type is 0, @p payload is NULL, or
 *                             @p len is 0 or above CRSF_MAX_PAYLOAD_SIZE.
 * @retval CRSF_ERR_NO_MEM      All CRSF_SCHED_MAX_SLOTS slots are in use by other
 *                             types.
 *
 * @note A @p len of 0 is refused. Every crsf_encode_* returns 0 to report that it
 *       could not represent its input, and passing that through as a length would
 *       schedule an empty frame forever. Use crsf_send_frame() if you genuinely
 *       need to emit a single payload-less frame.
 */
crsf_err_t crsf_telemetry_publish(crsf_handle_t h, uint8_t type,
                                  const void *payload, size_t len);

/**
 * @brief Set the send interval for a scheduled frame type.
 *
 * @param h           Port handle.
 * @param type        Frame type to pace.
 * @param interval_ms Interval in milliseconds; 0 disables scheduling for that
 *                    type and frees its slot.
 * @retval CRSF_OK              Applied.
 * @retval CRSF_ERR_INVALID_ARG @p h is NULL or @p type is 0.
 * @retval CRSF_ERR_NO_MEM      No free slot, and @p interval_ms is non-zero.
 *
 * @note The real resolution is the FreeRTOS tick period, not 1 ms: the TX task
 *       cannot wake more often than once per tick, which is 10 ms at the ESP-IDF
 *       default of 100 Hz. Intervals below that are served every tick. Raise
 *       CONFIG_FREERTOS_HZ if you need finer pacing.
 */
crsf_err_t crsf_telemetry_set_interval(crsf_handle_t h, uint8_t type,
                                       uint32_t interval_ms);

/**
 * @brief Bytes per second the current baudrate allows, at 8N1 (10 bits/byte).
 *
 * Exposed so an application can size its own telemetry set against the link.
 * Nothing enforces the budget — over-subscribing simply backs the queue up.
 *
 * @param h Port handle.
 * @return Bytes per second, or 0 for a NULL handle.
 */
uint32_t crsf_link_byte_budget(crsf_handle_t h);

/** @} */

/**
 * @name Telemetry — typed publishing
 *
 * The scheduler counterpart of the typed senders above: each takes the same
 * struct its `crsf_send_*` twin takes, encodes it, and stores the result for the
 * frame type it belongs to. Set a cadence with crsf_telemetry_set_interval() as
 * well — publishing alone sends nothing.
 *
 * ~~~{.c}
 * crsf_telemetry_set_interval(crsf, CRSF_TYPE_BATTERY, 200);
 * crsf_publish_battery(crsf, &battery);   // no buffer, no length, no encoder
 * ~~~
 *
 * Prefer these over the raw crsf_telemetry_publish(): the frame type is implied
 * by the function, so a struct that does not belong to it is a compile error
 * rather than a wrong-looking frame on the wire.
 *
 * All share crsf_telemetry_publish()'s return values, plus CRSF_ERR_INVALID_ARG
 * when the struct pointer is NULL or the struct cannot be encoded — a count above
 * the frame's maximum, typically.
 *
 * @note The RC channel frames 0x16 and 0x17 deliberately have no publisher. The
 *       scheduler resolves only to the FreeRTOS tick (see the note on
 *       crsf_telemetry_set_interval()), which would add quantisation jitter to
 *       the control stream; drive it with crsf_send_channels() on your own timer
 *       instead. The addressed frames — 0x28, 0x29, 0x32, 0x3A — have none
 *       either, because the scheduler only broadcasts.
 * @{
 */

/** @brief Publish 0x02 GPS. @param h Port handle. @param in Position and velocity. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_gps(crsf_handle_t h, const crsf_gps_t *in);

/** @brief Publish 0x03 GPS Time. @param h Port handle. @param in UTC timestamp. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_gps_time(crsf_handle_t h, const crsf_gps_time_t *in);

/** @brief Publish 0x06 GPS Extended. @param h Port handle. @param in Accuracy and velocity detail. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_gps_extended(crsf_handle_t h, const crsf_gps_extended_t *in);

/** @brief Publish 0x07 Variometer. @param h Port handle. @param in Vertical speed. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_vario(crsf_handle_t h, const crsf_vario_t *in);

/** @brief Publish 0x08 Battery Sensor. @param h Port handle. @param in Battery state; note the units at @ref crsf_battery_t. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_battery(crsf_handle_t h, const crsf_battery_t *in);

/** @brief Publish 0x09 Baro Altitude and Vertical Speed. @param h Port handle. @param in Altitude and vertical speed; packed lossily. @return As crsf_telemetry_publish(). */
#if CRSF_ENABLE_FLOAT_MATH
crsf_err_t crsf_publish_baro_altitude(crsf_handle_t h, const crsf_baro_altitude_t *in);
#endif

/** @brief Publish 0x0A Airspeed. @param h Port handle. @param in Airspeed. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_airspeed(crsf_handle_t h, const crsf_airspeed_t *in);

/**
 * @brief Publish 0x0B Heartbeat, announcing our own address.
 * @param h Port handle.
 * @return As crsf_telemetry_publish().
 */
crsf_err_t crsf_publish_heartbeat(crsf_handle_t h);

/** @brief Publish 0x0C RPM. @param h Port handle. @param in Source id and values. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_rpm(crsf_handle_t h, const crsf_rpm_t *in);

/** @brief Publish 0x0D Temperature. @param h Port handle. @param in Source id and values. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_temp(crsf_handle_t h, const crsf_temp_t *in);

/** @brief Publish 0x0E Voltages. @param h Port handle. @param in Source id and cell voltages. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_voltages(crsf_handle_t h, const crsf_voltages_t *in);

/** @brief Publish 0x10 VTX Telemetry. @param h Port handle. @param in VTX state. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_vtx_telemetry(crsf_handle_t h, const crsf_vtx_telemetry_t *in);

/** @brief Publish 0x11 Barometer. @param h Port handle. @param in Pressure and temperature. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_barometer(crsf_handle_t h, const crsf_barometer_t *in);

/** @brief Publish 0x12 Magnetometer. @param h Port handle. @param in Field vector. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_magnetometer(crsf_handle_t h, const crsf_magnetometer_t *in);

/** @brief Publish 0x13 Accel Gyro. @param h Port handle. @param in IMU sample. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_accel_gyro(crsf_handle_t h, const crsf_accel_gyro_t *in);

/** @brief Publish 0x14 Link Statistics. @param h Port handle. @param in Link quality figures. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_link_statistics(crsf_handle_t h, const crsf_link_statistics_t *in);

/** @brief Publish 0x1C Link Statistics RX. @param h Port handle. @param in Receiver-side figures. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_link_statistics_rx(crsf_handle_t h, const crsf_link_statistics_rx_t *in);

/** @brief Publish 0x1D Link Statistics TX. @param h Port handle. @param in Transmitter-side figures. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_link_statistics_tx(crsf_handle_t h, const crsf_link_statistics_tx_t *in);

/** @brief Publish 0x1E Attitude. @param h Port handle. @param in Angles; keep within ±180°, not clamped. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_attitude(crsf_handle_t h, const crsf_attitude_t *in);

/** @brief Publish 0x1F MAVLink FC. @param h Port handle. @param in Autopilot mode summary. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_mavlink_fc(crsf_handle_t h, const crsf_mavlink_fc_t *in);

/** @brief Publish 0xAC MAVLink System Status. @param h Port handle. @param in Sensor bitmasks. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_mavlink_status(crsf_handle_t h, const crsf_mavlink_status_t *in);

/**
 * @brief Publish 0x21 Flight Mode.
 * @param h    Port handle.
 * @param mode Null-terminated mode name, truncated to
 *             CRSF_FLIGHT_MODE_MAX_LEN - 1 characters.
 * @return As crsf_telemetry_publish().
 */
crsf_err_t crsf_publish_flight_mode(crsf_handle_t h, const char *mode);

/** @brief Publish 0x22 ESP_NOW. @param h Port handle. @param in Race message. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_esp_now(crsf_handle_t h, const crsf_esp_now_t *in);

/** @brief Publish 0x34 Logging. @param h Port handle. @param in Event id, timestamp and values. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_logging(crsf_handle_t h, const crsf_logging_t *in);

/** @brief Publish 0x3C Game. @param h Port handle. @param in Sub-type and its value. @return As crsf_telemetry_publish(). */
crsf_err_t crsf_publish_game(crsf_handle_t h, const crsf_game_t *in);

/** @} */

/**
 * @name Tunnels
 * @{
 */

#if CRSF_ENABLE_MAVLINK

/**
 * @brief Send a whole MAVLink frame, split across 0xAA envelopes as needed.
 *
 * @param h           Port handle.
 * @param destination Target address.
 * @param frame       The MAVLink frame; copied chunk by chunk.
 * @param len         Its length, up to CRSF_MAVLINK_FRAME_MAX (crsf.md:1297).
 * @retval CRSF_ERR_INVALID_ARG  @p frame is NULL or @p len is 0.
 * @retval CRSF_ERR_INVALID_SIZE @p len would need more than the 16 chunks a 4-bit
 *                              index can express (crsf.md:1305).
 * @return Otherwise as crsf_send_frame(); a partial send is possible if the
 *         queue fills mid-frame.
 *
 * @note Header form follows crsf_config_t::mavlink_envelope_extended_header.
 */
crsf_err_t crsf_mavlink_send(crsf_handle_t h, uint8_t destination,
                             const uint8_t *frame, size_t len);

/**
 * @brief Called when a complete MAVLink frame has been reassembled.
 *
 * @param h     The port it arrived on.
 * @param frame The reassembled frame; valid only for this call.
 * @param len   Its length, always non-zero.
 * @param ctx   The context given to crsf_mavlink_on_frame().
 */
typedef void (*crsf_mavlink_cb_t)(crsf_handle_t h, const uint8_t *frame, size_t len,
                                  void *ctx);

/**
 * @brief Register the MAVLink reassembly callback.
 *
 * @param h   Port handle.
 * @param cb  Callback, or NULL to stop reassembling.
 * @param ctx Passed back to @p cb unchanged.
 * @retval CRSF_OK              Registered.
 * @retval CRSF_ERR_INVALID_ARG @p h is NULL.
 */
crsf_err_t crsf_mavlink_on_frame(crsf_handle_t h, crsf_mavlink_cb_t cb, void *ctx);

#endif /* CRSF_ENABLE_MAVLINK */

#if CRSF_ENABLE_MSP

/**
 * @brief Send an MSP body, split across 0x7A or 0x7B frames as needed.
 *
 * @param h           Port handle.
 * @param destination Target address. For a response, pass the @c origin from
 *                    @ref crsf_msp_cb_t and the address swap crsf.md:1228
 *                    requires happens automatically.
 * @param body        MSP frame stripped of its `$M<`-style header and CRC
 *                    (crsf.md:1216).
 * @param len         Length of @p body, up to CRSF_MSP_BODY_MAX.
 * @param version     1 or 2.
 * @param is_response true to send as 0x7B, false as 0x7A.
 * @retval CRSF_ERR_INVALID_ARG @p len is 0 or above CRSF_MSP_BODY_MAX.
 * @return Otherwise as crsf_send_frame().
 */
crsf_err_t crsf_msp_send(crsf_handle_t h, uint8_t destination, const uint8_t *body,
                         size_t len, uint8_t version, bool is_response);

/**
 * @brief Called when a complete MSP body has been reassembled.
 *
 * @param h           The port it arrived on.
 * @param origin      Where it came from. crsf.md:1228 requires a response to go
 *                    back to this address with destination and origin swapped,
 *                    which crsf_msp_send() does when given this value.
 * @param is_response true when it arrived as 0x7B rather than 0x7A.
 * @param body        The reassembled MSP body; valid only for this call.
 * @param len         Its length.
 * @param version     MSP version from the status byte.
 * @param error       Error flag from the status byte (crsf.md:1223).
 * @param ctx         The context given to crsf_msp_on_frame().
 */
typedef void (*crsf_msp_cb_t)(crsf_handle_t h, uint8_t origin, bool is_response,
                              const uint8_t *body, size_t len, uint8_t version,
                              bool error, void *ctx);

/**
 * @brief Register the MSP reassembly callback.
 *
 * @param h   Port handle.
 * @param cb  Callback, or NULL to stop reassembling.
 * @param ctx Passed back to @p cb unchanged.
 * @retval CRSF_OK              Registered.
 * @retval CRSF_ERR_INVALID_ARG @p h is NULL.
 */
crsf_err_t crsf_msp_on_frame(crsf_handle_t h, crsf_msp_cb_t cb, void *ctx);

#endif /* CRSF_ENABLE_MSP */

/** @} */

/**
 * @name Parameter protocol
 * @{
 */

#if CRSF_ENABLE_PARAMS

/**
 * @brief Serve a parameter tree on this port (device side).
 *
 * Once attached, the port answers 0x2C reads and 0x2D writes by itself, so the
 * device shows up in a handset's configuration menu. The provider's own
 * `parameters_total` is then reported in the 0x29 ping reply.
 *
 * @param h        Port handle.
 * @param provider An initialised provider, or NULL to stop serving. Borrowed —
 *                 must stay valid until the port is closed or NULL is attached.
 * @retval CRSF_OK              Attached.
 * @retval CRSF_ERR_INVALID_ARG @p h is NULL.
 */
crsf_err_t crsf_params_attach_provider(crsf_handle_t h, crsf_param_provider_t *provider);

/**
 * @brief Called for each parameter discovered during a walk.
 *
 * @param h      The port the walk runs on.
 * @param device Address of the device being walked.
 * @param entry  The decoded entry; valid only for this call.
 * @param ctx    The context given to crsf_params_walk().
 */
typedef void (*crsf_param_entry_cb_t)(crsf_handle_t h, uint8_t device,
                                      const crsf_param_entry_t *entry, void *ctx);

/**
 * @brief Called when a walk finishes, successfully or not.
 *
 * @param h      The port the walk ran on.
 * @param device Address of the device that was walked.
 * @param ok     true when the list was walked to its end; false when the device
 *               stopped answering.
 * @param ctx    The context given to crsf_params_walk().
 */
typedef void (*crsf_param_done_cb_t)(crsf_handle_t h, uint8_t device, bool ok,
                                     void *ctx);

/**
 * @brief Walk a remote device's parameter tree (host side).
 *
 * Sends the 0x2C requests, reassembles the 0x2B chunks and reports each entry.
 * Returns immediately; the work proceeds on the port's TX task.
 *
 * @param h        Port handle.
 * @param device   Address of the device to interrogate.
 * @param total    `Parameters_total` from its 0x29 reply, or 0 if unknown — the
 *                 walk then relies on OUT_OF_RANGE to find the end (crsf.md:754).
 * @param on_entry Called per parameter; may be NULL.
 * @param on_done  Called once at the end; may be NULL.
 * @param ctx      Passed to both callbacks.
 * @retval CRSF_OK                Walk started.
 * @retval CRSF_ERR_INVALID_STATE A walk is already running on this port; only one
 *                               runs at a time.
 * @retval CRSF_ERR_INVALID_ARG   @p h is NULL.
 */
crsf_err_t crsf_params_walk(crsf_handle_t h, uint8_t device, uint8_t total,
                            crsf_param_entry_cb_t on_entry,
                            crsf_param_done_cb_t on_done, void *ctx);

/**
 * @brief Write a FLOAT parameter on a remote device.
 * @param h      Port handle.
 * @param device Target device address.
 * @param number Parameter number.
 * @param value  New value, scaled by the parameter's decimal point.
 * @return As crsf_send_frame().
 */
crsf_err_t crsf_params_write_float(crsf_handle_t h, uint8_t device, uint8_t number,
                                   int32_t value);

/**
 * @brief Write a TEXT_SELECTION parameter on a remote device.
 * @param h      Port handle.
 * @param device Target device address.
 * @param number Parameter number.
 * @param index  Index into the parameter's option list.
 * @return As crsf_send_frame().
 */
crsf_err_t crsf_params_write_selection(crsf_handle_t h, uint8_t device,
                                       uint8_t number, uint8_t index);

/**
 * @brief Write a STRING parameter on a remote device.
 * @param h      Port handle.
 * @param device Target device address.
 * @param number Parameter number.
 * @param value  Null-terminated replacement text.
 * @return As crsf_send_frame(), or CRSF_ERR_INVALID_ARG when @p value is NULL.
 */
crsf_err_t crsf_params_write_string(crsf_handle_t h, uint8_t device, uint8_t number,
                                    const char *value);

/**
 * @brief Drive a COMMAND parameter on a remote device (crsf.md:847).
 *
 * @param h      Port handle.
 * @param device Target device address.
 * @param number Parameter number of the COMMAND.
 * @param status One of START, CONFIRM, CANCEL or POLL. crsf.md:889 notes POLL is
 *               required to see progress updates.
 * @return As crsf_send_frame().
 */
crsf_err_t crsf_params_command(crsf_handle_t h, uint8_t device, uint8_t number,
                               crsf_cmd_status_t status);

#endif /* CRSF_ENABLE_PARAMS */

/** @} */

/**
 * @name Routing
 * @{
 */

#if CRSF_ENABLE_ROUTER

/**
 * @brief Attach this port to a router as port index @p port_index.
 *
 * Every attached port then forwards frames to the others per crsf.md:181.
 *
 * @param h          Port handle.
 * @param router     An initialised router; borrowed, must outlive all attached
 *                   ports.
 * @param port_index This port's index, below the router's port count.
 * @retval CRSF_OK                Attached.
 * @retval CRSF_ERR_INVALID_ARG   @p h or @p router is NULL, @p port_index is out
 *                               of range for the router, or the router's
 *                               self_address differs from this port's.
 * @retval CRSF_ERR_INVALID_STATE Another handle already occupies @p port_index.
 *
 * @note The router's @c self_address must equal this port's own address. They
 *       are consulted by different code -- the router decides what to consume
 *       locally, while auto_respond_ping and the frame callbacks use the port's
 *       -- so a mismatch silently stops frames addressed to this port from
 *       being handled here. Pass the same address to crsf_router_init() as the
 *       port resolves to, or leave both at their defaults.
 *
 * @warning Loop-free wiring is the integrator's responsibility; see the warning
 *          in crsf_router.h.
 */
crsf_err_t crsf_router_attach(crsf_handle_t h, crsf_router_t *router,
                              uint8_t port_index);

/**
 * @brief Detach this port from its router.
 *
 * @param h Port handle.
 * @retval CRSF_OK              Detached, or it was not attached.
 * @retval CRSF_ERR_INVALID_ARG @p h is NULL.
 */
crsf_err_t crsf_router_detach(crsf_handle_t h);

#endif /* CRSF_ENABLE_ROUTER */

/** @} */

/**
 * @name Baudrate negotiation (crsf.md:1052)
 * @{
 */

/**
 * @brief Propose a new baudrate to @p destination via 0x32.0x0A.0x70.
 *
 * We switch only after the peer accepts with 0x71, and fall back to the previous
 * baudrate if no valid frame arrives afterwards.
 *
 * @param h           Port handle.
 * @param destination Peer to negotiate with.
 * @param baudrate    Proposed baudrate.
 * @return As crsf_send_frame(). Success means the proposal was queued, not that
 *         the peer accepted — the switch happens later, on the TX task.
 *
 * @note The spec leaves initiator, switch timing and fallback unspecified
 *       (crsf.md:1052-1062); this is our policy, not a requirement. Unverified
 *       against real hardware — see COMPLIANCE.md §6.
 */
crsf_err_t crsf_propose_baudrate(crsf_handle_t h, uint8_t destination,
                                 uint32_t baudrate);

/**
 * @brief Baudrate currently in use.
 * @param h Port handle.
 * @return Baudrate, or 0 for a NULL handle.
 */
uint32_t crsf_get_baudrate(crsf_handle_t h);

/**
 * @brief Our own device address on this port.
 * @param h Port handle.
 * @return The address used as the origin of extended frames, or 0 for a NULL
 *         handle.
 */
uint8_t crsf_self_address(crsf_handle_t h);

/** @} */

/**
 * @name Port state
 *
 * Public so a caller can place a port in static storage, not so its fields can
 * be read. Treat every one of them as private; they change without notice.
 * @{
 */

/* CRSF_MAX_CALLBACKS is configuration; see crsf_conf.h. It sizes
 * crsf_port::callbacks, at 5 bytes an entry. */

/** One registered frame callback. */
typedef struct {
    uint8_t type;       /**< frame type to match, or 0 for every frame */
    crsf_frame_cb_t cb; /**< NULL marks the slot free */
    void *ctx;          /**< opaque context handed back to @ref cb */
} crsf_cb_entry_t;

/** A scheduled telemetry frame: newest payload plus its cadence. */
typedef struct {
    uint8_t type;                           /**< frame type; 0 means the slot is free */
    uint32_t interval_ms;                   /**< send cadence; 0 disables without freeing the slot */
    int64_t last_sent_us;                   /**< when it last went out; 0 means never */
    uint8_t len;                            /**< payload length in @ref payload */
    bool valid;                             /**< a payload has been published at least once */
    uint8_t payload[CRSF_MAX_PAYLOAD_SIZE]; /**< newest payload, overwritten */
} crsf_sched_slot_t;

/**
 * @brief Everything one CRSF port owns.
 *
 * Visible so a port can be placed in static storage without an allocator, and
 * for no other reason: treat every field as private. Nothing outside the
 * library reads or writes one, and the layout is free to change.
 *
 * The fields fall into three groups. Some are fixed by crsf_port_init() and
 * never change afterwards. Most are guarded by the lock the platform supplied,
 * which the library takes around every read and write of them. A few are
 * @c volatile flags written in one context and read in another without it,
 * each documented where it is declared.
 */
struct crsf_port {
    uint32_t baudrate;    /**< baudrate currently applied to the link */
    uint8_t self_address; /**< origin address stamped into extended frames */

    /*
     * The three configuration values the protocol layer itself reads. The rest
     * of crsf_config_t describes a UART and belongs to the port, not here.
     */
    crsf_role_t role;                      /**< which end of the link this is */
    bool auto_respond_ping;                /**< answer 0x28 with 0x29 */
    bool mavlink_envelope_extended_header; /**< 0xAA carries dest and origin */

    /*
     * How the protocol layer reaches the platform. Every call it makes
     * outwards -- transmit, clock, lock, baudrate, flush, events -- goes
     * through these, and through nothing else.
     */
    const crsf_io_ops_t *ops; /**< platform hooks; see crsf_io.h */
    void *io_ctx;             /**< context handed to every op */

    crsf_parser_t parser; /**< inbound byte-stream state, RX task only */

    /**
     * A baudrate change asks for the parser to be resynchronised, but it
     * happens on the TX task and @ref parser belongs to the RX task. Resetting
     * it from here would race the RX task mid-frame: crsf_parser_reset() zeroes
     * `expected` while the RX task is between storing a body byte and checking
     * the CRC, and the CRC check then computes `expected - 1` as a size_t,
     * i.e. SIZE_MAX, and reads far past the parser. So the TX task only asks,
     * and the RX task performs the reset between chunks.
     */
    volatile bool parser_reset_pending;

    /*
     * Cleared before a port is torn down, so a send attempted during shutdown
     * is refused rather than queued onto something about to be freed. The ESP
     * port's tasks watch the same flag to know when to finish.
     */
    volatile bool running; /**< false once teardown has begun */

    /* --- received snapshots --- */
    crsf_channels_t channels; /**< latest 0x16 values */
    int64_t channels_us;      /**< when they arrived; 0 means never */
    uint32_t link_timeout_ms; /**< failsafe hold for crsf_link_is_up() */

    crsf_link_statistics_t link_stats; /**< latest 0x14 values */
    int64_t link_stats_us;             /**< when they arrived; 0 means never */

    crsf_device_info_t device_info; /**< our own identity, for 0x29 replies */

    crsf_cb_entry_t callbacks[CRSF_MAX_CALLBACKS]; /**< frame callbacks */
    crsf_sched_slot_t slots[CRSF_SCHED_MAX_SLOTS]; /**< telemetry scheduler */

    /* --- tunnels --- */
#if CRSF_ENABLE_MAVLINK
    crsf_mavlink_reasm_t mavlink_reasm; /**< inbound 0xAA reassembly */
    crsf_mavlink_cb_t mavlink_cb;       /**< called on a complete frame */
    void *mavlink_ctx;                  /**< context for @ref mavlink_cb */

    /**
     * Completed frames are copied out of the reassembler here so the callback
     * can run with the mutex released. Per port, not function-scope static:
     * h->lock only serialises one port, so a shared buffer let a second port's
     * RX task overwrite this one's frame between the copy and the callback.
     */
    uint8_t mavlink_assembled[CRSF_MAVLINK_FRAME_MAX];
#endif /* CRSF_ENABLE_MAVLINK */

#if CRSF_ENABLE_MSP
    crsf_msp_reasm_t msp_reasm; /**< inbound 0x7A/0x7B reassembly */
    uint8_t msp_origin;         /**< origin of the frame being reassembled */
    bool msp_is_response;       /**< it arrived as 0x7B rather than 0x7A */
    crsf_msp_cb_t msp_cb;       /**< called on a complete body */
    void *msp_ctx;              /**< context for @ref msp_cb */

    /** Per-port copy of a completed MSP body; see @ref mavlink_assembled. */
    uint8_t msp_assembled[CRSF_MSP_BODY_MAX];
#endif /* CRSF_ENABLE_MSP */

    /* --- parameters --- */
#if CRSF_ENABLE_PARAMS
    crsf_param_provider_t *provider; /**< attached tree, or NULL */

    bool walk_active;                    /**< a host-side walk is in progress */
    crsf_param_client_t walk;            /**< its state */
    crsf_param_entry_cb_t walk_entry_cb; /**< called per discovered parameter */
    crsf_param_done_cb_t walk_done_cb;   /**< called once the walk ends */
    void *walk_ctx;                      /**< context for both walk callbacks */
    int64_t walk_sent_us;                /**< when the outstanding 0x2C went out */
    uint8_t walk_retries;                /**< retries spent on the current chunk */
#endif                                   /* CRSF_ENABLE_PARAMS */

    /* --- routing --- */
#if CRSF_ENABLE_ROUTER
    crsf_router_t *router; /**< attached router, or NULL */
    uint8_t router_port;   /**< our index within that router */
#endif                     /* CRSF_ENABLE_ROUTER */

    /* --- baudrate negotiation --- */
    uint32_t pending_baudrate;  /**< agreed rate, applied after the reply is sent */
    uint32_t previous_baudrate; /**< rate to fall back to; 0 when not switching */
    int64_t baud_switch_us;     /**< when the switch happened, for the timeout */
    /**
     * parser.stats.frames_ok as it stood at the switch. The fallback compares
     * against this: the running total is cumulative since crsf_parser_init()
     * and so is always non-zero by then, which made the old `frames_ok > 0`
     * test unconditionally true.
     */
    uint32_t baud_switch_frames_ok;
};

/** @} */

/**
 * @name Driving a port
 *
 * What a platform port calls. Everything else in this header is what the
 * application calls.
 * @{
 */

/**
 * @brief What a port needs to know about the link it is on.
 *
 * Zero-initialise and set what you need: every field documented with a default
 * treats 0 as "pick it for me". None of it describes hardware -- a UART number,
 * a pin or a buffer size belongs to the platform port, not here.
 */
typedef struct {
    crsf_role_t role;     /**< which end of the link this port is */
    crsf_wiring_t wiring; /**< selects the default baudrate */
    uint32_t baudrate;    /**< 0 selects the default for @ref wiring */

    /**
     * Our own address, stamped as the origin of extended frames. 0 picks a
     * default from the role: 0xEE (transmitter) for TX, 0xEC (receiver) for RX.
     */
    uint8_t self_address;

    /* --- identity, reported in the 0x29 reply to a 0x28 ping --- */
    const char *device_name; /**< borrowed for the call only; NULL yields "crsf" */
    uint32_t serial_number;  /**< free-form device serial */
    uint32_t hardware_id;    /**< hardware revision */
    uint32_t firmware_id;    /**< firmware revision */

    /** Answer 0x28 Device Ping with 0x29 Device Info automatically. */
    bool auto_respond_ping;

    /**
     * Send 0xAA MAVLink envelopes with an extended header (destination and
     * origin) instead of the short header. See crsf_config_t in a platform port
     * for why the specification leaves this open.
     */
    bool mavlink_envelope_extended_header;

    /** Failsafe hold for crsf_link_is_up(); 0 selects 1000 ms. */
    uint32_t link_timeout_ms;
} crsf_port_config_t;

/**
 * @brief Bind a port to its platform and make it ready to run.
 *
 * Zeroes the port, copies the configuration, clears the reassemblers and points
 * the frame parser at the protocol dispatcher. Call once, before anything else.
 *
 * The strings in @p cfg are copied, not borrowed, so they need not outlive the
 * call.
 *
 * @param h   Port to initialise. Need not be zeroed first; this does it.
 * @param ops Platform hooks. @c tx_push and @c now_us are required.
 * @param ctx Passed back to every hook, unchanged.
 * @param cfg Configuration; copied.
 * @retval CRSF_OK              Ready.
 * @retval CRSF_ERR_INVALID_ARG A required argument or hook is missing.
 */
crsf_err_t crsf_port_init(crsf_handle_t h, const crsf_io_ops_t *ops, void *ctx,
                          const crsf_port_config_t *cfg);

/**
 * @brief Hand received bytes to the protocol layer.
 *
 * Runs the parser and, for each complete frame, the protocol services and the
 * application's callbacks. It also transmits -- replies, acknowledgements and
 * forwarded frames all leave from here.
 *
 * @warning Not callable from an interrupt. See the context rules above.
 *
 * @param h    Port handle.
 * @param data Bytes as they arrived; may contain part of a frame, several
 *             frames, or noise.
 * @param len  Number of bytes.
 */
void crsf_port_feed(crsf_handle_t h, const uint8_t *data, size_t len);

/**
 * @brief Resynchronise the parser.
 *
 * For a port that has just lost bytes -- a receive overrun, a break, a framing
 * error. Whatever was half-collected is meaningless and is discarded.
 *
 * @param h Port handle.
 */
void crsf_port_reset_parser(crsf_handle_t h);

/**
 * @brief Perform a parser reset the protocol layer asked for, if one is due.
 *
 * A baudrate change needs the parser resynchronised, but it is decided during
 * the periodic tick while the parser belongs to whatever feeds it. So the tick
 * only asks, and the receiving side performs it between chunks. Call this
 * before handing over bytes.
 *
 * @param h Port handle.
 * @return true if a reset was performed.
 */
bool crsf_port_poll_reset(crsf_handle_t h);

/**
 * @brief Run the periodic work: telemetry scheduler, parameter walk, baudrate.
 *
 * Call regularly. The telemetry scheduler cannot resolve finer than the
 * interval between calls, so a 5 ms period suits the default cadences; the
 * parameter walk and the baudrate fallback are happy with far less.
 *
 * @param h Port handle.
 */
void crsf_port_tick(crsf_handle_t h);

/**
 * @brief Detach the port from its router and make sure nothing still points here.
 *
 * Call from a port's teardown, before the memory goes away. Another port
 * forwarding to a stale entry would otherwise transmit into freed storage.
 *
 * @param h Port handle.
 */
void crsf_port_leave_router(crsf_handle_t h);

/**
 * @brief Tell the protocol layer a frame has physically left the wire.
 *
 * The only point at which a negotiated baudrate may safely be applied: any
 * earlier would change the line rate underneath our own reply.
 *
 * @param h Port handle.
 */
void crsf_port_after_tx(crsf_handle_t h);

/** @} */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CRSF_CORE_H */
