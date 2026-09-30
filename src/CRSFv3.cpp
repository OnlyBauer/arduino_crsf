/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file CRSFv3.cpp
 * @brief The Arduino port. See CRSFv3.h.
 *
 * Very little here, which is the point: a `Stream` is already a byte sink and
 * source and `micros()` is a clock, so the protocol layer's hooks are three
 * short functions. Everything hard is in the vendored core.
 */

#include "CRSFv3.h"

bool CRSFv3::txPush(void *ctx, const uint8_t *frame, size_t len)
{
    CRSFv3 *self = static_cast<CRSFv3 *>(ctx);

    /*
     * Refuse rather than block, where the API lets us ask.
     *
     * Stream::write() blocks once the outgoing ring is full, and this is called
     * from the protocol layer's periodic tick -- blocking would make the
     * telemetry schedule the sketch's latency budget. availableForWrite() is
     * not on the Stream base class, so this only works for a HardwareSerial.
     */
    if (self->uart_) {
        const int room = self->uart_->availableForWrite();
        if (room >= 0 && static_cast<size_t>(room) < len) {
            return false;
        }
    }
    return self->io_->write(frame, len) == len;
}

int64_t CRSFv3::nowUs(void *ctx)
{
    (void)ctx;

    /*
     * micros() is 32 bits and wraps every 71.6 minutes, so widen it: the
     * protocol layer takes every age as a signed difference, and a clock that
     * jumps back stalls the telemetry scheduler until it catches up.
     *
     * Never returns 0 -- that is the protocol layer's "never happened"
     * sentinel, and micros() reads 0 for the first microsecond after reset.
     *
     * Static, so shared by every CRSFv3 in the sketch. Correct: one machine
     * clock.
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

bool CRSFv3::begin(uint32_t baud, crsf_role_t role)
{
    crsf_port_config_t cfg = {};
    cfg.role = role;
    cfg.wiring = CRSF_WIRING_FULL_DUPLEX;
    cfg.baudrate = baud ? baud : CRSF_ARDUINO_DEFAULT_BAUD;
    cfg.device_name = "arduino";
    cfg.auto_respond_ping = true;
    return begin(cfg);
}

bool CRSFv3::begin(const crsf_port_config_t &cfg)
{
    if (open_) {
        end();
    }

    const uint32_t baud = cfg.baudrate ? cfg.baudrate : CRSF_ARDUINO_DEFAULT_BAUD;
    if (uart_) {
        uart_->begin(baud);
    }

    ops_ = {};
    ops_.tx_push = &CRSFv3::txPush;
    ops_.now_us = &CRSFv3::nowUs;
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

void CRSFv3::end()
{
    if (!open_) {
        return;
    }
    crsf_port_leave_router(&port_);
    port_.running = false;
    open_ = false;
}

void CRSFv3::loop()
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

uint16_t CRSFv3::channel(uint8_t i)
{
    crsf_channels_t ch;
    if (i >= CRSF_NUM_CHANNELS ||
        crsf_get_channels(&port_, &ch, nullptr) != CRSF_OK) {
        return 992; /* the midpoint; see the warning in the header */
    }
    return ch.channel[i];
}

uint16_t CRSFv3::channelUs(uint8_t i)
{
    /* 172..1811 ticks map to about 988..2012 us; the usual linear conversion. */
    const int32_t raw = channel(i);
    return static_cast<uint16_t>((raw * 1024L) / 1639L + 881L);
}
