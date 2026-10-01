/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_arduino_port.cpp
 * @brief CRSFv3, driven on a host against a mock Arduino runtime.
 *
 * The code under test is the real, shipping `src/CRSFv3.cpp`. What is faked is
 * `Stream`, `HardwareSerial` and `micros()`.
 *
 * Read `arduino_stubs/Arduino.h` before trusting a pass: there is no board here.
 * What these cases establish is that the wrapper reads, feeds, ticks, widens its
 * clock and refuses a write it cannot complete — which is all of the wrapper,
 * because everything else is the vendored core, tested in c_crsf.
 */

extern "C" {
#include "test_util.h"
}

#include "CRSFv3.h"
#include "Arduino.h"

#include <string.h>

/** The stream the port runs on. */
static MockSerial g_serial;

/** @brief Open a port on the mock stream. @param p Storage. @return true on success. */
static bool open_port(CRSFv3 &p)
{
    g_serial.reset();
    mock_set_micros(1000);
    return p.begin(416666, CRSF_ROLE_RX);
}

/** @brief Build a battery telemetry frame. @param out Destination. @return Length. */
static size_t build_battery(uint8_t *out)
{
    uint8_t pl[CRSF_MAX_PAYLOAD_SIZE];
    crsf_battery_t bat;
    memset(&bat, 0, sizeof(bat));
    bat.voltage = 168;
    bat.current = 42;
    bat.capacity_used = 7;
    bat.remaining = 66;
    const size_t n = crsf_encode_battery(pl, &bat);
    return crsf_build_frame(out, CRSF_ADDR_FLIGHT_CONTROLLER, CRSF_TYPE_BATTERY, 0, 0,
                            pl, n);
}

/** Records that a frame callback ran. */
struct Rec {
    int calls;         /**< times called */
    uint8_t last_type; /**< type of the last frame */
};

/**
 * @brief Frame callback.
 * @param h     Port handle.
 * @param frame The frame.
 * @param ctx   A Rec.
 */
static void on_frame(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    (void)h;
    Rec *r = static_cast<Rec *>(ctx);
    r->calls++;
    r->last_type = frame->type;
}

/** @brief A port opens on a HardwareSerial and opens the serial too. */
static void test_begin_opens_serial(void)
{
    CRSFv3 crsf(g_serial);
    CHECK(open_port(crsf), "the port opens");
    CHECK_EQ_INT(g_serial.baud(), 416666, "and opened the serial at the right rate");
    CHECK(crsf.port() != NULL, "the protocol port is reachable");
    CHECK_EQ_INT(crsf.baudrate(), 416666, "which reports the same rate");
    crsf.end();
}

/** @brief Bytes on the stream reach the protocol layer when loop() runs. */
static void test_receive(void)
{
    CRSFv3 crsf(g_serial);
    CHECK(open_port(crsf), "the port opens");

    Rec rec = {0, 0};
    CHECK(crsf.onFrame(CRSF_TYPE_BATTERY, on_frame, &rec), "a callback registers");

    uint8_t frame[CRSF_MAX_FRAME_SIZE];
    const size_t len = build_battery(frame);
    g_serial.inject(frame, len);

    CHECK_EQ_INT(rec.calls, 0, "nothing happens until loop() runs");
    crsf.loop();
    CHECK_EQ_INT(rec.calls, 1, "loop() delivers it");
    CHECK_EQ_HEX(rec.last_type, CRSF_TYPE_BATTERY, "as the right frame");
    crsf.end();
}

/** @brief A frame arriving in pieces is still assembled. */
static void test_receive_in_pieces(void)
{
    CRSFv3 crsf(g_serial);
    CHECK(open_port(crsf), "the port opens");

    Rec rec = {0, 0};
    (void)crsf.onFrame(CRSF_TYPE_BATTERY, on_frame, &rec);

    uint8_t frame[CRSF_MAX_FRAME_SIZE];
    const size_t len = build_battery(frame);

    for (size_t i = 0; i < len; i++) {
        g_serial.inject(frame + i, 1);
        crsf.loop();
    }
    CHECK_EQ_INT(rec.calls, 1, "a frame delivered one byte at a time still arrives");
    crsf.end();
}

/** @brief A sent frame reaches the stream. */
static void test_transmit(void)
{
    CRSFv3 crsf(g_serial);
    CHECK(open_port(crsf), "the port opens");

    crsf_attitude_t att;
    memset(&att, 0, sizeof(att));
    att.pitch = 100;
    att.roll = -200;
    att.yaw = 300;

    /* Through the implicit conversion, which is the point of it. */
    CHECK_EQ_INT(crsf_send_attitude(crsf, &att), CRSF_OK, "the C API takes the object");
    crsf.loop();

    const size_t n = g_serial.sent_len();
    CHECK(n > 0, "and the frame reached the stream");
    if (n >= 3) {
        const uint8_t *w = g_serial.sent();
        CHECK_EQ_HEX(w[0], CRSF_ADDR_FLIGHT_CONTROLLER, "with the sync byte");
        CHECK_EQ_HEX(w[2], CRSF_TYPE_ATTITUDE, "and the right type");
        CHECK_EQ_INT(n, (int)w[1] + 2, "and the length the header declares");
    }
    crsf.end();
}

/**
 * @brief A write that cannot complete is refused, not blocked on.
 *
 * Stream::write() blocks once the outgoing ring is full, and the protocol layer
 * calls the sink from inside its periodic tick -- so blocking there would make
 * the telemetry schedule the sketch's latency budget.
 */
static void test_transmit_refuses_when_full(void)
{
    CRSFv3 crsf(g_serial);
    CHECK(open_port(crsf), "the port opens");

    g_serial.set_room(4); /* less than a frame */

    crsf_attitude_t att;
    memset(&att, 0, sizeof(att));
    CHECK_EQ_INT(crsf_send_attitude(crsf, &att), CRSF_ERR_TIMEOUT,
                 "a frame that will not fit is refused, not blocked on");
    CHECK_EQ_INT(g_serial.sent_len(), 0, "and nothing partial was written");

    g_serial.set_room(-1); /* room again */
    CHECK_EQ_INT(crsf_send_attitude(crsf, &att), CRSF_OK, "and it works once there is room");
    crsf.end();
}

/** @brief The scheduler paces telemetry, and the clock drives it. */
static void test_scheduler(void)
{
    CRSFv3 crsf(g_serial);
    CHECK(open_port(crsf), "the port opens");

    crsf_battery_t bat;
    memset(&bat, 0, sizeof(bat));
    bat.voltage = 168;

    CHECK(crsf.publishBattery(bat), "a value is published");
    CHECK(crsf.telemetryInterval(CRSF_TYPE_BATTERY, 100), "a cadence is set");

    crsf.loop();
    const size_t first = g_serial.sent_len();
    CHECK(first > 0, "the first pass emits");

    g_serial.clear_sent();
    mock_advance_micros(50000); /* half the interval */
    crsf.loop();
    CHECK_EQ_INT(g_serial.sent_len(), 0, "nothing emits inside the interval");

    mock_advance_micros(60000);
    crsf.loop();
    CHECK(g_serial.sent_len() > 0, "and one emits once it has passed");
    crsf.end();
}

/**
 * @brief The clock keeps increasing across the 32-bit micros() wrap.
 *
 * micros() wraps every 71.6 minutes, which a flight exceeds often enough to
 * matter. A clock that jumps backwards stops the telemetry scheduler until it
 * catches up.
 */
static void test_clock_wrap(void)
{
    CRSFv3 crsf(g_serial);
    g_serial.reset();
    mock_set_micros(0xFFFFFF00u);
    CHECK(crsf.begin(416666, CRSF_ROLE_RX), "the port opens near the wrap");

    crsf_battery_t bat;
    memset(&bat, 0, sizeof(bat));
    CHECK(crsf.publishBattery(bat), "a value is published");
    CHECK(crsf.telemetryInterval(CRSF_TYPE_BATTERY, 100), "a cadence is set");
    crsf.loop();
    g_serial.clear_sent();

    /*
     * Cross it. 0xFFFFFF00 to 0x00000100 is 512 microseconds of real elapsed
     * time -- a long way inside the 100 ms cadence -- so a correct clock emits
     * nothing here. A clock that failed to widen would read the jump as an
     * enormous step backwards; one that wrapped twice would read it as forwards
     * by over an hour. Either way the scheduler would fire, so this single check
     * catches both.
     */
    mock_set_micros(0x00000100u);
    crsf.loop();
    CHECK_EQ_INT(g_serial.sent_len(), 0,
                 "512 us across the wrap is still inside the interval");

    mock_advance_micros(200000);
    crsf.loop();
    CHECK(g_serial.sent_len() > 0, "and it fires normally afterwards");
    crsf.end();
}

/**
 * @brief channel() returns the midpoint before anything arrives.
 *
 * Documented, and checked, because a sketch that does not also test linkUp()
 * will fly on a centred stick after the link drops -- which is the single most
 * dangerous thing a CRSF library can do quietly.
 */
static void test_channel_defaults_to_midpoint(void)
{
    CRSFv3 crsf(g_serial);
    CHECK(open_port(crsf), "the port opens");

    CHECK(!crsf.linkUp(), "the link is not up before anything arrives");
    CHECK_EQ_INT(crsf.channel(0), 992, "and channel() reads the midpoint");
    CHECK_EQ_INT(crsf.channel(99), 992, "as does an out-of-range index");

    uint8_t pl[CRSF_MAX_PAYLOAD_SIZE];
    crsf_channels_t ch;
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        ch.channel[i] = (uint16_t)(200 + i);
    }
    crsf_channels_pack(pl, &ch);
    uint8_t frame[CRSF_MAX_FRAME_SIZE];
    const size_t len = crsf_build_frame(frame, CRSF_ADDR_FLIGHT_CONTROLLER,
                                        CRSF_TYPE_RC_CHANNELS_PACKED, 0, 0, pl,
                                        CRSF_CHANNELS_PAYLOAD_SIZE);
    g_serial.inject(frame, len);
    crsf.loop();

    CHECK(crsf.linkUp(), "once channels arrive the link is up");
    CHECK_EQ_INT(crsf.channel(0), 200, "and channel() reads what arrived");
    CHECK(crsf.channelUs(0) > 900 && crsf.channelUs(0) < 2100,
          "and channelUs() is in a plausible servo range");
    crsf.end();
}

/**
 * @brief The link hold time is reachable without dropping to the C API.
 *
 * linkUpTimeoutMs() is what makes linkUp() usable: the default 1000 ms is the
 * specification's failsafe recommendation, but a vehicle that must react sooner
 * needs to shorten it, and before this that meant calling
 * crsf_set_link_timeout() on the handle.
 */
static void test_link_timeout(void)
{
    CRSFv3 crsf(g_serial);
    g_serial.reset();
    mock_set_micros(0);
    CHECK(open_port(crsf), "the port opens");

    CHECK_EQ_INT(crsf.linkUpTimeoutMs(), 1000, "the default hold is 1000 ms");
    CHECK(crsf.linkUpTimeoutMs(250), "a shorter hold is accepted");
    CHECK_EQ_INT(crsf.linkUpTimeoutMs(), 250, "and reads back");

    /* Bring the link up, then let it age past the shortened hold. */
    uint8_t pl[CRSF_MAX_PAYLOAD_SIZE];
    crsf_channels_t ch;
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        ch.channel[i] = (uint16_t)(300 + i);
    }
    crsf_channels_pack(pl, &ch);
    uint8_t frame[CRSF_MAX_FRAME_SIZE];
    const size_t len = crsf_build_frame(frame, CRSF_ADDR_FLIGHT_CONTROLLER,
                                        CRSF_TYPE_RC_CHANNELS_PACKED, 0, 0, pl,
                                        CRSF_CHANNELS_PAYLOAD_SIZE);
    g_serial.inject(frame, len);
    crsf.loop();
    CHECK(crsf.linkUp(), "the link comes up");

    mock_advance_micros(200000); /* 200 ms, inside the hold */
    crsf.loop();
    CHECK(crsf.linkUp(), "and stays up inside the hold");

    mock_advance_micros(100000); /* 300 ms total, past it */
    crsf.loop();
    CHECK(!crsf.linkUp(), "and goes down once the hold expires");

    crsf.end();
}

/**
 * @brief CrsfEvery fires once per interval, and survives the millis() wrap.
 *
 * The examples used to open with a static, a millis() subtraction and a
 * comparison a beginner has to get right. This is that idiom, once.
 */
static void test_every(void)
{
    mock_set_micros(0);
    CrsfEvery tick(50);

    CHECK(!tick.due(), "nothing is due at start-up");
    mock_advance_micros(49000);
    CHECK(!tick.due(), "nor just before the interval");
    mock_advance_micros(1000);
    CHECK(tick.due(), "due once the interval passes");
    CHECK(!tick.due(), "and only once");

    mock_advance_micros(50000);
    CHECK(tick.due(), "due again a interval later");

    /*
     * The wrap. millis() is 32 bits and rolls over after 49 days; the unsigned
     * subtraction in due() stays correct across it, where a `now > last + ms`
     * comparison would stall the timer for another 49 days.
     */
    mock_set_micros(0xFFFFFFFFu - 10000u); /* ~10 ms before the micros wrap */
    (void)tick.due();                      /* re-anchor on this side of it */
    mock_advance_micros(60000);            /* across, and past the interval */
    CHECK(tick.due(), "still fires across the millis() wrap");
}

/** @brief A port can be constructed on a bare Stream, not only a HardwareSerial. */
static void test_plain_stream(void)
{
    g_serial.reset();
    mock_set_micros(1000);
    Stream &as_stream = g_serial;
    CRSFv3 crsf(as_stream);

    CHECK(crsf.begin(416666, CRSF_ROLE_RX), "a Stream-backed port opens");
    CHECK_EQ_INT(g_serial.baud(), 0,
                 "and does not try to open the stream itself");

    crsf_attitude_t att;
    memset(&att, 0, sizeof(att));
    CHECK_EQ_INT(crsf_send_attitude(crsf, &att), CRSF_OK, "sending still works");
    crsf.loop();
    CHECK(g_serial.sent_len() > 0, "and the bytes reach the stream");
    crsf.end();
}

/**
 * @brief Run every case.
 * @return 0 when they all pass.
 */
int main(void)
{
    test_begin("arduino");

    test_begin_opens_serial();
    test_receive();
    test_receive_in_pieces();
    test_transmit();
    test_transmit_refuses_when_full();
    test_scheduler();
    test_clock_wrap();
    test_channel_defaults_to_midpoint();
    test_plain_stream();
    test_link_timeout();
    test_every();

    return test_end();
}
