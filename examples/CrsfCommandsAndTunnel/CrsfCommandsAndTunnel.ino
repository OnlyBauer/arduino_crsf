/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @brief CrsfCommandsAndTunnel — 0x32 Direct Commands and the protocol tunnels.
 *
 * Two things that are easy to get wrong, shown working:
 *
 * 0x32 Direct Commands carry a *second* CRC (polynomial 0xBA) inside the
 * payload, on top of the normal frame CRC. crsf_send_command() handles both,
 * in the right order and over the right byte ranges (crsf.md:936).
 *
 * Tunnels: MAVLink frames reach 281 bytes and MSP frames can be longer still,
 * while a CRSF frame caps at 64. Both are therefore split into chunks and
 * reassembled; crsf_mavlink_send() and crsf_msp_send() do the splitting and
 * the port does the reassembly, handing you whole frames.
 */

#include <CRSFv3.h>

CRSFv3 crsf(Serial1);

/* --- inbound commands ----------------------------------------------------- */

/*
 * The port answers the command sets it implements itself (baudrate
 * negotiation and flow control) and ACKs the rest with "not handled".
 * Register a callback if the application wants to act on a command too.
 */
static void onCommand(crsf_handle_t h, const crsf_frame_t *frame, void *ctx)
{
    (void)h;
    (void)ctx;

    /* The parser has already validated the frame CRC; check the command CRC. */
    if (!crsf_command_crc_ok(frame) || frame->payload_len < 3) {
        return;
    }
    const uint8_t cmd = frame->payload[0];
    const uint8_t sub = frame->payload[1];

    switch (cmd) {
    case CRSF_CMD_FC:
        if (sub == CRSF_CMD_FC_FORCE_DISARM) {
            Serial.print(F("force disarm received from 0x"));
            Serial.println(frame->origin, HEX);
        }
        break;
    case CRSF_CMD_VTX:
        if (sub == CRSF_CMD_VTX_SET_POWER && frame->payload_len >= 4) {
            Serial.print(F("VTX power -> "));
            Serial.print(frame->payload[2]);
            Serial.println(F(" dBm"));
        } else if (sub == CRSF_CMD_VTX_SET_FREQUENCY && frame->payload_len >= 5) {
            Serial.print(F("VTX frequency -> "));
            Serial.print(crsf_get_be16(&frame->payload[2]));
            Serial.println(F(" MHz"));
        }
        break;
    case CRSF_CMD_LED:
        if (sub == CRSF_CMD_LED_OVERRIDE_COLOR && frame->payload_len >= 6) {
            crsf_hsv_t c;
            crsf_unpack_hsv(&frame->payload[2], &c);
            Serial.print(F("LED colour -> H"));
            Serial.print(c.h);
            Serial.print(F(" S"));
            Serial.print(c.s);
            Serial.print(F(" V"));
            Serial.println(c.v);
        }
        break;
    case CRSF_CMD_ACK:
        /* crsf.md:966 -- byte 2 is 1 if the target acted, 0 if it does not
         * implement the command. */
        if (frame->payload_len >= 5) {
            Serial.print(F("ACK for cmd 0x"));
            Serial.print(frame->payload[2], HEX);
            Serial.print('.');
            Serial.print(frame->payload[3], HEX);
            Serial.print(F(": "));
            Serial.println(frame->payload[4] ? F("handled") : F("not implemented"));
        }
        break;
    default:
        Serial.print(F("command 0x"));
        Serial.print(cmd, HEX);
        Serial.print('.');
        Serial.print(sub, HEX);
        Serial.print(F(" from 0x"));
        Serial.println(frame->origin, HEX);
        break;
    }
}

/* --- tunnels -------------------------------------------------------------- */

static void onMavlink(crsf_handle_t h, const uint8_t *frame, size_t len, void *ctx)
{
    (void)h;
    (void)ctx;
    /* A whole MAVLink frame, already reassembled from however many 0xAA
     * chunks it took. Hand it to your MAVLink parser here. */
    Serial.print(F("MAVLink frame reassembled: "));
    Serial.print(len);
    Serial.print(F(" bytes, starts "));
    Serial.print(frame[0], HEX);
    Serial.print(' ');
    Serial.print(frame[1], HEX);
    Serial.print(' ');
    Serial.println(frame[2], HEX);
}

static void onMsp(crsf_handle_t h, uint8_t origin, bool isResponse, const uint8_t *body,
                  size_t len, uint8_t version, bool error, void *ctx)
{
    (void)ctx;

    /* For MSPv2 the function id sits in bytes 1..2, little-endian. */
    const uint16_t function = (version == 2 && len >= 3)
                                  ? (uint16_t)(body[1] | (body[2] << 8))
                                  : (len >= 2 ? body[1] : 0);

    Serial.print(F("MSP v"));
    Serial.print(version);
    Serial.print(' ');
    Serial.print(isResponse ? F("response") : F("request"));
    Serial.print(F(": function "));
    Serial.print(function);
    Serial.print(F(", "));
    Serial.print(len);
    Serial.print(F(" bytes"));
    if (error) {
        Serial.print(F(" (error flag set)"));
    }
    Serial.println();

    if (!isResponse) {
        /* crsf.md:1228 -- a response must go back to the origin of the
         * request, with destination and origin swapped. Passing origin here
         * does that. */
        uint8_t reply[8];
        reply[0] = 0;       /* flag */
        reply[1] = body[1]; /* echo the function id */
        reply[2] = (len >= 3) ? body[2] : 0;
        reply[3] = 2;
        reply[4] = 0; /* payload size = 2, little-endian */
        reply[5] = 0xAA;
        reply[6] = 0xBB;
        crsf_msp_send(h, origin, reply, 7, 2, true);
    }
}

void setup()
{
    Serial.begin(115200);

    crsf_port_config_t cfg = {};
    cfg.role = CRSF_ROLE_TX;
    cfg.wiring = CRSF_WIRING_FULL_DUPLEX;
    cfg.device_name = "arduino cmdtun";
    cfg.auto_respond_ping = true;
    /*
     * The spec contradicts itself on whether 0xAA carries an extended header.
     * We follow its frame diagram (short header), which is the only reading
     * consistent with the documented 58-byte data cap. Set this true if your
     * peer expects destination and origin bytes instead.
     */
    cfg.mavlink_envelope_extended_header = false;
    crsf.begin(cfg);

    crsf.onFrame(CRSF_TYPE_COMMAND, onCommand);
    crsf_mavlink_on_frame(crsf, onMavlink, nullptr);
    crsf_msp_on_frame(crsf, onMsp, nullptr);

    Serial.println(F("sending a command every few seconds"));
}

void loop()
{
    crsf.loop();

    static uint32_t last = 0;
    if (millis() - last < 3000) {
        return;
    }
    last = millis();

    static uint32_t tick = 0;

    switch (tick % 6) {
    case 0: {
        /* Set VTX power to 25 dBm (crsf.md:1017). */
        const uint8_t dbm = 25;
        crsf_send_command(crsf, CRSF_ADDR_VTX, CRSF_CMD_VTX, CRSF_CMD_VTX_SET_POWER,
                          &dbm, 1);
        Serial.println(F("-> VTX set power 25 dBm"));
        break;
    }
    case 1: {
        /* Set VTX frequency; the argument is a big-endian uint16 in MHz. */
        uint8_t mhz[2];
        crsf_put_be16(mhz, 5800);
        crsf_send_command(crsf, CRSF_ADDR_VTX, CRSF_CMD_VTX,
                          CRSF_CMD_VTX_SET_FREQUENCY, mhz, sizeof(mhz));
        Serial.println(F("-> VTX set frequency 5800 MHz"));
        break;
    }
    case 2: {
        /* Override the LED colour. HSV is packed into 3 bytes. */
        crsf_hsv_t green;
        green.h = 120;
        green.s = 100;
        green.v = 80;
        uint8_t hsv[3];
        crsf_pack_hsv(hsv, &green);
        crsf_send_command(crsf, CRSF_ADDR_OSD, CRSF_CMD_LED,
                          CRSF_CMD_LED_OVERRIDE_COLOR, hsv, sizeof(hsv));
        Serial.println(F("-> LED colour green"));
        break;
    }
    case 3:
        /* Put a receiver into bind mode. */
        crsf_send_command(crsf, CRSF_ADDR_RECEIVER, CRSF_CMD_CROSSFIRE,
                          CRSF_CMD_XF_SET_BIND_MODE, nullptr, 0);
        Serial.println(F("-> receiver bind mode"));
        break;

    case 4: {
        /* A MAVLink HEARTBEAT-sized frame. 21 bytes fits one envelope; a
         * 281-byte frame would be split across five and reassembled for you. */
        uint8_t mav[21];
        mav[0] = 0xFD; /* MAVLink2 magic */
        for (size_t i = 1; i < sizeof(mav); i++) {
            mav[i] = (uint8_t)i;
        }
        crsf_mavlink_send(crsf, CRSF_ADDR_FLIGHT_CONTROLLER, mav, sizeof(mav));
        Serial.print(F("-> MAVLink frame ("));
        Serial.print(sizeof(mav));
        Serial.println(F(" bytes)"));
        break;
    }
    case 5: {
        /* An MSPv2 request. The body is the MSP frame without its "$X<"
         * header and without the CRC (crsf.md:1216):
         *   [flag][function lo][function hi][size lo][size hi][payload...] */
        uint8_t msp[5];
        msp[0] = 0;
        msp[1] = 0x02;
        msp[2] = 0x10; /* function 0x1002 */
        msp[3] = 0;
        msp[4] = 0; /* no payload */
        crsf_msp_send(crsf, CRSF_ADDR_FLIGHT_CONTROLLER, msp, sizeof(msp), 2, false);
        Serial.println(F("-> MSP request function 0x1002"));
        break;
    }
    default:
        break;
    }

    tick++;
}
