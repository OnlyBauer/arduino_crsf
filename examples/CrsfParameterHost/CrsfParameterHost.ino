/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @brief CrsfParameterHost — discover devices on the bus and read their settings.
 *
 * Acts as the configuring side of the parameter protocol, the role a handset
 * normally plays:
 *
 *   1. broadcast a 0x28 ping
 *   2. collect the 0x29 replies to learn who is out there and how many
 *      parameters each has
 *   3. walk one device's tree with 0x2C reads, reassembling the 0x2B chunks
 *   4. optionally write a value with 0x2D
 *
 * Point this at CrsfParameterDevice on a second board, or at a real Crossfire
 * receiver or VTX.
 */

#include <CRSFv3.h>

#if defined(ARDUINO_ARCH_STM32)
/*
 * Unlike ESP32, RP2040 and SAMD, the STM32 core does not provide a global
 * Serial1: a HardwareSerial exists only where the sketch declares one. PA9/
 * PA10 are USART1, wired to the D1/D0 pins on a Nucleo64 board's Arduino
 * header; adjust for a different board.
 */
HardwareSerial Serial1(PA10, PA9);
#endif

CRSFv3 crsf(Serial1);

/* --- devices we have discovered ------------------------------------------- */

static const uint8_t MAX_DEVICES = 4;

/** One device found by broadcasting a 0x28 ping. */
struct DeviceInfo {
    uint8_t address;
    uint8_t parametersTotal;
    char name[CRSF_DEVICE_NAME_MAX_LEN];
    bool valid;
};

static DeviceInfo devices[MAX_DEVICES];
static uint8_t deviceCount;

static void onDeviceInfo(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    (void)h;
    (void)ctx;

    crsf_device_info_t di;
    if (!crsf_decode_device_info(frame->payload, frame->payload_len, &di)) {
        return;
    }

    /* Ignore a device we already know. */
    for (uint8_t i = 0; i < deviceCount; i++) {
        if (devices[i].address == frame->origin) {
            return;
        }
    }
    if (deviceCount >= MAX_DEVICES) {
        return;
    }

    DeviceInfo &d = devices[deviceCount++];
    d.address = frame->origin;
    d.parametersTotal = di.parameters_total;
    strncpy(d.name, di.device_name, sizeof(d.name) - 1);
    d.name[sizeof(d.name) - 1] = '\0';
    d.valid = true;

    Serial.print(F("device 0x"));
    Serial.print(d.address, HEX);
    Serial.print(F(" \""));
    Serial.print(d.name);
    Serial.print(F("\": fw "));
    Serial.print(di.firmware_id, HEX);
    Serial.print(F(", "));
    Serial.print(d.parametersTotal);
    Serial.println(F(" parameters"));
}

/* --- walking a device's parameter tree ------------------------------------ */

static volatile bool walkFinished;

static void printEntry(const crsf_param_entry_t *e)
{
    const char *hidden = e->hidden ? " (hidden)" : "";

    Serial.print(F("  ["));
    Serial.print(e->number);
    Serial.print(F("] "));
    Serial.print(e->name);
    Serial.print(hidden);

    switch (e->type) {
    case CRSF_PARAM_FOLDER:
        Serial.print(F(" FOLDER, "));
        Serial.print(e->u.folder.child_count);
        Serial.println(F(" children"));
        break;

    case CRSF_PARAM_FLOAT: {
        /* dec_point arrives from the peer; clamp defensively -- an unbounded
         * 10^dec_point overflows to 0 at dec_point 32, which would divide by
         * zero below. crsf_param_parse() already clamps it on the wire path,
         * but this example reads the struct directly. */
        uint8_t digits = e->u.flt.dec_point;
        if (digits > CRSF_PARAM_DEC_POINT_MAX) {
            digits = CRSF_PARAM_DEC_POINT_MAX;
        }
        int32_t scale = 1;
        for (uint8_t i = 0; i < digits; i++) {
            scale *= 10;
        }
        Serial.print(F(" FLOAT  "));
        Serial.print(e->u.flt.value / scale);
        Serial.print('.');
        Serial.print((e->u.flt.value < 0 ? -e->u.flt.value : e->u.flt.value) % scale);
        Serial.print(' ');
        Serial.print(e->u.flt.unit);
        Serial.print(F("  (range "));
        Serial.print(e->u.flt.min);
        Serial.print(F(".."));
        Serial.print(e->u.flt.max);
        Serial.print(F(", step "));
        Serial.print(e->u.flt.step);
        Serial.println(')');
        break;
    }

    case CRSF_PARAM_TEXT_SELECTION:
        Serial.print(F(" SELECT index "));
        Serial.print(e->u.sel.value);
        Serial.print(F(" of \""));
        Serial.print(e->u.sel.options);
        Serial.println('"');
        break;

    case CRSF_PARAM_STRING:
        Serial.print(F(" STRING \""));
        Serial.print(e->u.str.value);
        Serial.print(F("\" (max "));
        Serial.print(e->u.str.max_len);
        Serial.println(')');
        break;

    case CRSF_PARAM_INFO:
        Serial.print(F(" INFO   \""));
        Serial.print(e->u.info.info);
        Serial.println('"');
        break;

    case CRSF_PARAM_COMMAND:
        Serial.print(F(" COMMAND status "));
        Serial.print(e->u.cmd.status);
        Serial.print(F(", timeout "));
        Serial.print(e->u.cmd.timeout);
        Serial.print(F("00 ms, \""));
        Serial.print(e->u.cmd.info);
        Serial.println('"');
        break;

    default:
        Serial.print(F(" type "));
        Serial.println(e->type);
        break;
    }
}

static void onEntry(crsf_handle_t h, uint8_t device, const crsf_param_entry_t *entry,
                    void *ctx)
{
    (void)h;
    (void)device;
    (void)ctx;
    printEntry(entry);
}

static void onDone(crsf_handle_t h, uint8_t device, bool ok, void *ctx)
{
    (void)h;
    (void)ctx;
    Serial.print(F("finished walking device 0x"));
    Serial.print(device, HEX);
    Serial.println(ok ? F(" (ok)") : F(" (failed)"));
    walkFinished = true;
}

/* --- steps 3 and 4, run once from loop() ---------------------------------- */

enum class Step { kPing,
                  kWaitReplies,
                  kWalk,
                  kWrite,
                  kCommand,
                  kDone };
static Step step = Step::kPing;
static uint32_t stepDeadline = 0;
static uint8_t walkIndex = 0;
static uint8_t commandPolls = 0;

void setup()
{
    Serial.begin(115200);
    crsf.begin(416666, CRSF_ROLE_TX); /* the configuring side sits at the TX end */
    crsf.onFrame(CRSF_TYPE_DEVICE_INFO, onDeviceInfo);
}

/** Step 1 and 2: ping everything and give the replies a moment to arrive. */
static void stepPingAndWait()
{
    if (step == Step::kPing) {
        Serial.println(F("pinging the bus..."));
        crsf_send_ping(crsf, CRSF_ADDR_BROADCAST);
        stepDeadline = millis() + 1000;
        step = Step::kWaitReplies;
        return;
    }

    if (millis() < stepDeadline) {
        return;
    }
    if (deviceCount == 0) {
        Serial.println(F("no devices answered -- check the wiring and that the "
                         "peer has auto_respond_ping enabled"));
    }
    walkIndex = 0;
    step = Step::kWalk;
}

/** Step 3: walk each device found, one at a time, without blocking. */
static void stepWalk()
{
    if (walkIndex >= deviceCount) {
        step = Step::kWrite;
        return;
    }

    if (stepDeadline == 0) {
        const DeviceInfo &d = devices[walkIndex];
        Serial.print(F("walking 0x"));
        Serial.print(d.address, HEX);
        Serial.print(F(" \""));
        Serial.print(d.name);
        Serial.println(F("\":"));

        walkFinished = false;
        /* Passing parametersTotal lets the walk stop at the right place.
         * Passing 0 also works -- it then relies on the device answering
         * OUT_OF_RANGE past the end of its list (crsf.md:754). */
        if (crsf_params_walk(crsf, d.address, d.parametersTotal, onEntry, onDone,
                             nullptr) != CRSF_OK) {
            Serial.println(F("walk failed to start"));
            walkIndex++;
            return;
        }
        stepDeadline = millis() + 10000;
        return;
    }

    if (walkFinished) {
        walkIndex++;
        stepDeadline = 0;
    } else if (millis() >= stepDeadline) {
        Serial.println(F("walk timed out"));
        walkIndex++;
        stepDeadline = 0;
    }
}

/** Step 4: change something on the first device found. */
static void stepWriteAndCommand()
{
    if (step == Step::kWrite) {
        if (deviceCount > 0) {
            const uint8_t target = devices[0].address;
            Serial.print(F("writing parameter 1 = 750 on 0x"));
            Serial.println(target, HEX);
            crsf_params_write_float(crsf, target, 1, 750);
        }
        stepDeadline = millis() + 200;
        commandPolls = 0;
        step = Step::kCommand;
        return;
    }

    if (millis() < stepDeadline) {
        return;
    }
    if (deviceCount == 0) {
        Serial.println(F("done"));
        step = Step::kDone;
        return;
    }

    const uint8_t target = devices[0].address;
    if (commandPolls == 0) {
        Serial.print(F("starting command parameter 5 on 0x"));
        Serial.println(target, HEX);
        crsf_params_command(crsf, target, 5, CRSF_CMD_STATUS_START);
        stepDeadline = millis() + 200;
    } else if (commandPolls == 1) {
        /* A confirmation-style command needs CONFIRM next... */
        crsf_params_command(crsf, target, 5, CRSF_CMD_STATUS_CONFIRM);
        stepDeadline = millis() + 500;
    } else if (commandPolls <= 6) {
        /* ...and POLL is what surfaces progress (crsf.md:889). */
        crsf_params_command(crsf, target, 5, CRSF_CMD_STATUS_POLL);
        stepDeadline = millis() + 500;
    } else {
        Serial.println(F("done"));
        step = Step::kDone;
        return;
    }
    commandPolls++;
}

void loop()
{
    crsf.loop();

    switch (step) {
    case Step::kPing:
    case Step::kWaitReplies:
        stepPingAndWait();
        break;
    case Step::kWalk:
        stepWalk();
        break;
    case Step::kWrite:
    case Step::kCommand:
        stepWriteAndCommand();
        break;
    case Step::kDone:
        break;
    }
}
