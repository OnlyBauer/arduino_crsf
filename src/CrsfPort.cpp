/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file CrsfPort.cpp
 * @brief The Arduino port. See CrsfPort.h.
 *
 * There is very little here, which is the point. A `Stream` already is a byte
 * sink and a byte source, and `micros()` is a clock, so the hooks the protocol
 * layer insists on are three short functions. Everything that makes CRSF
 * difficult is in the vendored core, identical to what the ESP-IDF and STM32
 * ports run.
 */

#include "CrsfPort.h"

bool CrsfPort::txPush(void *ctx, const uint8_t *frame, size_t len)
{
    CrsfPort *self = static_cast<CrsfPort *>(ctx);

    /*
     * Refuse rather than block where the API allows it to be asked.
     *
     * Stream::write() blocks once the outgoing ring is full, and the protocol
     * layer calls this from inside its periodic tick -- so blocking here would
     * make the telemetry schedule the sketch's latency budget.
     * availableForWrite() is not part of the Stream base class, so this is only
     * possible when we were handed a HardwareSerial. On a plain Stream the
     * write is taken as-is, which is the caller's choice of transport.
     */
    if (self->uart_) {
        const int room = self->uart_->availableForWrite();
        if (room >= 0 && static_cast<size_t>(room) < len) {
            return false;
        }
    }
    return self->io_->write(frame, len) == len;
}

int64_t CrsfPort::nowUs(void *ctx)
{
    (void)ctx;

    /*
     * micros() is 32 bits and wraps every 71.6 minutes, so it is widened here.
     * A flight is longer than that often enough to matter, and the protocol
     * layer computes every age as a signed difference: a clock that jumps
     * backwards stops the telemetry scheduler until it catches up.
     *
     * It also never returns 0, because 0 is the protocol layer's "this never
     * happened" sentinel. micros() reads 0 for the first microsecond after
     * reset, and a port that hands that over makes the scheduler re-emit on
     * every call until the counter moves.
     *
     * Static, and therefore shared by every CrsfPort in the sketch. That is
     * correct: they are all reading the same machine clock.
     */
    static uint32_t last = 0;
    static int64_t high = 0;

    const uint32_t now = micros();
    if (now < last) {
        high += static_cast<int64_t>(1) << 32;
    }
    last = now;

    const int64_t t = high + static_cast<int64_t>(now);
    return t > 0 ? t : 1;
}

bool CrsfPort::begin(uint32_t baud, crsf_role_t role)
{
    crsf_port_config_t cfg = {};
    cfg.role = role;
    cfg.wiring = CRSF_WIRING_FULL_DUPLEX;
    cfg.baudrate = baud ? baud : CRSF_ARDUINO_DEFAULT_BAUD;
    cfg.device_name = "arduino";
    cfg.auto_respond_ping = true;
    return begin(cfg);
}

bool CrsfPort::begin(const crsf_port_config_t &cfg)
{
    if (open_) {
        end();
    }

    const uint32_t baud = cfg.baudrate ? cfg.baudrate : CRSF_ARDUINO_DEFAULT_BAUD;
    if (uart_) {
        uart_->begin(baud);
    }

    ops_ = {};
    ops_.tx_push = &CrsfPort::txPush;
    ops_.now_us = &CrsfPort::nowUs;
    /*
     * No lock, deliberately. A sketch is single-threaded: loop() feeds the port
     * and the sketch calls the API from that same loop(). Where that stops being
     * true -- a second FreeRTOS task on an ESP32, or telemetry published from an
     * interrupt -- fill in ops_.lock and ops_.unlock. Leaving it empty there
     * would be silently wrong rather than loudly wrong, which is why it is
     * called out here and in the README rather than left to be discovered.
     */

    crsf_port_config_t c = cfg;
    c.baudrate = baud;
    open_ = crsf_port_init(&port_, &ops_, this, &c) == CRSF_OK;
    return open_;
}

void CrsfPort::end()
{
    if (!open_) {
        return;
    }
    crsf_port_leave_router(&port_);
    port_.running = false;
    open_ = false;
}

void CrsfPort::loop()
{
    if (!open_) {
        return;
    }

    uint8_t buf[CRSF_ARDUINO_READ_CHUNK];
    int pending = io_->available();
    while (pending > 0) {
        const size_t want = pending > static_cast<int>(sizeof(buf))
                                ? sizeof(buf)
                                : static_cast<size_t>(pending);
        const size_t got = io_->readBytes(buf, want);
        if (got == 0) {
            break;
        }
        crsf_port_poll_reset(&port_);
        crsf_port_feed(&port_, buf, got);
        pending -= static_cast<int>(got);
    }

    /*
     * Anything the protocol layer queued during feed() has already gone out,
     * because txPush() writes straight to the stream. So there is no transmit
     * pump here, and after_tx() is called unconditionally: by this point a frame
     * handed to write() is as far gone as this port can observe. A baudrate
     * change therefore takes effect one loop() late, which is within the
     * negotiation's timing and is the best a Stream can offer -- it has no
     * "transmission complete" to wait on.
     */
    crsf_port_after_tx(&port_);
    crsf_port_tick(&port_);
}

uint16_t CrsfPort::channel(uint8_t i)
{
    crsf_channels_t ch;
    if (i >= CRSF_NUM_CHANNELS ||
        crsf_get_channels(&port_, &ch, nullptr) != CRSF_OK) {
        return 992; /* the midpoint; see the warning in the header */
    }
    return ch.channel[i];
}

uint16_t CrsfPort::channelUs(uint8_t i)
{
    /* 172..1811 ticks map to about 988..2012 us; the usual linear conversion. */
    const int32_t raw = channel(i);
    return static_cast<uint16_t>((raw * 1024L) / 1639L + 881L);
}
