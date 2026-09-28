/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file Arduino.h
 * @brief Enough of the Arduino runtime to compile and drive CrsfPort on a host.
 *
 * **The code under test is the real, shipping `src/CrsfPort.cpp`** — nothing is
 * reimplemented, and no `#ifdef` was added to it to accommodate this.
 *
 * What carries real behaviour:
 *
 * | Stub | Behaviour |
 * | --- | --- |
 * | `Stream` | a real byte queue in each direction, so partial reads and a full outgoing buffer are both reachable |
 * | `HardwareSerial::availableForWrite()` | a settable figure, so the refuse-rather-than-block path can be driven |
 * | `micros()` | a clock the test moves by hand, including across the 32-bit wrap |
 *
 * @par What this deliberately cannot test
 * There is no board. Nothing here says anything about a real UART, a real clock,
 * interrupt latency, or whether `Serial1` exists on the part you have. It tests
 * the wrapper's logic — reading, feeding, ticking, the clock widening and the
 * write-refusal path — and nothing else.
 */

#ifndef ARDUINO_MOCK_H
#define ARDUINO_MOCK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/** @brief Microseconds since start; the test moves it. @return Microseconds. */
uint32_t micros(void);
/** @brief Milliseconds since start. @return Milliseconds. */
uint32_t millis(void);

/** @brief Set the microsecond clock. @param us New value. */
void mock_set_micros(uint32_t us);
/** @brief Advance the microsecond clock. @param us Amount. */
void mock_advance_micros(uint32_t us);

/** @brief A byte stream, as Arduino defines one. */
class Stream {
public:
    virtual ~Stream() {}

    /** @brief Bytes waiting to be read. @return Count. */
    virtual int available() = 0;
    /** @brief Read one byte. @return The byte, or -1. */
    virtual int read() = 0;
    /** @brief Look at the next byte. @return The byte, or -1. */
    virtual int peek() = 0;
    /** @brief Write bytes. @param b Data. @param n Length. @return Bytes written. */
    virtual size_t write(const uint8_t *b, size_t n) = 0;

    /**
     * @brief Read up to @p n bytes.
     * @param b Destination.
     * @param n Maximum.
     * @return Bytes read.
     */
    virtual size_t readBytes(uint8_t *b, size_t n)
    {
        size_t got = 0;
        while (got < n) {
            const int c = read();
            if (c < 0) {
                break;
            }
            b[got++] = (uint8_t)c;
        }
        return got;
    }
};

/** Capacity of each direction of the mock stream. */
#define MOCK_STREAM_CAP 8192

/** @brief A stream backed by two byte queues a test can reach. */
class MockSerial : public Stream {
public:
    MockSerial() { reset(); }

    /** @brief Empty both directions and forget the settings. */
    void reset()
    {
        in_len = in_pos = out_len = 0;
        begun_baud = 0;
        room = -1;
    }

    /** @brief Open at a rate. @param baud The rate. */
    void begin(uint32_t baud) { begun_baud = baud; }
    /** @brief Close. */
    void end() {}

    int available() override { return (int)(in_len - in_pos); }

    int read() override
    {
        return in_pos < in_len ? in_buf[in_pos++] : -1;
    }

    int peek() override { return in_pos < in_len ? in_buf[in_pos] : -1; }

    size_t write(const uint8_t *b, size_t n) override
    {
        if (out_len + n > MOCK_STREAM_CAP) {
            n = MOCK_STREAM_CAP - out_len;
        }
        memcpy(out_buf + out_len, b, n);
        out_len += n;
        return n;
    }

    /**
     * @brief Room in the outgoing buffer, as HardwareSerial reports it.
     * @return Bytes, or -1 when the test has not forced a figure.
     */
    int availableForWrite() { return room; }

    /* --- test control --- */

    /** @brief Deliver bytes as if they had arrived. @param b Data. @param n Length. */
    void inject(const uint8_t *b, size_t n)
    {
        if (in_pos > 0 && in_pos == in_len) {
            in_pos = in_len = 0;
        }
        if (in_len + n > MOCK_STREAM_CAP) {
            n = MOCK_STREAM_CAP - in_len;
        }
        memcpy(in_buf + in_len, b, n);
        in_len += n;
    }

    /** @brief Everything written. @return Pointer to the capture. */
    const uint8_t *sent() const { return out_buf; }
    /** @brief Length of the capture. @return Byte count. */
    size_t sent_len() const { return out_len; }
    /** @brief Drop the capture. */
    void clear_sent() { out_len = 0; }
    /** @brief Force what availableForWrite() reports. @param n Bytes, or -1. */
    void set_room(int n) { room = n; }
    /** @brief The rate begin() was called with. @return Baud, or 0. */
    uint32_t baud() const { return begun_baud; }

private:
    uint8_t in_buf[MOCK_STREAM_CAP];  /**< bytes waiting to be read */
    size_t in_len;                    /**< bytes written into in_buf */
    size_t in_pos;                    /**< read position */
    uint8_t out_buf[MOCK_STREAM_CAP]; /**< bytes written by the port */
    size_t out_len;                   /**< how many */
    uint32_t begun_baud;              /**< what begin() was told */
    int room;                         /**< forced availableForWrite() */
};

/** The mock stands in for HardwareSerial too, so both constructors are covered. */
typedef MockSerial HardwareSerial;

#endif /* ARDUINO_MOCK_H */
