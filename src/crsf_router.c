/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_router.c
 * @brief Frame forwarding decisions for multi-port CRSF nodes (crsf.md:181).
 */

#include "crsf_router.h"
#include <string.h>

/** Placeholder: ISO C forbids an empty translation unit, and every function
 *  in this file can be configured out. */
typedef int crsf_router_translation_unit_is_not_empty;

#if CRSF_ENABLE_ROUTER

bool crsf_router_init(crsf_router_t *r, uint8_t port_count, uint8_t self_address)
{
    if (!r || port_count == 0 || port_count > CRSF_ROUTER_MAX_PORTS) {
        return false;
    }
    memset(r, 0, sizeof(*r));
    r->port_count = port_count;
    r->self_address = self_address;
    return true;
}

/*
 * The lock is the router's own, not any port's: one router is shared by every
 * port on the node, so a per-port mutex cannot cover its table.
 */
void crsf_router_set_lock(crsf_router_t *r, void (*lock)(void *),
                          void (*unlock)(void *), void *ctx)
{
    if (!r) {
        return;
    }
    r->lock = lock;
    r->unlock = unlock;
    r->lock_ctx = ctx;
}

bool crsf_router_add_static_route(crsf_router_t *r, uint8_t address, uint8_t port)
{
    if (port >= r->port_count) {
        return false;
    }
    /* Replace an existing entry for the same address. */
    for (size_t i = 0; i < CRSF_ROUTER_MAX_ROUTES; i++) {
        if (r->routes[i].valid && r->routes[i].address == address) {
            r->routes[i].port = port;
            r->routes[i].is_static = true;
            return true;
        }
    }
    for (size_t i = 0; i < CRSF_ROUTER_MAX_ROUTES; i++) {
        if (!r->routes[i].valid) {
            r->routes[i].address = address;
            r->routes[i].port = port;
            r->routes[i].is_static = true;
            r->routes[i].valid = true;
            return true;
        }
    }
    return false; /* table full */
}

void crsf_router_clear_learned(crsf_router_t *r)
{
    for (size_t i = 0; i < CRSF_ROUTER_MAX_ROUTES; i++) {
        if (r->routes[i].valid && !r->routes[i].is_static) {
            memset(&r->routes[i], 0, sizeof(r->routes[i]));
        }
    }
}

int crsf_router_lookup(const crsf_router_t *r, uint8_t address)
{
    for (size_t i = 0; i < CRSF_ROUTER_MAX_ROUTES; i++) {
        if (r->routes[i].valid && r->routes[i].address == address) {
            return (int)r->routes[i].port;
        }
    }
    return -1;
}

/**
 * @brief Record that @p address lives on @p port.
 *
 * Static entries are never overwritten, and the broadcast address is never
 * learned — it is a destination, not a location. Our own address is skipped too,
 * since a frame claiming to originate from us says nothing about where anything
 * lives. A full table keeps what it has rather than thrashing.
 *
 * @param r       Router instance.
 * @param address Origin address just observed.
 * @param port    Port it was observed on.
 */
static void learn(crsf_router_t *r, uint8_t address, uint8_t port)
{
    if (port >= r->port_count || address == CRSF_ADDR_BROADCAST) {
        return;
    }
    if (address == r->self_address) {
        return;
    }

    for (size_t i = 0; i < CRSF_ROUTER_MAX_ROUTES; i++) {
        if (r->routes[i].valid && r->routes[i].address == address) {
            if (!r->routes[i].is_static && r->routes[i].port != port) {
                /* A device moved to another port. */
                r->routes[i].port = port;
                r->learned++;
            }
            return;
        }
    }
    for (size_t i = 0; i < CRSF_ROUTER_MAX_ROUTES; i++) {
        if (!r->routes[i].valid) {
            r->routes[i].address = address;
            r->routes[i].port = port;
            r->routes[i].is_static = false;
            r->routes[i].valid = true;
            r->learned++;
            return;
        }
    }
    /* Table full: keep what we have rather than thrashing. */
}

/**
 * @brief Add every port except @p except to the decision.
 *
 * @param r      Router instance.
 * @param except Port to leave out, normally the one the frame arrived on. Pass
 *               CRSF_ROUTER_PORT_LOCAL to include them all.
 * @param out    Decision to append to; its port list is overwritten.
 */
static void flood(const crsf_router_t *r, uint8_t except, crsf_route_decision_t *out)
{
    for (uint8_t p = 0; p < r->port_count; p++) {
        if (p == except) {
            continue;
        }
        if (out->port_count < CRSF_ROUTER_MAX_PORTS) {
            out->ports[out->port_count++] = p;
        }
    }
}

void crsf_router_route(crsf_router_t *r, const crsf_frame_t *frame,
                       uint8_t in_port, crsf_route_decision_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Learn where the sender lives so replies need not be flooded. */
    if (frame->is_extended && in_port < r->port_count) {
        learn(r, frame->origin, in_port);
    }

    if (!frame->is_extended) {
        /*
         * Short-header frames are broadcasts by nature — RC channels, telemetry,
         * link statistics. Everyone on the bus may care, so consume and repeat.
         */
        out->consume_locally = true;
        flood(r, in_port, out);
        r->consumed++;
        if (out->port_count) {
            r->forwarded++;
        }
        return;
    }

    /* Addressed to us: ours alone. */
    if (frame->destination == r->self_address) {
        out->consume_locally = true;
        r->consumed++;
        return;
    }

    /* Broadcast: ours as well as everyone else's (crsf.md:651 pings rely on it). */
    if (frame->destination == CRSF_ADDR_BROADCAST) {
        out->consume_locally = true;
        flood(r, in_port, out);
        r->consumed++;
        if (out->port_count) {
            r->forwarded++;
        }
        return;
    }

    /* Addressed elsewhere: send it toward the known port, else flood. */
    const int port = crsf_router_lookup(r, frame->destination);
    if (port >= 0) {
        if ((uint8_t)port == in_port) {
            /*
             * The destination is back where the frame came from. Repeating it
             * there would bounce it straight back, so drop it.
             */
            r->dropped_no_route++;
            return;
        }
        out->ports[out->port_count++] = (uint8_t)port;
        r->forwarded++;
        return;
    }

    flood(r, in_port, out);
    if (out->port_count) {
        r->forwarded++;
    } else {
        r->dropped_no_route++;
    }
}

#endif /* CRSF_ENABLE_ROUTER */
