/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @brief CrsfParameterDevice — make this board configurable from a handset.
 *
 * Exposes a small parameter tree over the CRSF parameter protocol
 * (0x2B / 0x2C / 0x2D). A handset running EdgeTX "Agent Lite", or the
 * CrsfParameterHost example, can then browse and change these values.
 *
 * The tree deliberately covers every usable parameter type:
 *
 *   0  ROOT        FOLDER          mandatory (crsf.md:709)
 *   1  Power       FLOAT           a number with a unit and a step size
 *   2  Rate        TEXT_SELECTION  a pick-list
 *   3  Model       STRING          editable text
 *   4  Version     INFO            read-only text
 *   5  Bind        COMMAND         an action with a confirmation step
 *   6  Advanced    FOLDER          a sub-folder
 *   7  Invert      TEXT_SELECTION  lives inside Advanced
 */

#include <CRSFv3.h>

/* Which serial port the other device is on. See README.md for other boards.
 * Not AVR: this sketch needs features an ATmega328P has no room for. */
#if defined(ARDUINO_ARCH_STM32)
Uart CrsfSerial(PA10, PA9); /* USART1; this board has no usable Serial1 */
CRSFv3 crsf(CrsfSerial);
#else
CRSFv3 crsf(Serial1);
#endif

/* --- the parameter tree --------------------------------------------------- */

/** Children of the root folder: every top-level parameter number. */
static const uint8_t rootChildren[] = {1, 2, 3, 4, 5, 6};

/** Children of the Advanced sub-folder. */
static const uint8_t advancedChildren[] = {7};

/** Backing store for the STRING parameter, which must be writable. */
static char modelName[CRSF_PARAM_STRING_MAX] = "Quad-1";

/** The parameter tree this device exposes; index i is parameter number i. */
static crsf_param_t params[8];

static void initParams()
{
    for (auto &p : params) {
        p = crsf_param_t();
    }

    /* 0 -- the root folder is mandatory and must be a FOLDER (crsf.md:709). */
    params[0].parent = 0;
    params[0].type = CRSF_PARAM_FOLDER;
    params[0].name = "ROOT";
    params[0].u.folder.children = rootChildren;
    params[0].u.folder.child_count = sizeof(rootChildren);

    /* 1 -- FLOAT. Value, min, max and default are integers; dec_point says
     *      where the decimal point goes, so 250 with dec_point 1 displays as
     *      "25.0". */
    params[1].parent = 0;
    params[1].type = CRSF_PARAM_FLOAT;
    params[1].name = "Power";
    params[1].u.flt.value = 250;
    params[1].u.flt.min = 0;
    params[1].u.flt.max = 1000;
    params[1].u.flt.deflt = 100;
    params[1].u.flt.step = 10;
    params[1].u.flt.dec_point = 1;
    params[1].u.flt.unit = "mW";

    /* 2 -- TEXT_SELECTION. Options are one semicolon-separated string; the
     *      value is the zero-based index into it (crsf.md:782). */
    params[2].parent = 0;
    params[2].type = CRSF_PARAM_TEXT_SELECTION;
    params[2].name = "Rate";
    params[2].u.sel.options = "50Hz;150Hz;250Hz;500Hz";
    params[2].u.sel.value = 2;
    params[2].u.sel.min = 0;
    params[2].u.sel.max = 3;
    params[2].u.sel.deflt = 1;
    params[2].u.sel.unit = "";

    /* 3 -- STRING. */
    params[3].parent = 0;
    params[3].type = CRSF_PARAM_STRING;
    params[3].name = "Model";
    params[3].u.str.value = modelName;
    params[3].u.str.max_len = 16;

    /* 4 -- INFO is read-only text. */
    params[4].parent = 0;
    params[4].type = CRSF_PARAM_INFO;
    params[4].name = "Version";
    params[4].u.info.info = "arduino_crsf 0.1.0";

    /* 5 -- COMMAND. Timeout is in units of 100 ms, so 20 means 2 s. */
    params[5].parent = 0;
    params[5].type = CRSF_PARAM_COMMAND;
    params[5].name = "Bind";
    params[5].u.cmd.status = CRSF_CMD_STATUS_READY;
    params[5].u.cmd.timeout = 20;
    params[5].u.cmd.info = "";

    /* 6 -- a sub-folder. */
    params[6].parent = 0;
    params[6].type = CRSF_PARAM_FOLDER;
    params[6].name = "Advanced";
    params[6].u.folder.children = advancedChildren;
    params[6].u.folder.child_count = sizeof(advancedChildren);

    /* 7 -- lives inside Advanced, so its parent is 6. */
    params[7].parent = 6;
    params[7].type = CRSF_PARAM_TEXT_SELECTION;
    params[7].name = "Invert";
    params[7].u.sel.options = "Off;On";
    params[7].u.sel.value = 0;
    params[7].u.sel.min = 0;
    params[7].u.sel.max = 1;
    params[7].u.sel.deflt = 0;
    params[7].u.sel.unit = "";
}

/** Serves params[] to whoever asks over 0x2C / 0x2D. */
static crsf_param_provider_t provider;

/* --- reacting to changes -------------------------------------------------- */

/**
 * @brief Called after a value write has been stored.
 *
 * This is where you would persist to flash or apply the setting to hardware.
 *
 * @return true to accept the write; false rolls it back.
 */
static bool onParamWrite(uint8_t number, crsf_param_t *p, void *ctx)
{
    (void)ctx;
    switch (p->type) {
    case CRSF_PARAM_FLOAT:
        Serial.print(F("param "));
        Serial.print(number);
        Serial.print(F(" \""));
        Serial.print(p->name);
        Serial.print(F("\" = "));
        Serial.println(p->u.flt.value);
        break;
    case CRSF_PARAM_TEXT_SELECTION:
        Serial.print(F("param "));
        Serial.print(number);
        Serial.print(F(" \""));
        Serial.print(p->name);
        Serial.print(F("\" = index "));
        Serial.println(p->u.sel.value);
        break;
    case CRSF_PARAM_STRING:
        Serial.print(F("param "));
        Serial.print(number);
        Serial.print(F(" \""));
        Serial.print(p->name);
        Serial.print(F("\" = \""));
        Serial.print(p->u.str.value);
        Serial.println('"');
        break;
    default:
        break;
    }
    return true; /* accept */
}

/** When the Bind command entered PROGRESS, so POLL can finish it after 2 s. */
static uint32_t bindStartedMs;

/**
 * @brief Drives the COMMAND lifecycle (crsf.md:847).
 *
 * The device is READY; the host writes START; we may answer PROGRESS,
 * CONFIRMATION_NEEDED or go back to READY. On CONFIRMATION_NEEDED the host
 * replies CONFIRM or CANCEL. POLL only asks and must not change state, which is
 * why a long-running command reports completion from there.
 */
static void onParamCommand(uint8_t number, crsf_param_t *p, crsf_cmd_status_t requested,
                           void *ctx)
{
    (void)number;
    (void)ctx;

    switch (requested) {
    case CRSF_CMD_STATUS_START:
        Serial.println(F("bind requested -- asking for confirmation"));
        p->u.cmd.status = CRSF_CMD_STATUS_CONFIRMATION_NEEDED;
        p->u.cmd.info = "Start binding?";
        break;

    case CRSF_CMD_STATUS_CONFIRM:
        Serial.println(F("bind confirmed -- running"));
        p->u.cmd.status = CRSF_CMD_STATUS_PROGRESS;
        p->u.cmd.info = "Binding...";
        bindStartedMs = millis();
        break;

    case CRSF_CMD_STATUS_CANCEL:
        Serial.println(F("bind cancelled"));
        p->u.cmd.status = CRSF_CMD_STATUS_READY;
        p->u.cmd.info = "Cancelled";
        break;

    case CRSF_CMD_STATUS_POLL:
        /* POLL only asks for the current state and must not change it
         * (crsf.md:851). The handset has to poll to see progress, so this is
         * where a long-running command reports completion. */
        if (p->u.cmd.status == CRSF_CMD_STATUS_PROGRESS &&
            (millis() - bindStartedMs) > 2000) {
            p->u.cmd.status = CRSF_CMD_STATUS_READY;
            p->u.cmd.info = "Bound OK";
            Serial.println(F("bind finished"));
        }
        break;

    default:
        break;
    }
}

/* Print the current settings every five seconds. */
CrsfEvery report(5000);

void setup()
{
    Serial.begin(115200);
    crsf.begin(416666, CRSF_ROLE_RX); /* a configurable peripheral on the craft side */

    initParams();

    /* Parameter 0 must be a FOLDER; the provider refuses the tree otherwise,
     * which is far easier to debug than an empty menu on the handset. */
    const uint8_t count = sizeof(params) / sizeof(params[0]);
    if (!crsf_param_provider_init(&provider, params, count, nullptr)) {
        Serial.println(F("parameter tree rejected -- is parameter 0 a FOLDER?"));
        return;
    }
    provider.on_write = onParamWrite;
    provider.on_command = onParamCommand;

    crsf_params_attach_provider(crsf.port(), &provider);

    Serial.print(F("serving "));
    Serial.print(count);
    Serial.println(F(" parameters -- browse them from a handset or from "
                     "CrsfParameterHost"));
}

void loop()
{
    crsf.loop();

    /* Nothing more to do: the port answers reads and writes on its own. */
    if (!report.due()) {
        return;
    }

    Serial.print(F("Power="));
    Serial.print(params[1].u.flt.value);
    Serial.print(F("  Rate="));
    Serial.print(params[2].u.sel.value);
    Serial.print(F("  Model=\""));
    Serial.print(params[3].u.str.value);
    Serial.print(F("\"  Bind status="));
    Serial.println(params[5].u.cmd.status);
}
