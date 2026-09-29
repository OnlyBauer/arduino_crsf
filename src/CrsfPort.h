/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file CrsfPort.h
 * @brief CRSFv3 for Arduino.
 *
 * A thin C++ shell over a `Stream`. The protocol underneath is
 * [c_crsf](https://git.bauer.pub/Bauer/c_crsf), the same code the ESP-IDF and
 * STM32 ports run, vendored flat into `src/`.
 *
 * @code
 * #include <CrsfPort.h>
 *
 * CrsfPort crsf(Serial1);
 *
 * void setup() {
 *     crsf.begin(416666, CRSF_ROLE_RX);
 * }
 *
 * void loop() {
 *     crsf.loop();
 *     if (crsf.linkUp()) {
 *         uint16_t throttle = crsf.channel(2);
 *     }
 * }
 * @endcode
 *
 * @par The whole API, not just these methods
 * There are about fifteen convenience methods here, for what a thirty-line
 * sketch actually does. Everything else — the parameter protocol, the MAVLink
 * and MSP tunnels, `0x32` Direct Commands, routing, baudrate negotiation and
 * every `crsf_send_*` and `crsf_publish_*` — is the C API, reached through
 * port() or the implicit conversion:
 *
 * @code
 * crsf_publish_gps(crsf, &gps);      // the conversion makes this work
 * crsf_params_attach_provider(crsf.port(), &provider);
 * @endcode
 *
 * That is deliberate rather than lazy. Wrapping all ~120 functions would double
 * the documentation, guarantee that a function added upstream silently does not
 * appear here, and buy nothing at the byte level: Arduino compiles with
 * `-ffunction-sections -Wl,--gc-sections`, so an unreferenced C function costs
 * exactly as little as an unreferenced inline method would. It is documented
 * once, in c_crsf, and not paraphrased here.
 *
 * @par AVR is not supported
 * A port needs about 3.5 KB of RAM. An ATmega328P has 2 KB in total. There is no
 * combination of switches that closes a gap of that size and leaves something
 * still worth calling a CRSF library — see the README. Compiling for AVR stops
 * with an `#error` rather than an incomprehensible link failure.
 */

#ifndef CRSF_PORT_H
#define CRSF_PORT_H

#include "crsf_arduino_conf.h"

#include <Arduino.h>

extern "C" {
#include "crsf_core.h"
}

/**
 * @brief One CRSF port on an Arduino `Stream`.
 *
 * Allocates nothing: the protocol state is a member, so the object can be a
 * global exactly as an Arduino sketch expects.
 */
class CrsfPort
{
public:
    /**
     * @brief Bind to a stream that something else opens.
     *
     * Use this when the port is not a `HardwareSerial` — a `SoftwareSerial`, a
     * USB CDC, a test double — or when you want to call `begin()` on the serial
     * yourself.
     *
     * @param io The stream; must outlive this object.
     */
    explicit CrsfPort(Stream &io) : io_(&io), uart_(nullptr) {}

    /**
     * @brief Bind to a hardware serial, which this class will open.
     * @param uart The UART; must outlive this object.
     */
    explicit CrsfPort(HardwareSerial &uart) : io_(&uart), uart_(&uart) {}

    /**
     * @brief Open the port.
     *
     * When constructed from a `HardwareSerial`, this calls `begin()` on it at
     * @p baud. When constructed from a `Stream`, the stream is assumed to be
     * open already and @p baud is used only to pace telemetry.
     *
     * @param baud Line rate; 0 selects 416666, the full-duplex default.
     * @param role Which end of the link this is.
     * @return true if the port opened.
     */
    bool begin(uint32_t baud = 0, crsf_role_t role = CRSF_ROLE_RX);

    /**
     * @brief Open the port with the full configuration.
     * @param cfg Everything c_crsf takes; `baudrate` also opens the UART.
     * @return true if the port opened.
     */
    bool begin(const crsf_port_config_t &cfg);

    /** @brief Close the port. The stream is left alone. */
    void end();

    /**
     * @brief Service the port. Call this every pass through `loop()`.
     *
     * Reads whatever the stream has, hands it to the protocol layer, and runs
     * the periodic work: the telemetry scheduler, the parameter walk and the
     * baudrate fallback.
     *
     * Call it often. The telemetry scheduler cannot resolve finer than the
     * interval between calls, so a sketch that blocks for 200 ms cannot honour a
     * 100 ms cadence.
     */
    void loop();

    /**
     * @brief The underlying CRSF port, for the whole C API.
     * @return The handle.
     */
    crsf_handle_t port() { return &port_; }

    /**
     * @brief Implicit conversion, so the C API reads naturally in a sketch.
     *
     * @code
     * crsf_publish_battery(crsf, &battery);
     * @endcode
     *
     * @return The handle.
     */
    operator crsf_handle_t() { return &port_; }

    /** @name What a sketch usually wants
     *  Thin inlines over the C API. Nothing here is unique to Arduino; it is
     *  spelled the way a sketch reads best.
     *  @{
     */

    /** @brief Whether channel frames are arriving. @return true while up. */
    bool linkUp() { return crsf_link_is_up(&port_); }

    /**
     * @brief Latest channel values.
     * @param out   Receives them.
     * @param ageMs Milliseconds since they arrived, or UINT32_MAX if never.
     * @return true if any have ever arrived.
     */
    bool channels(crsf_channels_t &out, uint32_t *ageMs = nullptr)
    {
        return crsf_get_channels(&port_, &out, ageMs) == CRSF_OK;
    }

    /**
     * @brief One channel, in raw CRSF ticks (172..1811).
     *
     * @warning Returns the midpoint when nothing has arrived, so a sketch that
     *          does not also check linkUp() will fly on a centred stick after
     *          the link drops. Check it.
     *
     * @param i Channel index, 0..15.
     * @return The value, or 992 if unavailable.
     */
    uint16_t channel(uint8_t i);

    /**
     * @brief One channel, converted to microseconds (about 988..2012).
     * @param i Channel index, 0..15.
     * @return Microseconds.
     */
    uint16_t channelUs(uint8_t i);

    /** @brief Send channels now. @param c The values. @return true on success. */
    bool sendChannels(const crsf_channels_t &c)
    {
        return crsf_send_channels(&port_, &c) == CRSF_OK;
    }

    /** @brief Set a telemetry cadence. @param type Frame type. @param ms Interval; 0 disables. @return true on success. */
    bool telemetryInterval(uint8_t type, uint32_t ms)
    {
        return crsf_telemetry_set_interval(&port_, type, ms) == CRSF_OK;
    }

    /** @brief Publish battery telemetry. @param b Values. @return true on success. */
    bool publishBattery(const crsf_battery_t &b)
    {
        return crsf_publish_battery(&port_, &b) == CRSF_OK;
    }

    /** @brief Publish attitude telemetry. @param a Values. @return true on success. */
    bool publishAttitude(const crsf_attitude_t &a)
    {
        return crsf_publish_attitude(&port_, &a) == CRSF_OK;
    }

    /** @brief Publish GPS telemetry. @param g Values. @return true on success. */
    bool publishGps(const crsf_gps_t &g)
    {
        return crsf_publish_gps(&port_, &g) == CRSF_OK;
    }

    /** @brief Publish a flight-mode string. @param mode Up to 15 characters. @return true on success. */
    bool publishFlightMode(const char *mode)
    {
        return crsf_publish_flight_mode(&port_, mode) == CRSF_OK;
    }

    /**
     * @brief Call a function for every received frame of one type.
     * @param type Frame type, or 0 for all of them.
     * @param cb   The callback; runs inside loop().
     * @param ctx  Passed back unchanged.
     * @return true on success.
     */
    bool onFrame(uint8_t type, crsf_frame_cb_t cb, void *ctx = nullptr)
    {
        return crsf_on_frame(&port_, type, cb, ctx) == CRSF_OK;
    }

    /** @brief The rate currently in force. @return Baud. */
    uint32_t baudrate() { return crsf_get_baudrate(&port_); }

    /** @brief Frame parser counters, useful for diagnosing a quiet link. @param out Receives them. @return true on success. */
    bool parserStats(crsf_parser_stats_t &out)
    {
        return crsf_get_parser_stats(&port_, &out) == CRSF_OK;
    }

    /** @} */

private:
    /** @brief The frame sink handed to the protocol layer. */
    static bool txPush(void *ctx, const uint8_t *frame, size_t len);
    /** @brief The clock handed to the protocol layer. */
    static int64_t nowUs(void *ctx);

    Stream *io_;           /**< where bytes go and come from */
    HardwareSerial *uart_; /**< non-null when this class opens the UART */
    crsf_port_t port_;     /**< the protocol state */
    crsf_io_ops_t ops_;    /**< the hooks */
    bool open_ = false;    /**< whether begin() succeeded */
};

#endif /* CRSF_PORT_H */
