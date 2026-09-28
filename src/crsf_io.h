/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_io.h
 * @brief Everything the protocol layer needs from the platform underneath it.
 *
 * Seven function pointers. A platform that supplies them gets the whole of CRSF
 * -- the telemetry scheduler, the parameter protocol, both tunnels, 0x32
 * commands, routing and baudrate negotiation -- without writing any of it.
 *
 * @par What is deliberately not here
 * There is no byte-level write. The outbound entry point is @ref
 * crsf_io_ops_t::tx_push and it takes a whole frame, because the layer above it
 * relies on exactly one thing being on the wire at a time: that is what
 * serialises transmission without a second lock, what makes half-duplex echo
 * suppression possible, and what gives baudrate negotiation a defined moment at
 * which the old rate is finished with. A byte sink would transmit on whichever
 * task happened to call @c crsf_publish_battery(), and all three would be lost.
 *
 * There is also no @c printf-style log hook. It would drag @c <stdarg.h> and a
 * formatting implementation into every port -- eight or twelve kilobytes of
 * flash on a small MCU, for eight messages. @ref crsf_event_t carries the same
 * information as an enum plus two numbers, and a port renders it however it
 * already renders anything.
 *
 * @par The four contracts
 * These are not style preferences. Each one was paid for once already.
 *
 * 1. **@c tx_push must not block indefinitely.** It is called from inside the
 *    periodic tick and from every @c crsf_send_* call. A sink that waits for the
 *    wire turns the telemetry scheduler into the application's latency budget.
 *    Returning @c false when there is no room is correct and expected; the
 *    caller reports it as ::CRSF_ERR_TIMEOUT.
 * 2. **The lock is never held across an application callback**, and never across
 *    @c tx_push. The layer above guarantees this; a port must not add an outer
 *    lock of its own that would break it. A non-recursive mutex taken twice on
 *    one thread is a permanent deadlock, and that is not hypothetical here.
 * 3. **@c lock and @c unlock may be NULL**, meaning single-threaded. A port
 *    whose API calls and tick all run in one context pays nothing.
 * 4. **@c set_baudrate is called only from the periodic tick**, never while
 *    feeding received bytes, so a port may safely stop and re-arm its receiver
 *    inside it.
 */

#ifndef CRSF_IO_H
#define CRSF_IO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Something worth reporting, in place of a formatted log line.
 *
 * The two @c uint32_t arguments mean different things per event and are
 * documented with each. A port may ignore all of this; the hook is optional.
 */
typedef enum {
    /** A 0x32 frame failed its second CRC and was dropped. No arguments. */
    CRSF_EV_COMMAND_BAD_CRC = 0,
    /** A peer's baudrate proposal was accepted. @c a is the rate. */
    CRSF_EV_BAUD_PROPOSAL_ACCEPTED,
    /** The acceptance could not be queued. @c a is the error, @c b the rate kept. */
    CRSF_EV_BAUD_ACCEPT_SEND_FAILED,
    /** A peer answered our proposal. @c a is non-zero if it accepted. */
    CRSF_EV_BAUD_PEER_REPLY,
    /** A negotiated rate is now in force. @c a is the rate. */
    CRSF_EV_BAUD_APPLIED,
    /** No traffic after a change; reverting. @c a is the rate reverted to. */
    CRSF_EV_BAUD_REVERTING,
    /** The revert itself failed. @c a is the error. */
    CRSF_EV_BAUD_REVERT_FAILED,
    /** A router's address does not match the port's. @c a router, @c b port. */
    CRSF_EV_ROUTER_ADDRESS_MISMATCH,
} crsf_event_t;

/**
 * @brief The platform functions the protocol layer calls.
 *
 * @c tx_push and @c now_us are required; the rest may be NULL.
 */
typedef struct {
    /**
     * @brief Accept one complete frame for transmission.
     *
     * @param ctx   The context given alongside these ops.
     * @param frame Complete frame, sync byte through CRC. Copy it; it does not
     *              outlive the call.
     * @param len   Frame length, at most @c CRSF_MAX_FRAME_SIZE.
     * @return true if it was accepted, false if there was no room.
     */
    bool (*tx_push)(void *ctx, const uint8_t *frame, size_t len);

    /**
     * @brief Monotonic microsecond clock.
     *
     * Signed, and **zero is reserved**: the layer above stores 0 to mean "this
     * never happened" and computes every age as a signed difference.
     *
     * So this must never return 0, and a port whose underlying counter starts
     * there has to offset it. Returning 0 makes every timestamp written in that
     * instant read back as "never", which makes the telemetry scheduler emit
     * again on every call for as long as it lasts. On a millisecond counter
     * starting from reset that is a real, if brief, misbehaviour at boot; on a
     * test harness with a frozen clock it never ends.
     *
     * It must also never go backwards. A counter that wraps has to be widened,
     * not allowed to restart.
     *
     * @param ctx The context given alongside these ops.
     * @return Microseconds since some fixed point.
     */
    int64_t (*now_us)(void *ctx);

    /**
     * @brief Take the port lock. NULL means single-threaded.
     * @param ctx The context given alongside these ops.
     */
    void (*lock)(void *ctx);

    /**
     * @brief Release the port lock. NULL means single-threaded.
     * @param ctx The context given alongside these ops.
     */
    void (*unlock)(void *ctx);

    /**
     * @brief Change the line rate. NULL disables baudrate negotiation.
     *
     * Called only from the periodic tick, so stopping and re-arming the
     * receiver inside it is safe.
     *
     * @param ctx  The context given alongside these ops.
     * @param baud The new rate.
     * @return true on success.
     */
    bool (*set_baudrate)(void *ctx, uint32_t baud);

    /**
     * @brief Discard anything buffered on the receive side. May be NULL.
     * @param ctx The context given alongside these ops.
     */
    void (*flush_rx)(void *ctx);

    /**
     * @brief Take the node-wide router lock. May be NULL.
     *
     * Distinct from @c lock, which covers one port. A router is shared by every
     * port on the node, so its table needs a lock no single port owns. The
     * context passed is still the port's, because a port is what has one -- an
     * implementation is free to ignore it and use a lock of its own.
     *
     * @param ctx The context given alongside these ops.
     */
    void (*router_lock)(void *ctx);

    /**
     * @brief Release the node-wide router lock. May be NULL.
     * @param ctx The context given alongside these ops.
     */
    void (*router_unlock)(void *ctx);

    /**
     * @brief Report an event. May be NULL.
     * @param ctx The context given alongside these ops.
     * @param ev  What happened.
     * @param a   First argument, per @ref crsf_event_t.
     * @param b   Second argument, per @ref crsf_event_t.
     */
    void (*event)(void *ctx, crsf_event_t ev, uint32_t a, uint32_t b);
} crsf_io_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* CRSF_IO_H */
