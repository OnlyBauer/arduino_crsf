/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_protocol.h
 * @brief Pure CRSF protocol definitions — frame types, addresses, limits, payload structs.
 *
 * Deliberately free of ESP-IDF / FreeRTOS dependencies so the protocol layer
 * (CRC, parser, codec, tunnels, parameters, router) can be unit-tested on a host
 * compiler. Nothing in this header allocates, blocks or touches hardware.
 *
 * Reference: tbs-crsf-spec/crsf.md (TBS CRSFv3, CC BY-SA 4.0).
 * Line references in comments point at that document.
 *
 * @see crsf_codec.h for turning these structs into wire bytes and back.
 */

#ifndef CRSF_PROTOCOL_H
#define CRSF_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup crsf_protocol Protocol definitions
 * @brief Frame types, device addresses, size limits and host-order payload structs.
 * @{
 */

/**
 * @name Frame limits
 * A CRSF frame is `sync | length | type | [dest | origin] | payload | crc`, where
 * the length field counts everything from @c type through @c crc inclusive
 * (crsf.md:146, :168, :171).
 * @{
 */

/** Maximum total frame size including sync byte and CRC (crsf.md:146). */
#define CRSF_MAX_FRAME_SIZE 64

/** Minimum value of the frame-length field: Type + CRC (crsf.md:171). */
#define CRSF_FRAME_LEN_MIN 2

/** Maximum value of the frame-length field (crsf.md:171). */
#define CRSF_FRAME_LEN_MAX 62

/** Bytes preceding the length-counted region: sync + length. */
#define CRSF_FRAME_HEADER_SIZE 2

/** Largest payload of a broadcast frame: 62 - type - crc. */
#define CRSF_MAX_PAYLOAD_SIZE 60

/** Largest payload of an extended frame: 60 - dest - origin. */
#define CRSF_MAX_EXT_PAYLOAD_SIZE 58

/** Size of the length-counted region buffer (type .. crc). */
#define CRSF_MAX_FRAME_BODY CRSF_FRAME_LEN_MAX

/**
 * Frame types >= this value carry an extended header (dest + origin),
 * with documented exceptions — see crsf_type_has_ext_header() (crsf.md:647).
 */
#define CRSF_EXT_TYPE_THRESHOLD 0x28

/** @} */

/**
 * @brief CRSF frame types.
 *
 * Naming follows crsf.md section headings. Note that several widely-used
 * libraries (notably ExpressLRS) use different names for some of these IDs;
 * the values here follow the TBS spec, which is authoritative for this project.
 *
 * Types marked "not implemented" are deliberate scope decisions, documented with
 * reasoning in COMPLIANCE.md; the constants exist so a received frame can still
 * be identified and forwarded.
 */
/* clang-format off */ /* Wertespalte ausgerichtet: Tabelle von Wire-Konstanten. */
typedef enum {
    /* --- Broadcast frame types, short header (crsf.md:255-643) --- */
    CRSF_TYPE_GPS                  = 0x02, /**< crsf.md:259 */
    CRSF_TYPE_GPS_TIME             = 0x03, /**< crsf.md:270 */
    CRSF_TYPE_GPS_EXTENDED         = 0x06, /**< crsf.md:284 */
    CRSF_TYPE_VARIO                = 0x07, /**< crsf.md:301 */
    CRSF_TYPE_BATTERY              = 0x08, /**< crsf.md:307 */
    CRSF_TYPE_BARO_ALTITUDE        = 0x09, /**< crsf.md:316 */
    CRSF_TYPE_AIRSPEED             = 0x0A, /**< crsf.md:380 */
    CRSF_TYPE_HEARTBEAT            = 0x0B, /**< crsf.md:386 */
    CRSF_TYPE_RPM                  = 0x0C, /**< crsf.md:392 */
    CRSF_TYPE_TEMP                 = 0x0D, /**< crsf.md:401 */
    CRSF_TYPE_VOLTAGES             = 0x0E, /**< crsf.md:415 */
    CRSF_TYPE_DISCONTINUED         = 0x0F, /**< crsf.md:429 — never emit */
    CRSF_TYPE_VTX_TELEMETRY        = 0x10, /**< crsf.md:431 */
    CRSF_TYPE_BAROMETER            = 0x11, /**< crsf.md:442 */
    CRSF_TYPE_MAGNETOMETER         = 0x12, /**< crsf.md:449 */
    CRSF_TYPE_ACCEL_GYRO           = 0x13, /**< crsf.md:457 */
    CRSF_TYPE_LINK_STATISTICS      = 0x14, /**< crsf.md:481 */
    CRSF_TYPE_LINK_STATS_REPEATER  = 0x15, /**< crsf.md:499 */
    CRSF_TYPE_RC_CHANNELS_PACKED   = 0x16, /**< crsf.md:517 */
    CRSF_TYPE_SUBSET_RC_CHANNELS   = 0x17, /**< crsf.md:548 — discouraged */
    CRSF_TYPE_RC_CHANNELS_11BIT    = 0x18, /**< crsf.md:573 */
    CRSF_TYPE_LINK_STATISTICS_RX   = 0x1C, /**< crsf.md:579 */
    CRSF_TYPE_LINK_STATISTICS_TX   = 0x1D, /**< crsf.md:589 */
    CRSF_TYPE_ATTITUDE             = 0x1E, /**< crsf.md:600 */
    CRSF_TYPE_MAVLINK_FC           = 0x1F, /**< crsf.md:611 */
    CRSF_TYPE_FLIGHT_MODE          = 0x21, /**< crsf.md:627 */
    CRSF_TYPE_ESP_NOW              = 0x22, /**< crsf.md:633 */

    /* --- Extended frame types, dest + origin header (crsf.md:645-1334) --- */
    CRSF_TYPE_DEVICE_PING          = 0x28, /**< crsf.md:649 */
    CRSF_TYPE_DEVICE_INFO          = 0x29, /**< crsf.md:653 */
    CRSF_TYPE_PARAM_ENTRY          = 0x2B, /**< crsf.md:700 */
    CRSF_TYPE_PARAM_READ           = 0x2C, /**< crsf.md:897 */
    CRSF_TYPE_PARAM_WRITE          = 0x2D, /**< crsf.md:906 */
    CRSF_TYPE_COMMAND              = 0x32, /**< crsf.md:926 */
    CRSF_TYPE_LOGGING              = 0x34, /**< crsf.md:1152 — short header! */
    CRSF_TYPE_REMOTE               = 0x3A, /**< crsf.md:1169 */
    CRSF_TYPE_GAME                 = 0x3C, /**< crsf.md:1185 */
    CRSF_TYPE_MSP_REQUEST          = 0x7A, /**< crsf.md:1200 */
    CRSF_TYPE_MSP_RESPONSE         = 0x7B, /**< crsf.md:1208 */
    CRSF_TYPE_ARDUPILOT_PASSTHRU   = 0x80, /**< crsf.md:1232 — short header, not implemented */
    CRSF_TYPE_MLRS_TO_TX           = 0x81, /**< crsf.md:1269 — short header, not implemented */
    CRSF_TYPE_MLRS_FROM_TX         = 0x82, /**< crsf.md:1278 — short header, not implemented */
    CRSF_TYPE_ROTORFLIGHT          = 0x88, /**< crsf.md:1289 — not implemented */
    CRSF_TYPE_MAVLINK_ENVELOPE     = 0xAA, /**< crsf.md:1295 — short header, see crsf_type_has_ext_header() */
    CRSF_TYPE_MAVLINK_SYS_STATUS   = 0xAC, /**< crsf.md:1324 */
} crsf_frame_type_t;
/* clang-format on */

/**
 * @brief Assigned CRSF device addresses (crsf.md:219-253).
 *
 * Used as the destination and origin of extended frames, and as the leading sync
 * byte. Addresses 0x20-0x7F are a dynamic NAT range and therefore not listed
 * individually; see crsf_is_valid_sync().
 */
/* clang-format off */ /* Wertespalte ausgerichtet: Tabelle von Wire-Konstanten. */
typedef enum {
    CRSF_ADDR_BROADCAST          = 0x00, /**< every node; consumed and forwarded */
    CRSF_ADDR_CLOUD              = 0x0E,
    CRSF_ADDR_USB                = 0x10,
    CRSF_ADDR_BLUETOOTH          = 0x12,
    CRSF_ADDR_WIFI_RECEIVER      = 0x13,
    CRSF_ADDR_VIDEO_RECEIVER     = 0x14,
    CRSF_ADDR_OSD                = 0x80, /**< OSD / TBS CORE PNP PRO */
    CRSF_ADDR_ESC1               = 0x90,
    CRSF_ADDR_ESC2               = 0x91,
    CRSF_ADDR_ESC3               = 0x92,
    CRSF_ADDR_ESC4               = 0x93,
    CRSF_ADDR_ESC5               = 0x94,
    CRSF_ADDR_ESC6               = 0x95,
    CRSF_ADDR_ESC7               = 0x96,
    CRSF_ADDR_ESC8               = 0x97,
    CRSF_ADDR_CURRENT_SENSOR     = 0xC0,
    CRSF_ADDR_GPS                = 0xC2,
    CRSF_ADDR_BLACKBOX           = 0xC4,
    CRSF_ADDR_FLIGHT_CONTROLLER  = 0xC8, /**< doubles as the serial sync byte */
    CRSF_ADDR_RACE_TAG           = 0xCC,
    CRSF_ADDR_VTX                = 0xCE,
    CRSF_ADDR_REMOTE_CONTROL     = 0xEA, /**< handset */
    CRSF_ADDR_REPEATER_RX        = 0xEB,
    CRSF_ADDR_RECEIVER           = 0xEC, /**< default address of a CRSF_ROLE_RX port */
    CRSF_ADDR_REPEATER_TX        = 0xED,
    CRSF_ADDR_TRANSMITTER        = 0xEE, /**< default address of a CRSF_ROLE_TX port */
} crsf_address_t;
/* clang-format on */

/** Serial sync byte (crsf.md:165). Numerically equal to the FC address. */
#define CRSF_SYNC_BYTE 0xC8

/**
 * @name Telemetry payload structs
 *
 * These are HOST-ORDER structs, not wire overlays. Conversion to and from the
 * big-endian wire format happens exclusively in crsf_codec.c via explicit
 * byte-offset helpers.
 *
 * @warning Never memcpy or cast a wire buffer onto one of these. The previous
 *          implementation did, which silently depended on the compiler's
 *          little-endian bitfield allocation and read past short payloads.
 * @{
 */

/** 0x02 GPS — 15 bytes on the wire (crsf.md:259). */
typedef struct {
    int32_t latitude;     /**< degrees * 1e7 */
    int32_t longitude;    /**< degrees * 1e7 */
    uint16_t groundspeed; /**< km/h * 100  (LSB = 0.01 km/h) — crsf.md:264 */
    uint16_t heading;     /**< degrees * 100 */
    uint16_t altitude;    /**< metres + 1000 offset (1000 == 0 m) */
    uint8_t satellites;   /**< satellites in view */
} crsf_gps_t;

/** 0x03 GPS Time — 9 bytes (crsf.md:270). UTC, as reported by the GPS. */
typedef struct {
    int16_t year;         /**< full year, e.g. 2026 */
    uint8_t month;        /**< 1..12 */
    uint8_t day;          /**< 1..31 */
    uint8_t hour;         /**< 0..23 */
    uint8_t minute;       /**< 0..59 */
    uint8_t second;       /**< 0..59 */
    uint16_t millisecond; /**< 0..999 */
} crsf_gps_time_t;

/** 0x06 GPS Extended — 20 bytes (crsf.md:284). */
typedef struct {
    uint8_t fix_type;      /**< enum not defined by the spec */
    int16_t n_speed;       /**< cm/s, north positive */
    int16_t e_speed;       /**< cm/s, east positive */
    int16_t v_speed;       /**< cm/s, up positive */
    int16_t h_speed_acc;   /**< horizontal speed accuracy, cm/s */
    int16_t track_acc;     /**< track accuracy, degrees * 10 */
    int16_t alt_ellipsoid; /**< metres above WGS84 ellipsoid (not MSL) */
    int16_t h_acc;         /**< horizontal accuracy, cm */
    int16_t v_acc;         /**< vertical accuracy, cm */
    uint8_t reserved;      /**< reserved by the spec; emitted as 0 */
    uint8_t hdop;          /**< horizontal dilution of precision, units of 0.1 */
    uint8_t vdop;          /**< vertical dilution of precision, units of 0.1 */
} crsf_gps_extended_t;

/** 0x07 Variometer — 2 bytes (crsf.md:301). */
typedef struct {
    int16_t v_speed; /**< cm/s, up positive */
} crsf_vario_t;

/**
 * @brief 0x08 Battery Sensor — 8 bytes (crsf.md:307).
 *
 * @note Units deviate deliberately from the spec text. crsf.md:310-311 states
 *       "LSB = 10 µV" / "10 µA", which cannot be correct: an int16 at 10 µV
 *       tops out at 0.33 V. Every real implementation (Betaflight, ArduPilot,
 *       ExpressLRS, EdgeTX) uses decivolts and deciamps, so that is what we
 *       emit and expect. Treated as a documentation bug in the spec, recorded in
 *       COMPLIANCE.md §5.1.
 */
typedef struct {
    int16_t voltage;        /**< 0.1 V */
    int16_t current;        /**< 0.1 A, signed (regen / reverse current) */
    uint32_t capacity_used; /**< mAh consumed — 24 bit on the wire */
    uint8_t remaining;      /**< percent */
} crsf_battery_t;

/**
 * @brief 0x09 Barometric Altitude & Vertical Speed — 3 bytes (crsf.md:316).
 *
 * Held in decoded form; the packed representation (dm/m switch on bit 15 and
 * the logarithmic vertical speed) is produced by the codec.
 *
 * @see crsf_altitude_pack(), crsf_vspeed_pack()
 */
typedef struct {
    int32_t altitude_dm;  /**< decimetres relative to the calibration point */
    int16_t v_speed_cm_s; /**< cm/s, roughly +-2500 representable */
} crsf_baro_altitude_t;

/** 0x0A Airspeed — 2 bytes (crsf.md:380). */
typedef struct {
    uint16_t speed; /**< 0.1 km/h */
} crsf_airspeed_t;

/** 0x0B Heartbeat — 2 bytes (crsf.md:386). */
typedef struct {
    int16_t origin_address; /**< address of the device announcing itself */
} crsf_heartbeat_t;

/** Maximum RPM values in one 0x0C frame (crsf.md:398). */
#define CRSF_RPM_MAX_VALUES 19

/** 0x0C RPM — 1 + 3*n bytes (crsf.md:392). */
typedef struct {
    uint8_t source_id;                /**< which motor / shaft group these belong to */
    uint8_t count;                    /**< 1 .. CRSF_RPM_MAX_VALUES */
    int32_t rpm[CRSF_RPM_MAX_VALUES]; /**< int24 on the wire, negative = reverse */
} crsf_rpm_t;

/** Maximum temperature values in one 0x0D frame (crsf.md:407). */
#define CRSF_TEMP_MAX_VALUES 20

/**
 * @name Fixed temperature source IDs (crsf.md:412-413)
 * @{
 */
#define CRSF_TEMP_SOURCE_TX 0xEE /**< temperature reported by the TX module */
#define CRSF_TEMP_SOURCE_RX 0xEC /**< temperature reported by the receiver */
/** @} */

/** 0x0D Temperature — 1 + 2*n bytes (crsf.md:401). */
typedef struct {
    uint8_t source_id;                         /**< sensor group, e.g. CRSF_TEMP_SOURCE_RX */
    uint8_t count;                             /**< 1 .. CRSF_TEMP_MAX_VALUES */
    int16_t temperature[CRSF_TEMP_MAX_VALUES]; /**< deci-degrees Celsius */
} crsf_temp_t;

/** Maximum voltages in one 0x0E frame (crsf.md:426). */
#define CRSF_VOLTAGES_MAX_VALUES 29

/** 0x0E Voltages — 1 + 2*n bytes (crsf.md:415). */
typedef struct {
    uint8_t source_id;                          /**< 0..127 = cells of one battery; 128..255 = general */
    uint8_t count;                              /**< 1 .. CRSF_VOLTAGES_MAX_VALUES */
    uint16_t voltage[CRSF_VOLTAGES_MAX_VALUES]; /**< millivolts */
} crsf_voltages_t;

/**
 * @brief 0x10 VTX Telemetry — 5 bytes (crsf.md:431).
 *
 * @warning The three pitmode fields share one byte and the spec does not state
 *          the bit order. We pack LSB-first, consistently with
 *          @ref crsf_vtx_pitmode_t. See COMPLIANCE.md §5.4.
 */
typedef struct {
    uint8_t origin_address;  /**< address of the reporting VTX */
    uint8_t power_dbm;       /**< transmit power in dBm */
    uint16_t frequency_mhz;  /**< centre frequency in MHz */
    uint8_t pit_mode;        /**< 1 bit:  0=Off, 1=On */
    uint8_t pitmode_control; /**< 2 bits: 0=Off, 1=On, 2=Switch, 3=Failsafe */
    uint8_t pitmode_switch;  /**< 4 bits: 0=Ch5, 1=Ch5 Inv, ... 15=Ch12 Inv */
} crsf_vtx_telemetry_t;

/** 0x11 Barometer — 8 bytes (crsf.md:442). */
typedef struct {
    int32_t pressure_pa; /**< absolute pressure in pascals */
    int32_t baro_temp;   /**< sensor temperature, centidegrees Celsius */
} crsf_barometer_t;

/** 0x12 Magnetometer — 6 bytes (crsf.md:449). */
typedef struct {
    int16_t field_x; /**< milligauss * 3 */
    int16_t field_y; /**< milligauss * 3 */
    int16_t field_z; /**< milligauss * 3 */
} crsf_magnetometer_t;

/** 0x13 Accel Gyro — 18 bytes (crsf.md:457). Axes are the NEU body frame. */
typedef struct {
    uint32_t sample_time; /**< microseconds */
    int16_t gyro_x;       /**< LSB = INT16_MAX/2000 DPS */
    int16_t gyro_y;       /**< LSB = INT16_MAX/2000 DPS */
    int16_t gyro_z;       /**< LSB = INT16_MAX/2000 DPS */
    int16_t acc_x;        /**< LSB = INT16_MAX/16 G */
    int16_t acc_y;        /**< LSB = INT16_MAX/16 G */
    int16_t acc_z;        /**< LSB = INT16_MAX/16 G */
    int16_t gyro_temp;    /**< sensor temperature, centidegrees Celsius */
} crsf_accel_gyro_t;

/** RF power enum for crsf_link_statistics_t::up_rf_power (crsf.md:492). */
typedef enum {
    CRSF_RF_POWER_0_MW = 0,
    CRSF_RF_POWER_10_MW = 1,
    CRSF_RF_POWER_25_MW = 2,
    CRSF_RF_POWER_100_MW = 3,
    CRSF_RF_POWER_500_MW = 4,
    CRSF_RF_POWER_1000_MW = 5,
    CRSF_RF_POWER_2000_MW = 6,
    CRSF_RF_POWER_250_MW = 7,
    CRSF_RF_POWER_50_MW = 8,
} crsf_rf_power_t;

/**
 * @brief 0x14 Link Statistics / 0x15 Repeater — 10 bytes (crsf.md:481).
 *
 * "Up" is the uplink as seen by the receiver, "down" the downlink as seen by the
 * transmitter. Values are only meaningful when a radio actually fills them in.
 */
typedef struct {
    uint8_t up_rssi_ant1;      /**< dBm * -1 */
    uint8_t up_rssi_ant2;      /**< dBm * -1 */
    uint8_t up_link_quality;   /**< percent */
    int8_t up_snr;             /**< dB */
    uint8_t active_antenna;    /**< diversity antenna currently selected */
    uint8_t rf_profile;        /**< 0=4fps, 1=50fps, 2=150fps */
    uint8_t up_rf_power;       /**< @ref crsf_rf_power_t */
    uint8_t down_rssi;         /**< dBm * -1 */
    uint8_t down_link_quality; /**< percent */
    int8_t down_snr;           /**< dB */
} crsf_link_statistics_t;

/** 0x1C Link Statistics RX — 5 bytes (crsf.md:579). */
typedef struct {
    uint8_t rssi_db;      /**< dBm * -1 */
    uint8_t rssi_percent; /**< percent */
    uint8_t link_quality; /**< percent */
    int8_t snr;           /**< dB */
    uint8_t rf_power_db;  /**< dBm */
} crsf_link_statistics_rx_t;

/** 0x1D Link Statistics TX — 6 bytes (crsf.md:589). */
typedef struct {
    uint8_t rssi_db;      /**< dBm * -1 */
    uint8_t rssi_percent; /**< percent */
    uint8_t link_quality; /**< percent */
    int8_t snr;           /**< dB */
    uint8_t rf_power_db;  /**< dBm */
    uint8_t fps;          /**< frames per second / 10 */
} crsf_link_statistics_tx_t;

/** @} */

/**
 * @name 0x16 RC Channels Packed (crsf.md:517)
 * @{
 */

/** Number of channels in a 0x16 frame. */
#define CRSF_NUM_CHANNELS 16

/** Wire size of a 0x16 payload: 16 * 11 bits. */
#define CRSF_CHANNELS_PAYLOAD_SIZE 22

/** Default baudrate for full-duplex wiring (crsf.md:136). */
#define CRSF_BAUD_FULL_DUPLEX_DEFAULT 416666

/** Default baudrate for half-duplex wiring (crsf.md:132). */
#define CRSF_BAUD_HALF_DUPLEX_DEFAULT 400000

/** Channel tick value corresponding to 1500 us (crsf.md:525). */
#define CRSF_CHANNEL_CENTER 992

/** Conventional tick value for the 988 us endpoint. */
#define CRSF_CHANNEL_MIN 172

/**
 * Conventional tick value for the upper endpoint.
 *
 * CRSF_TICKS_TO_US(1811) is 2011, not the 2012 this comment used to claim:
 * (1811 - 992) * 5 / 8 + 1500 truncates to 511 + 1500. 1811 is the value the
 * ecosystem uses, so it stays; only the arithmetic in the comment was wrong.
 *
 * Note also that CRSF_TICKS_TO_US and CRSF_US_TO_TICKS are not exact inverses
 * at the extremes -- both truncate toward zero, so 172 -> 988 us -> 173 ticks.
 * test_channel_scaling_extremes() pins that asymmetry deliberately; it is not
 * a defect to be "fixed".
 */
#define CRSF_CHANNEL_MAX 1811

/** Widest value an 11-bit channel can hold. */
#define CRSF_CHANNEL_RAW_MAX 2047

/**
 * @brief Convert channel ticks to microseconds (crsf.md:522).
 * @param x Channel ticks, 0 .. CRSF_CHANNEL_RAW_MAX.
 * @return Pulse width in microseconds.
 */
#define CRSF_TICKS_TO_US(x) (((int32_t)(x) - CRSF_CHANNEL_CENTER) * 5 / 8 + 1500)

/**
 * @brief Convert microseconds to channel ticks (crsf.md:523).
 * @param x Pulse width in microseconds.
 * @return Channel ticks.
 */
#define CRSF_US_TO_TICKS(x) (((int32_t)(x) - 1500) * 8 / 5 + CRSF_CHANNEL_CENTER)

/**
 * @brief 0x16 RC Channels — decoded form.
 *
 * Deliberately a plain array rather than the 11-bit bitfield struct the old
 * implementation used: bitfield layout is implementation-defined, and the
 * previous code silently depended on GCC little-endian LSB-first allocation.
 * The codec does explicit shifting instead.
 *
 * @note Also used for 0x18, which shares this bit layout but scales differently.
 */
typedef struct {
    uint16_t channel[CRSF_NUM_CHANNELS]; /**< 11-bit ticks, 0 .. 2047 */
} crsf_channels_t;

/** @} */

/**
 * @brief 0x1E Attitude — 6 bytes (crsf.md:600).
 *
 * @note crsf.md:603 requires all three angles to stay within ±180°, which is
 *       ±31416 counts. The encoder does not clamp; keeping to the range is the
 *       caller's responsibility.
 */
typedef struct {
    int16_t pitch; /**< LSB = 100 urad (1e-4 rad) */
    int16_t roll;  /**< LSB = 100 urad (1e-4 rad) */
    int16_t yaw;   /**< LSB = 100 urad (1e-4 rad) */
} crsf_attitude_t;

/** 0x1F MAVLink FC — 9 bytes (crsf.md:611). */
typedef struct {
    int16_t airspeed;       /**< units unspecified by the spec */
    uint8_t base_mode;      /**< MAV_MODE_FLAG bitmask */
    uint32_t custom_mode;   /**< autopilot-specific mode */
    uint8_t autopilot_type; /**< MAV_AUTOPILOT */
    uint8_t firmware_type;  /**< MAV_TYPE */
} crsf_mavlink_fc_t;

/** Buffer size for the 0x21 flight-mode string, including terminator. */
#define CRSF_FLIGHT_MODE_MAX_LEN 16

/** 0x21 Flight Mode — null-terminated string (crsf.md:627). */
typedef struct {
    char flight_mode[CRSF_FLIGHT_MODE_MAX_LEN]; /**< always NUL-terminated here */
} crsf_flight_mode_t;

/** Buffer size for the 0x29 device-name string, including terminator. */
#define CRSF_DEVICE_NAME_MAX_LEN 16

/** 0x29 Parameter Device Information (crsf.md:653). */
typedef struct {
    char device_name[CRSF_DEVICE_NAME_MAX_LEN]; /**< always NUL-terminated here */
    uint32_t serial_number;                     /**< device serial, free-form */
    uint32_t hardware_id;                       /**< hardware revision */
    uint32_t firmware_id;                       /**< firmware revision */
    uint8_t parameters_total;                   /**< number of parameters, drives the host's walk */
    uint8_t parameter_version;                  /**< parameter protocol version */
} crsf_device_info_t;

/** 0x3A.0x10 Timing Correction / CRSF Shot (crsf.md:1173). */
typedef struct {
    uint32_t update_interval; /**< LSB = 100 ns */
    int32_t offset;           /**< LSB = 100 ns, positive = data came too early */
} crsf_timing_correction_t;

/** Sub-type byte for 0x3A Timing Correction (crsf.md:1173). */
#define CRSF_REMOTE_SUBTYPE_TIMING_CORRECTION 0x10

/**
 * @name 0x17 / 0x18 Subset RC Channels (crsf.md:548, :573)
 * @{
 */

/** Channel resolution selector for 0x17 (crsf.md:562). */
typedef enum {
    CRSF_SUBSET_RES_10BIT = 0,
    CRSF_SUBSET_RES_11BIT = 1,
    CRSF_SUBSET_RES_12BIT = 2,
    CRSF_SUBSET_RES_13BIT = 3,
} crsf_subset_res_t;

/**
 * @brief Bit width for a resolution selector.
 * @param res A @ref crsf_subset_res_t value; only the low 2 bits are used.
 * @return Bits per channel, 10..13.
 */
static inline unsigned crsf_subset_res_bits(uint8_t res)
{
    return 10u + (res & 0x03u);
}

/**
 * Largest channel count we decode from one 0x17 frame.
 *
 * Not a bit-budget figure: a maximal 60-byte payload is 480 bits, less the
 * 8-bit header, which at the narrowest 10-bit resolution would carry 47
 * channels. The real bound is the 5-bit starting_channel field, which cannot
 * address beyond 32.
 *
 * crsf_decode_subset_channels() stops here and reports what it has, so a frame
 * carrying more is truncated silently. That is well clear of
 * CRSF_NUM_CHANNELS (16), and crsf.md:550 marks 0x17 discouraged with a
 * revision in progress, so the cap is left as it is -- see COMPLIANCE.md's
 * known limitations.
 */
#define CRSF_SUBSET_MAX_CHANNELS 32

/** Largest number of 10-bit digital switch channels we accept. */
#define CRSF_SUBSET_MAX_SWITCHES 8

/**
 * @brief 0x17 Subset RC Channels Packed (crsf.md:548).
 *
 * @warning The spec marks this frame as discouraged with a revision in progress
 *          (crsf.md:550). Implemented for completeness; prefer 0x16.
 * @warning Decoding is inherently incomplete when @ref digital_switch is set —
 *          analogue and switch channels share one bit budget, so the frame
 *          length cannot say where the split lies. See COMPLIANCE.md §7.
 */
typedef struct {
    uint8_t starting_channel;                          /**< 5 bits: first channel number in the frame */
    uint8_t res_configuration;                         /**< 2 bits: @ref crsf_subset_res_t */
    uint8_t digital_switch;                            /**< 1 bit: digital switch channels present */
    uint8_t channel_count;                             /**< derived from the frame length on decode */
    uint16_t channel[CRSF_SUBSET_MAX_CHANNELS];        /**< res_configuration bits each */
    uint8_t switch_count;                              /**< valid entries in @ref switch_channel */
    uint16_t switch_channel[CRSF_SUBSET_MAX_SWITCHES]; /**< 10 bits each */
} crsf_subset_channels_t;

/*
 * Conversion for 0x17 and 0x18 (crsf.md:555-557), which differs from 0x16:
 *   PACK_TX(x)      = (x - 3750) * 8 / 25 + 993
 *   UNPACK_RX(x, S) = x * S + 988
 * with S = 1.0, 0.5, 0.25, 0.125 for 10, 11, 12, 13 bits.
 *
 * S is expressed here as a numerator over 8 to stay in integer arithmetic:
 * 10-bit -> 8/8, 11-bit -> 4/8, 12-bit -> 2/8, 13-bit -> 1/8.
 */

/**
 * @brief Scale numerator over 8 for a resolution selector.
 * @param res A @ref crsf_subset_res_t value.
 * @return 8, 4, 2 or 1, to be divided by 8.
 */
static inline unsigned crsf_subset_scale_num(uint8_t res)
{
    return 8u >> (res & 0x03u);
}

/**
 * @brief Convert a subset channel value to microseconds (crsf.md:556).
 * @param value Raw channel value at resolution @p res.
 * @param res   A @ref crsf_subset_res_t value.
 * @return Pulse width in microseconds.
 */
static inline int32_t crsf_subset_to_us(uint16_t value, uint8_t res)
{
    return (int32_t)((value * crsf_subset_scale_num(res)) / 8u) + 988;
}

/**
 * @brief Convert microseconds to a subset channel value at @p res.
 *
 * Clamped at both ends. The upper clamp matters: the value goes on to
 * crsf_bw_put_lsb() at @p res bits, which keeps only that many low bits, so an
 * unclamped result does not overflow loudly — it silently comes back as an
 * unrelated channel position. At 13-bit resolution 3000 us scales to 16096,
 * which truncates to 7904 and decodes as 1976 us.
 *
 * @param us  Pulse width in microseconds.
 * @param res A @ref crsf_subset_res_t value.
 * @return Raw channel value, clamped to 0 .. (2^res_bits - 1).
 */
static inline uint16_t crsf_subset_from_us(int32_t us, uint8_t res)
{
    const int32_t scaled = ((us - 988) * 8) / (int32_t)crsf_subset_scale_num(res);
    const int32_t max = (int32_t)((1u << crsf_subset_res_bits(res)) - 1u);
    if (scaled < 0) {
        return 0;
    }
    return (uint16_t)(scaled > max ? max : scaled);
}

/** @} */

/**
 * @name 0x22 ESP_NOW Messages (crsf.md:633)
 * 52 bytes, fixed-width character fields that are *not* NUL-terminated on the
 * wire. The struct adds one byte per field so the decoded values are ordinary
 * C strings.
 * @{
 */

/** Wire length of each lap-time field. */
#define CRSF_ESP_NOW_LAP_LEN 15

/** Wire length of the free-text field. */
#define CRSF_ESP_NOW_FREE_TEXT_LEN 20

/** Total 0x22 payload size. */
#define CRSF_PAYLOAD_SIZE_ESP_NOW 52

/** 0x22 ESP_NOW race message. */
typedef struct {
    uint8_t seat_position;                          /**< pilot seat / slot number */
    uint8_t current_lap;                            /**< lap counter */
    char lap_time_a[CRSF_ESP_NOW_LAP_LEN + 1];      /**< +1 for our terminator */
    char lap_time_b[CRSF_ESP_NOW_LAP_LEN + 1];      /**< +1 for our terminator */
    char free_text[CRSF_ESP_NOW_FREE_TEXT_LEN + 1]; /**< +1 for our terminator */
} crsf_esp_now_t;

/** @} */

/**
 * @name 0x34 Logging (crsf.md:1152)
 * Short header despite the type being above 0x28.
 * @{
 */

/** Largest parameter count that fits: (60 - 2 - 4) / 4 = 13. */
#define CRSF_LOG_MAX_PARAMS 13

/** 0x34 Logging frame. */
typedef struct {
    uint16_t logtype;                    /**< application-defined event id */
    uint32_t timestamp;                  /**< application-defined time base */
    uint8_t param_count;                 /**< 0 .. CRSF_LOG_MAX_PARAMS */
    uint32_t param[CRSF_LOG_MAX_PARAMS]; /**< application-defined values */
} crsf_logging_t;

/** @} */

/**
 * @name 0x3C Game (crsf.md:1185)
 * @{
 */

/** 0x3C sub-types, selecting which member of crsf_game_t::u applies. */
typedef enum {
    CRSF_GAME_ADD_POINTS = 0x01,   /**< int16 points */
    CRSF_GAME_COMMAND_CODE = 0x02, /**< uint16 code */
} crsf_game_subtype_t;

/** 0x3C Game frame. */
typedef struct {
    uint8_t sub_type; /**< @ref crsf_game_subtype_t */
    /** Payload, selected by @ref sub_type. */
    union {
        int16_t points; /**< CRSF_GAME_ADD_POINTS, may be negative */
        uint16_t code;  /**< CRSF_GAME_COMMAND_CODE */
    } u;
} crsf_game_t;

/** @} */

/**
 * @name 0xAC MAVLink System Status Sensor (crsf.md:1324)
 * @{
 */

/** Total 0xAC payload size. */
#define CRSF_PAYLOAD_SIZE_MAVLINK_STATUS 12

/** Fields are MAV_SYS_STATUS_SENSOR bitmasks; see the MAVLink common message set. */
typedef struct {
    uint32_t sensor_present; /**< sensors fitted */
    uint32_t sensor_enabled; /**< sensors switched on */
    uint32_t sensor_health;  /**< sensors reporting healthy */
} crsf_mavlink_status_t;

/** @} */

/**
 * @name 0xAA MAVLink Envelope (crsf.md:1295)
 * @{
 */

/** Maximum payload data bytes per envelope chunk (crsf.md:1307). */
#define CRSF_MAVLINK_CHUNK_MAX 58

/** Largest MAVLink2 frame the spec expects to tunnel (crsf.md:1297). */
#define CRSF_MAVLINK_FRAME_MAX 281

/**
 * @brief One 0xAA chunk. Indices are zero-based (crsf.md:1298).
 *
 * Both indices live in a single wire byte, @ref total_chunks in bits 4-7 and
 * @ref current_chunk in bits 0-3, which is why no frame may need more than 16
 * chunks.
 */
typedef struct {
    uint8_t total_chunks;                 /**< index of the LAST chunk, not a count */
    uint8_t current_chunk;                /**< index of this chunk */
    uint8_t data_size;                    /**< valid bytes in @ref data, 0 .. CRSF_MAVLINK_CHUNK_MAX */
    uint8_t data[CRSF_MAVLINK_CHUNK_MAX]; /**< this chunk's slice of the frame */
} crsf_mavlink_envelope_t;

/** @} */

/**
 * @name 0x7A / 0x7B MSP over CRSF (crsf.md:1200)
 * @{
 */

/** Maximum MSP body bytes per CRSF frame (crsf.md:1226). */
#define CRSF_MSP_CHUNK_MAX 57

/**
 * @name MSP status byte fields (crsf.md:1219-1223)
 * @{
 */
#define CRSF_MSP_STATUS_SEQ_MASK 0x0Fu /**< bits 0-3: cyclic sequence number */
#define CRSF_MSP_STATUS_START 0x10u    /**< bit 4: first chunk of a new frame */
#define CRSF_MSP_STATUS_VER_MASK 0x60u /**< bits 5-6: MSP version, 1 or 2 */
#define CRSF_MSP_STATUS_VER_SHIFT 5u   /**< shift for CRSF_MSP_STATUS_VER_MASK */
#define CRSF_MSP_STATUS_ERROR 0x80u    /**< bit 7: error, responses only */
/** @} */

/** @} */

/**
 * @name Parameter protocol type codes (crsf.md:734)
 * @{
 */

/** Bit 7 of the type byte marks a hidden parameter (crsf.md:731). */
#define CRSF_PARAM_HIDDEN_MASK 0x80u

/** Bits 0-6 of the type byte hold the @ref crsf_param_type_t value. */
#define CRSF_PARAM_TYPE_MASK 0x7Fu

/**
 * @brief Parameter data types.
 *
 * Only FLOAT, TEXT_SELECTION, STRING, FOLDER, INFO and COMMAND are usable. The
 * integer types 0..5 are deprecated in favour of FLOAT (crsf.md:756); this
 * implementation decodes them for interoperability but never emits them.
 */
typedef enum {
    CRSF_PARAM_UINT8 = 0,          /**< deprecated, decode only */
    CRSF_PARAM_INT8 = 1,           /**< deprecated, decode only */
    CRSF_PARAM_UINT16 = 2,         /**< deprecated, decode only */
    CRSF_PARAM_INT16 = 3,          /**< deprecated, decode only */
    CRSF_PARAM_UINT32 = 4,         /**< deprecated, decode only */
    CRSF_PARAM_INT32 = 5,          /**< deprecated, decode only */
    CRSF_PARAM_FLOAT = 8,          /**< scaled integer with a decimal point */
    CRSF_PARAM_TEXT_SELECTION = 9, /**< index into a list of option strings */
    CRSF_PARAM_STRING = 10,        /**< free text */
    CRSF_PARAM_FOLDER = 11,        /**< groups other parameters */
    CRSF_PARAM_INFO = 12,          /**< read-only text */
    CRSF_PARAM_COMMAND = 13,       /**< triggers an action, see @ref crsf_cmd_status_t */
    CRSF_PARAM_OUT_OF_RANGE = 127, /**< also marks the end of the list (crsf.md:754) */
} crsf_param_type_t;

/**
 * @brief COMMAND parameter lifecycle (crsf.md:866).
 *
 * A COMMAND starts at READY. The host writes START; the device answers
 * PROGRESS, CONFIRMATION_NEEDED, or READY again. On CONFIRMATION_NEEDED the host
 * replies CONFIRM or CANCEL. POLL asks for current state without changing it.
 */
typedef enum {
    CRSF_CMD_STATUS_READY = 0,               /**< device -> host: idle */
    CRSF_CMD_STATUS_START = 1,               /**< host -> device: run it */
    CRSF_CMD_STATUS_PROGRESS = 2,            /**< device -> host: working */
    CRSF_CMD_STATUS_CONFIRMATION_NEEDED = 3, /**< device -> host: ask the user */
    CRSF_CMD_STATUS_CONFIRM = 4,             /**< host -> device: user agreed */
    CRSF_CMD_STATUS_CANCEL = 5,              /**< host -> device: user declined */
    CRSF_CMD_STATUS_POLL = 6,                /**< host -> device: report state */
} crsf_cmd_status_t;

/** Maximum entry-payload bytes per 0x2B chunk (crsf.md:697). */
#define CRSF_PARAM_CHUNK_MAX 56

/** Terminator of a FOLDER's children list (crsf.md:826). */
#define CRSF_PARAM_CHILDREN_END 0xFF

/** @} */

/**
 * @name 0x32 Direct Commands (crsf.md:926-1150)
 * @{
 */

/** Command sets (crsf.md:966-1150). */
typedef enum {
    CRSF_CMD_FC = 0x01,           /**< crsf.md:977 */
    CRSF_CMD_BLUETOOTH = 0x03,    /**< crsf.md:984 */
    CRSF_CMD_OSD = 0x05,          /**< crsf.md:993 */
    CRSF_CMD_VTX = 0x08,          /**< crsf.md:1000 */
    CRSF_CMD_LED = 0x09,          /**< crsf.md:1021 */
    CRSF_CMD_GENERAL = 0x0A,      /**< crsf.md:1052 */
    CRSF_CMD_CROSSFIRE = 0x10,    /**< crsf.md:1064 */
    CRSF_CMD_RC_OVER_WIFI = 0x13, /**< crsf.md:1083 */
    CRSF_CMD_FLOW_CONTROL = 0x20, /**< crsf.md:1097 */
    CRSF_CMD_SCREEN = 0x22,       /**< crsf.md:1109 */
    CRSF_CMD_ACK = 0xFF,          /**< crsf.md:966 */
} crsf_command_id_t;

/** 0x32.0x01 FC sub-commands (crsf.md:977). */
typedef enum {
    CRSF_CMD_FC_FORCE_DISARM = 0x01,  /**< no arguments */
    CRSF_CMD_FC_SCALE_CHANNEL = 0x02, /**< payload undefined by the spec */
} crsf_cmd_fc_t;

/** 0x32.0x03 Bluetooth sub-commands (crsf.md:984). */
typedef enum {
    CRSF_CMD_BT_RESET = 0x01,  /**< no arguments */
    CRSF_CMD_BT_ENABLE = 0x02, /**< uint8 enable */
    CRSF_CMD_BT_ECHO = 0x64,   /**< no arguments */
} crsf_cmd_bluetooth_t;

/** 0x32.0x08 VTX sub-commands (crsf.md:1000). */
typedef enum {
    CRSF_CMD_VTX_SET_FREQUENCY = 0x02,  /**< uint16 MHz */
    CRSF_CMD_VTX_ENABLE_PITMODE = 0x04, /**< packed @ref crsf_vtx_pitmode_t */
    CRSF_CMD_VTX_POWER_UP = 0x05,       /**< no arguments */
    CRSF_CMD_VTX_SET_DYN_POWER = 0x06,  /**< uint8 dBm, must repeat at 1 Hz */
    CRSF_CMD_VTX_SET_POWER = 0x08,      /**< uint8 dBm */
} crsf_cmd_vtx_t;

/** 0x32.0x0A General sub-commands (crsf.md:1052). */
typedef enum {
    CRSF_CMD_GEN_SPEED_PROPOSAL = 0x70,          /**< uint8 port, uint32 baud */
    CRSF_CMD_GEN_SPEED_PROPOSAL_RESPONSE = 0x71, /**< uint8 port, uint8 accepted */
} crsf_cmd_general_t;

/** 0x32.0x10 Crossfire sub-commands (crsf.md:1064). */
typedef enum {
    CRSF_CMD_XF_SET_BIND_MODE = 0x01,        /**< no arguments */
    CRSF_CMD_XF_CANCEL_BIND_MODE = 0x02,     /**< no arguments */
    CRSF_CMD_XF_SET_BIND_ID = 0x03,          /**< payload undefined by the spec */
    CRSF_CMD_XF_MODEL_SELECT = 0x05,         /**< uint8 model number */
    CRSF_CMD_XF_QUERY_MODEL_SELECT = 0x06,   /**< no arguments */
    CRSF_CMD_XF_REPLY_MODEL_SELECT = 0x07,   /**< uint8 model number */
    CRSF_CMD_XF_ENABLE_RX_TELEMETRY = 0x0A,  /**< no arguments */
    CRSF_CMD_XF_DISABLE_RX_TELEMETRY = 0x0B, /**< no arguments */
} crsf_cmd_crossfire_t;

/** 0x32.0x20 Flow-control sub-commands (crsf.md:1097). */
typedef enum {
    CRSF_CMD_FLOW_SUBSCRIBE = 0x01,   /**< uint8 type, uint16 max interval ms */
    CRSF_CMD_FLOW_UNSUBSCRIBE = 0x02, /**< uint8 type */
} crsf_cmd_flow_t;

/** 0x32.0x09 LED sub-commands (crsf.md:1021). */
typedef enum {
    CRSF_CMD_LED_SET_DEFAULT = 0x01,    /**< bare command, returns to default behaviour */
    CRSF_CMD_LED_OVERRIDE_COLOR = 0x02, /**< HSV */
    CRSF_CMD_LED_OVERRIDE_PULSE = 0x03, /**< uint16 duration + HSV start + HSV stop */
    CRSF_CMD_LED_OVERRIDE_BLINK = 0x04, /**< uint16 interval + HSV start + HSV stop */
    CRSF_CMD_LED_OVERRIDE_SHIFT = 0x05, /**< uint16 interval + HSV */
} crsf_cmd_led_t;

/** 0x32.0x13 RC-over-WiFi sub-commands (crsf.md:1083). */
typedef enum {
    CRSF_CMD_RCWIFI_ENABLE = 0x01,  /**< no arguments */
    CRSF_CMD_RCWIFI_DISABLE = 0x02, /**< no arguments */
} crsf_cmd_rcwifi_t;

/** 0x32.0x22 Screen sub-commands (crsf.md:1109). */
typedef enum {
    CRSF_CMD_SCREEN_POPUP_START = 0x01,   /**< @ref crsf_popup_message_t */
    CRSF_CMD_SCREEN_SELECTION_RET = 0x02, /**< @ref crsf_popup_response_t */
} crsf_cmd_screen_t;

/**
 * @brief A colour for the 0x32.0x09 LED commands (crsf.md:1026).
 *
 * Packed as 9 bits hue, 7 bits saturation, 8 bits value — 24 bits total.
 *
 * @warning The spec does not state the bit order for this field. Unlike the RC
 *          channel frames, where the ecosystem has settled on LSB-first, there
 *          is no consensus here. We pack MSB-first (hue in the top 9 bits)
 *          because the protocol declares frames big-endian (crsf.md:173). This
 *          is a reasoned assumption, not a verified one — if you find a device
 *          that disagrees, this is the first place to look. See COMPLIANCE.md
 *          §5.4.
 */
typedef struct {
    uint16_t h; /**< hue, 0..359 degrees */
    uint8_t s;  /**< saturation, 0..100 percent */
    uint8_t v;  /**< value/brightness, 0..100 percent */
} crsf_hsv_t;

/**
 * @name OSD button bitmask for 0x32.0x05.0x01 (crsf.md:997)
 * @{
 */
#define CRSF_OSD_BTN_ENTER 0x80u /**< enter / select */
#define CRSF_OSD_BTN_UP 0x40u    /**< navigate up */
#define CRSF_OSD_BTN_DOWN 0x20u  /**< navigate down */
#define CRSF_OSD_BTN_LEFT 0x10u  /**< navigate left */
#define CRSF_OSD_BTN_RIGHT 0x08u /**< navigate right */
/** @} */

/** PitMode control mode for 0x32.0x08.0x04 (crsf.md:1010). */
typedef enum {
    CRSF_PITMODE_CTRL_OFF = 0,      /**< pitmode never engaged automatically */
    CRSF_PITMODE_CTRL_ON = 1,       /**< always engaged */
    CRSF_PITMODE_CTRL_ARM = 2,      /**< follows the arm state / a switch */
    CRSF_PITMODE_CTRL_FAILSAFE = 3, /**< engaged on failsafe */
} crsf_pitmode_control_t;

/**
 * @brief 0x32.0x08.0x04 Enable PitMode on power up (crsf.md:1008).
 *
 * @warning The three fields share one byte and the spec does not state the bit
 *          order. We pack LSB-first, consistently with
 *          @ref crsf_vtx_telemetry_t. See COMPLIANCE.md §5.4.
 */
typedef struct {
    uint8_t pit_mode;        /**< 1 bit:  0 = off, 1 = on */
    uint8_t pitmode_control; /**< 2 bits: @ref crsf_pitmode_control_t */
    uint8_t pitmode_switch;  /**< 4 bits: 0 = Ch5, 1 = Ch5 inverted, ... */
} crsf_vtx_pitmode_t;

/** Buffer size for each string in a pop-up message, including terminator. */
#define CRSF_POPUP_STR_MAX 32

/**
 * @brief A pop-up message for devices with a screen (0x32.0x22.0x01, crsf.md:1113).
 *
 * Everything from @ref has_add_data onward is optional and may be absent or
 * truncated mid-frame. crsf.md:1139 is explicit that optional fields must not be
 * read beyond the payload, so the decoder reports which parts were actually
 * present rather than guessing.
 */
typedef struct {
    char header[CRSF_POPUP_STR_MAX];       /**< dialog title */
    char info_message[CRSF_POPUP_STR_MAX]; /**< body text */
    uint8_t max_timeout_interval;          /**< seconds before auto-dismiss */
    bool close_button_option;              /**< offer a close button */

    /** True when the add_data block was present with a non-empty selection text. */
    bool has_add_data;
    char selection_text[CRSF_POPUP_STR_MAX]; /**< label for the value selector */
    uint8_t value;                           /**< current value */
    uint8_t min_value;                       /**< lower bound */
    uint8_t max_value;                       /**< upper bound */
    uint8_t default_value;                   /**< value to offer initially */
    char unit[CRSF_POPUP_STR_MAX];           /**< unit suffix, may be empty */

    /** True when the trailing semicolon-separated value list was present. */
    bool has_possible_values;
    char possible_values[CRSF_POPUP_STR_MAX]; /**< e.g. "Off;On;Auto" */
} crsf_popup_message_t;

/** 0x32.0x22.0x02 Selection Return Value (crsf.md:1141). */
typedef struct {
    uint8_t value; /**< value the user settled on */
    bool process;  /**< true = process, false = cancel */
} crsf_popup_response_t;

/** @} */

/**
 * @brief One validated CRSF frame handed out by the parser.
 *
 * @note @ref payload points into the parser's internal buffer and is only valid
 *       for the duration of the callback. Copy anything you need to keep.
 */
typedef struct {
    uint8_t sync;           /**< the leading byte as received */
    uint8_t type;           /**< @ref crsf_frame_type_t */
    bool is_extended;       /**< true when dest/origin were stripped */
    uint8_t destination;    /**< only meaningful when @ref is_extended */
    uint8_t origin;         /**< only meaningful when @ref is_extended */
    const uint8_t *payload; /**< payload after any extended header */
    uint8_t payload_len;    /**< bytes available at @ref payload */
} crsf_frame_t;

/**
 * @brief Whether a frame type carries an extended (dest + origin) header.
 *
 * The blanket rule is "type >= 0x28" (crsf.md:647), but the spec documents
 * several exceptions that keep the short header: 0x34 Logging (crsf.md:1154),
 * 0x80 ArduPilot passthrough (crsf.md:1234) and 0x81/0x82 mLRS (crsf.md:1265).
 *
 * 0xAA MAVLink Envelope is also treated as short-header here: its frame diagram
 * (crsf.md:1316) shows no dest/origin, and the documented 58-byte data cap only
 * adds up with a short header (1+1+1+1+1+58+1 = 64, against 66 with an extended
 * header). This contradicts the blanket rule, and the arithmetic is the only
 * tie-breaker the spec offers. A peer that reads it the other way can be
 * accommodated with @c crsf_config_t::mavlink_envelope_extended_header, which
 * overrides the transport's choice without changing this function.
 *
 * @param type Frame type byte.
 * @retval true  The frame carries destination and origin bytes.
 * @retval false Short header.
 *
 * @see COMPLIANCE.md §5.3 for the full reasoning.
 */
static inline bool crsf_type_has_ext_header(uint8_t type)
{
    if (type < CRSF_EXT_TYPE_THRESHOLD) {
        return false;
    }
    switch (type) {
    case CRSF_TYPE_LOGGING:
    case CRSF_TYPE_ARDUPILOT_PASSTHRU:
    case CRSF_TYPE_MLRS_TO_TX:
    case CRSF_TYPE_MLRS_FROM_TX:
    case CRSF_TYPE_MAVLINK_ENVELOPE:
        return false;
    default:
        return true;
    }
}

/**
 * @brief Whether a byte is acceptable as a frame's leading sync byte.
 *
 * crsf.md:164 requires accepting 0xC8, 0x00 (broadcast) or any device address.
 * Kept permissive on purpose: the length and CRC checks downstream do the real
 * filtering, and rejecting an unlisted address here would break forwarding
 * through the dynamic NAT address space (0x20-0x7F, crsf.md:227).
 *
 * @param b Candidate byte.
 * @retval true  Start collecting a frame here.
 * @retval false Not a plausible sync byte; keep hunting.
 */
static inline bool crsf_is_valid_sync(uint8_t b)
{
    return b == CRSF_SYNC_BYTE || b == CRSF_ADDR_BROADCAST ||
           b == CRSF_ADDR_CLOUD || b == CRSF_ADDR_USB ||
           b == CRSF_ADDR_BLUETOOTH || b == CRSF_ADDR_WIFI_RECEIVER ||
           b == CRSF_ADDR_VIDEO_RECEIVER || b == CRSF_ADDR_OSD ||
           (b >= CRSF_ADDR_ESC1 && b <= CRSF_ADDR_ESC8) ||
           (b >= 0x20 && b <= 0x7F) || /* dynamic NAT space */
           b == CRSF_ADDR_CURRENT_SENSOR || b == CRSF_ADDR_GPS ||
           b == CRSF_ADDR_BLACKBOX || b == CRSF_ADDR_RACE_TAG ||
           b == CRSF_ADDR_VTX ||
           (b >= CRSF_ADDR_REMOTE_CONTROL && b <= CRSF_ADDR_TRANSMITTER);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CRSF_PROTOCOL_H */
