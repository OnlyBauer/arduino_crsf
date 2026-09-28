/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_router.h
 * @brief Frame forwarding between multiple CRSF ports.
 *
 * crsf.md:181 requires it: "If a device has more than one CRSF port it's
 * required to forward all received frames to the other ports. CRSF works as a
 * star network with fixed address tables on each node."
 *
 * This header holds the decision logic only — given "a frame arrived on port N",
 * it says which ports to repeat it on and whether we should also process it
 * ourselves. Keeping it free of UART code makes the routing table testable
 * without hardware, which matters because routing bugs are otherwise only
 * visible as mysteriously missing telemetry.
 *
 * @warning The same spec line forbids loop connections. We deliberately do not
 *          try to detect them: the only cheap mechanism would be a
 *          recently-seen cache keyed on frame content, and identical telemetry
 *          frames legitimately repeat, so such a cache would drop valid traffic.
 *          The single safeguard here is that a frame is never sent back out the
 *          port it arrived on. Wire the ports as a star or a tree; a ring will
 *          circulate frames forever.
 */

#ifndef CRSF_ROUTER_H
#define CRSF_ROUTER_H

#include "crsf_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup crsf_router Routing
 * @brief Multi-port forwarding decisions with address learning.
 * @{
 */

/** Maximum CRSF ports one router can serve. */
#define CRSF_ROUTER_MAX_PORTS 4

/** Size of the learned address table, shared between static and learned entries. */
#define CRSF_ROUTER_MAX_ROUTES 16

/** No port; used for locally originated frames. */
#define CRSF_ROUTER_PORT_LOCAL 0xFF

/** One learned or configured address-to-port mapping. */
typedef struct {
    uint8_t address; /**< CRSF device address this entry describes */
    uint8_t port;    /**< port index that address was reached on */
    bool is_static;  /**< configured entries are never overwritten by learning */
    bool valid;      /**< false marks a free slot */
} crsf_route_t;

struct crsf_port;

/**
 * @brief Router instance.
 *
 * One per node, shared by every port on it. Initialise with crsf_router_init()
 * and treat the fields as private, except the counters, which are meant to be
 * read.
 */
typedef struct {
    uint8_t port_count;                          /**< attached ports, 1..CRSF_ROUTER_MAX_PORTS */
    uint8_t self_address;                        /**< frames addressed here are consumed locally */
    crsf_route_t routes[CRSF_ROUTER_MAX_ROUTES]; /**< address table, static plus learned */

    /*
     * The ports themselves, so a decision to forward can be carried out rather
     * than merely returned. This used to be a file-scope table inside the
     * ESP-IDF port, which made forwarding untestable with more than one port
     * and put a hidden process-global in the middle of a protocol library.
     */
    struct crsf_port *ports[CRSF_ROUTER_MAX_PORTS]; /**< attached ports, by index */

    /*
     * One router is shared by every port on a node, so its table and counters
     * are reached from each port's receive context. A port's own lock cannot
     * cover that -- it is per port -- so the router carries its own.
     *
     * NULL means single-threaded, which is the common case on a bare-metal
     * target and costs nothing there.
     */
    void (*lock)(void *ctx);   /**< take the router lock, or NULL */
    void (*unlock)(void *ctx); /**< release it */
    void *lock_ctx;            /**< passed to both */

    /* Counters, useful for diagnosing a quiet link. All are cumulative and
     * never reset by crsf_router_clear_learned(). */
    uint32_t forwarded;        /**< frames repeated on at least one port */
    uint32_t consumed;         /**< frames handed to our own handlers */
    uint32_t dropped_no_route; /**< addressed frames flooded for want of a route */
    uint32_t learned;          /**< origin addresses added or refreshed */
} crsf_router_t;

/** What to do with a frame that just arrived. */
typedef struct {
    bool consume_locally;                 /**< hand it to our own frame handlers */
    uint8_t port_count;                   /**< number of valid entries in @ref ports */
    uint8_t ports[CRSF_ROUTER_MAX_PORTS]; /**< which ports to repeat it on */
} crsf_route_decision_t;

/**
 * @brief Initialise a router.
 *
 * @param r            Router instance; fully overwritten, counters zeroed.
 * @param port_count   Number of attached ports, 1..CRSF_ROUTER_MAX_PORTS. A
 *                     single port is allowed and simply never forwards.
 * @param self_address Our own device address.
 * @retval true  Initialised.
 * @retval false @p port_count is 0 or above CRSF_ROUTER_MAX_PORTS; @p r is
 *               left untouched.
 */
bool crsf_router_init(crsf_router_t *r, uint8_t port_count, uint8_t self_address);

/**
 * @brief Give the router a lock, and a context to pass to it.
 *
 * Call once, before any port is attached. Without it the router is
 * single-threaded, which is correct when every port is serviced from one
 * context and wrong -- silently -- when they are not.
 *
 * @param r      Router instance.
 * @param lock   Takes the lock; NULL for single-threaded.
 * @param unlock Releases it; NULL for single-threaded.
 * @param ctx    Passed to both, unchanged.
 */
void crsf_router_set_lock(crsf_router_t *r, void (*lock)(void *),
                          void (*unlock)(void *), void *ctx);

/**
 * @brief Pin an address to a port permanently.
 *
 * These entries survive learning, which is what the spec means by "fixed address
 * tables" (crsf.md:181). Use it when the topology is known at build time.
 *
 * @param r       Router instance.
 * @param address Device address to pin.
 * @param port    Port index, below @ref crsf_router_t::port_count.
 * @retval true  Entry stored, replacing any existing entry for @p address.
 * @retval false Invalid port, or the table is full of static entries.
 */
bool crsf_router_add_static_route(crsf_router_t *r, uint8_t address, uint8_t port);

/**
 * @brief Forget every learned route; static entries remain.
 *
 * Useful after a topology change, so a stale mapping does not keep sending
 * replies down a port the device no longer sits on.
 *
 * @param r Router instance.
 */
void crsf_router_clear_learned(crsf_router_t *r);

/**
 * @brief Look up the port for @p address.
 *
 * @param r       Router instance.
 * @param address Device address to resolve.
 * @return Port index, or -1 when the address is not in the table.
 */
int crsf_router_lookup(const crsf_router_t *r, uint8_t address);

/**
 * @brief Decide what to do with a received frame.
 *
 * Rules, in order:
 *  - The frame is never repeated on @p in_port.
 *  - An extended frame addressed to us is consumed and not forwarded.
 *  - An extended frame addressed to broadcast (0x00) is consumed **and**
 *    forwarded, since other nodes need it too.
 *  - Any other extended frame is forwarded to the port where its destination was
 *    last seen, or flooded to every other port when the destination is unknown.
 *  - Broadcast (short-header) frames are consumed and flooded, because telemetry
 *    and channel frames concern every node.
 *
 * Also learns the frame's origin address, so replies can be routed directly
 * instead of flooded.
 *
 * @param r       Router instance.
 * @param frame   The received frame; only its type and header are inspected.
 * @param in_port Port the frame arrived on, or CRSF_ROUTER_PORT_LOCAL when we
 *                generated it ourselves.
 * @param out     Receives the decision; fully overwritten on every call.
 */
void crsf_router_route(crsf_router_t *r, const crsf_frame_t *frame,
                       uint8_t in_port, crsf_route_decision_t *out);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CRSF_ROUTER_H */
