/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file golden_script.h
 * @brief A fixed sequence of public-API calls, and the bytes it puts on the wire.
 *
 * The point of this file is to survive the split. It drives only the portable
 * public API, through a handful of harness callbacks, so the same sequence can
 * be run two ways: against esp_crsf.c with tests/idf_stubs/, and against the
 * extracted core with its own simulated port. If the two byte streams are
 * identical, the extraction did not change what leaves the UART -- which is the
 * one property the unit suites cannot check, because they assert return codes
 * and never look at the wire.
 *
 * Keep the sequence deterministic. Every clock movement is explicit, nothing
 * depends on wall time, and no value is random. A trace that differs between two
 * runs of the same binary is worthless as a comparison.
 */

#ifndef GOLDEN_SCRIPT_H
#define GOLDEN_SCRIPT_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "crsf_protocol.h"
#include "crsf_parser.h"

/**
 * @brief What the script needs from whichever port implementation it drives.
 *
 * Deliberately tiny: anything richer would start encoding assumptions about one
 * of the two harnesses, and the whole value here is that both satisfy the same
 * interface.
 */
typedef struct {
    /** Open a port; non-zero @c tx_role selects the sending end. */
    crsf_handle_t (*open)(int tx_role);
    /** Close a port. */
    void (*close)(crsf_handle_t h);
    /** Run one transmit iteration: at most one frame out, then periodic work. */
    void (*pump)(crsf_handle_t h);
    /** Move the monotonic clock forward. */
    void (*advance_ms)(int64_t ms);
    /** Deliver bytes to the port as if they had arrived on the wire. */
    void (*rx)(crsf_handle_t h, const uint8_t *bytes, size_t len);
    /** Everything written to the wire since the last clear. */
    const uint8_t *(*tx_data)(void);
    /** Length of the buffer returned by @c tx_data. */
    size_t (*tx_len)(void);
    /** Forget what has been written so far. */
    void (*tx_clear)(void);
} golden_ops_t;

/**
 * @brief Pump repeatedly, so queued frames actually reach the wire.
 * @param ops Harness callbacks.
 * @param h   Port handle.
 * @param n   Iterations to run.
 */
static void golden_pump(const golden_ops_t *ops, crsf_handle_t h, int n)
{
    for (int i = 0; i < n; i++) {
        ops->pump(h);
    }
}

/**
 * @brief Dump what is on the wire under a heading, then clear it.
 * @param ops   Harness callbacks.
 * @param out   Where the trace goes.
 * @param label Section heading.
 */
static void golden_dump(const golden_ops_t *ops, FILE *out, const char *label)
{
    const uint8_t *d = ops->tx_data();
    const size_t n = ops->tx_len();

    fprintf(out, "== %s (%u bytes) ==\n", label, (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        fprintf(out, "%02X%s", d[i], ((i % 16) == 15 || i + 1 == n) ? "\n" : " ");
    }
    if (n == 0) {
        fprintf(out, "\n");
    }
    ops->tx_clear();
}

/**
 * @brief Run the whole sequence, writing a hex trace.
 *
 * @param ops Harness callbacks.
 * @param out Where the trace goes.
 */
static void golden_run(const golden_ops_t *ops, FILE *out)
{
    uint8_t buf[CRSF_MAX_FRAME_SIZE];
    uint8_t big[200];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 7 + 3);
    }

    /* --- the receiving end ------------------------------------------------ */
    crsf_handle_t h = ops->open(0);
    if (!h) {
        fprintf(out, "== FAILED TO OPEN RX ==\n");
        return;
    }
    ops->tx_clear();

    const crsf_gps_t gps = {.latitude = 521234567,
                            .longitude = 133456789,
                            .groundspeed = 1234,
                            .heading = 18000,
                            .altitude = 1250,
                            .satellites = 11};
    const crsf_battery_t bat = {.voltage = 168,
                                .current = 425,
                                .capacity_used = 1350,
                                .remaining = 72};
    const crsf_attitude_t att = {.pitch = 1234, .roll = -4321, .yaw = 15000};
    const crsf_vario_t var = {.v_speed = -250};
    const crsf_baro_altitude_t baro = {.altitude_dm = 4321};

    (void)crsf_send_gps(h, &gps);
    (void)crsf_send_battery(h, &bat);
    (void)crsf_send_attitude(h, &att);
    (void)crsf_send_vario(h, &var);
    (void)crsf_send_baro_altitude(h, &baro);
    (void)crsf_send_heartbeat(h);
    (void)crsf_send_flight_mode(h, "ACRO");
    golden_pump(ops, h, 12);
    golden_dump(ops, out, "telemetry-send");

    /* The scheduler: publish, set a cadence, let time pass. */
    (void)crsf_publish_battery(h, &bat);
    (void)crsf_publish_attitude(h, &att);
    (void)crsf_telemetry_set_interval(h, CRSF_TYPE_BATTERY, 100);
    (void)crsf_telemetry_set_interval(h, CRSF_TYPE_ATTITUDE, 50);
    golden_pump(ops, h, 4);
    golden_dump(ops, out, "scheduler-first-tick");

    ops->advance_ms(120);
    golden_pump(ops, h, 4);
    golden_dump(ops, out, "scheduler-after-120ms");

    (void)crsf_telemetry_set_interval(h, CRSF_TYPE_BATTERY, 0);
    (void)crsf_telemetry_set_interval(h, CRSF_TYPE_ATTITUDE, 0);
    golden_pump(ops, h, 4);
    ops->tx_clear();

    /* 0x32 Direct Commands, which carry the second CRC. */
    const uint8_t args[3] = {0x11, 0x22, 0x33};
    (void)crsf_send_command(h, CRSF_ADDR_FLIGHT_CONTROLLER, CRSF_CMD_VTX, 0x01,
                            args, sizeof(args));
    (void)crsf_send_command_ack(h, CRSF_ADDR_FLIGHT_CONTROLLER, CRSF_CMD_VTX,
                                0x01, true, "ok");
    (void)crsf_send_command_ack(h, CRSF_ADDR_FLIGHT_CONTROLLER, CRSF_CMD_VTX,
                                0x02, false, NULL);
    golden_pump(ops, h, 6);
    golden_dump(ops, out, "commands");

    /* Tunnels: both chunk, which is where the byte budgets bite. */
    (void)crsf_mavlink_send(h, CRSF_ADDR_FLIGHT_CONTROLLER, big, 120);
    golden_pump(ops, h, 10);
    golden_dump(ops, out, "mavlink-120");

    (void)crsf_msp_send(h, CRSF_ADDR_FLIGHT_CONTROLLER, big, 40, 2, false);
    golden_pump(ops, h, 8);
    golden_dump(ops, out, "msp-40");

    (void)crsf_send_ping(h, CRSF_ADDR_BROADCAST);
    (void)crsf_send_device_info(h, CRSF_ADDR_FLIGHT_CONTROLLER);
    golden_pump(ops, h, 6);
    golden_dump(ops, out, "ping-device-info");

    /* Inbound: a ping addressed to us should draw an automatic 0x29. */
    {
        const size_t n = crsf_build_frame(buf, CRSF_ADDR_RECEIVER,
                                          CRSF_TYPE_DEVICE_PING,
                                          CRSF_ADDR_RECEIVER,
                                          CRSF_ADDR_TRANSMITTER, NULL, 0);
        ops->rx(h, buf, n);
        golden_pump(ops, h, 6);
        golden_dump(ops, out, "inbound-ping-autoreply");
    }

    ops->close(h);

    /* --- the sending end -------------------------------------------------- */
    h = ops->open(1);
    if (!h) {
        fprintf(out, "== FAILED TO OPEN TX ==\n");
        return;
    }
    ops->tx_clear();

    crsf_channels_t ch;
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        ch.channel[i] = (uint16_t)(172 + i * 100);
    }
    (void)crsf_send_channels(h, &ch);
    golden_pump(ops, h, 4);
    golden_dump(ops, out, "channels");

    ops->close(h);
}

#endif /* GOLDEN_SCRIPT_H */
