/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_codec.h
 * @brief Encode/decode CRSF payloads between host structs and the big-endian wire format.
 *
 * Every conversion is explicit. There is deliberately no struct-overlay path:
 * the previous implementation cast a wire buffer straight onto a packed struct,
 * which silently depended on GCC little-endian bitfield allocation and read
 * past the end of short payloads.
 *
 * @par Conventions
 * All codecs in this header follow the same contract, so the per-function
 * documentation below does not repeat it:
 *
 * - `crsf_encode_*(out, in)` writes the payload and returns its size in bytes.
 *   @c out must have room for CRSF_MAX_PAYLOAD_SIZE bytes. A return of 0 means
 *   the input could not be represented and nothing usable was written.
 * - `crsf_decode_*(pl, len, out)` returns false when @c len is below the type's
 *   minimum size, or when the payload is malformed for that type. On failure
 *   @c out is either left untouched or zeroed, depending on the type -- several
 *   decoders clear it up front and only then validate. Either way its contents
 *   are meaningless after a false return, so test the return value rather than
 *   relying on a previous value surviving. Extra trailing bytes are ignored, as
 *   crsf.md:175 requires, so a longer frame from a newer peer still decodes.
 *
 * These functions handle the payload only. Wrapping one in a frame with sync,
 * length and CRC is crsf_build_frame()'s job.
 *
 * @see crsf_protocol.h for the structs, crsf_parser.h for framing.
 */

#ifndef CRSF_CODEC_H
#define CRSF_CODEC_H

#include "crsf_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup crsf_codec Frame codecs
 * @brief Payload encode and decode, explicit big-endian.
 * @{
 */

/**
 * @name Big-endian byte helpers (crsf.md:173)
 * Used instead of struct overlays or htons(), so byte order is visible at the
 * point of use and does not depend on the host's endianness.
 * @{
 */

/**
 * @brief Read a 16-bit big-endian value.
 * @param p Two readable bytes.
 * @return The value in host order.
 */
static inline uint16_t crsf_get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/**
 * @brief Read a 24-bit big-endian unsigned value.
 * @param p Three readable bytes.
 * @return The value in host order, zero-extended.
 */
static inline uint32_t crsf_get_be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/**
 * @brief Read a 32-bit big-endian value.
 * @param p Four readable bytes.
 * @return The value in host order.
 */
static inline uint32_t crsf_get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/**
 * @brief Sign-extend a 24-bit big-endian value (used by 0x0C RPM).
 * @param p Three readable bytes.
 * @return The value in host order, sign-extended to 32 bits.
 */
static inline int32_t crsf_get_be24_signed(const uint8_t *p)
{
    uint32_t v = crsf_get_be24(p);
    return (v & 0x800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
}

/**
 * @brief Write a 16-bit value big-endian.
 * @param p Two writable bytes.
 * @param v Value to store.
 */
static inline void crsf_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/**
 * @brief Write the low 24 bits of a value big-endian.
 * @param p Three writable bytes.
 * @param v Value to store; bits above 24 are discarded.
 */
static inline void crsf_put_be24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

/**
 * @brief Write a 32-bit value big-endian.
 * @param p Four writable bytes.
 * @param v Value to store.
 */
static inline void crsf_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/** @} */

/**
 * @name Fixed wire payload sizes
 * The size each fixed-length encoder produces, and the minimum each decoder
 * requires. Variable-length frames (RPM, temperature, voltages, logging,
 * strings) have no entry here.
 * @{
 */
#define CRSF_PAYLOAD_SIZE_GPS 15          /**< 0x02 GPS */
#define CRSF_PAYLOAD_SIZE_GPS_TIME 9      /**< 0x03 GPS Time */
#define CRSF_PAYLOAD_SIZE_GPS_EXTENDED 20 /**< 0x06 GPS Extended */
#define CRSF_PAYLOAD_SIZE_VARIO 2         /**< 0x07 Variometer */
#define CRSF_PAYLOAD_SIZE_BATTERY 8       /**< 0x08 Battery Sensor */
#define CRSF_PAYLOAD_SIZE_BARO_ALTITUDE 3 /**< 0x09 Baro Altitude + Vertical Speed */
#define CRSF_PAYLOAD_SIZE_AIRSPEED 2      /**< 0x0A Airspeed */
#define CRSF_PAYLOAD_SIZE_HEARTBEAT 2     /**< 0x0B Heartbeat */
#define CRSF_PAYLOAD_SIZE_VTX 5           /**< 0x10 VTX Telemetry */
#define CRSF_PAYLOAD_SIZE_BAROMETER 8     /**< 0x11 Barometer */
#define CRSF_PAYLOAD_SIZE_MAGNETOMETER 6  /**< 0x12 Magnetometer */
#define CRSF_PAYLOAD_SIZE_ACCEL_GYRO 18   /**< 0x13 Accel Gyro */
#define CRSF_PAYLOAD_SIZE_LINK_STATS 10   /**< 0x14 / 0x15 Link Statistics */
#define CRSF_PAYLOAD_SIZE_LINK_STATS_RX 5 /**< 0x1C Link Statistics RX */
#define CRSF_PAYLOAD_SIZE_LINK_STATS_TX 6 /**< 0x1D Link Statistics TX */
#define CRSF_PAYLOAD_SIZE_ATTITUDE 6      /**< 0x1E Attitude */
#define CRSF_PAYLOAD_SIZE_MAVLINK_FC 9    /**< 0x1F MAVLink FC */
#define CRSF_PAYLOAD_SIZE_TIMING_CORR 9   /**< 0x3A.0x10: sub-type byte + 4 + 4 */
/** @} */

/**
 * @name 0x16 RC Channels Packed
 * @{
 */

/**
 * @brief Unpack 22 bytes of 0x16 payload into 16 channel values.
 *
 * The spec declares 0x16 as 11-bit bitfields (crsf.md:527) while separately
 * stating that frames are big-endian (crsf.md:173). Those two statements
 * conflict, and the bit order is never spelled out. Every shipping
 * implementation packs LSB-first across a little-endian bit stream, so that is
 * what this does — explicitly, with shifts, rather than relying on the
 * compiler's bitfield allocation the way the old code did.
 *
 * @param payload Exactly CRSF_CHANNELS_PAYLOAD_SIZE readable bytes.
 * @param out     Receives all 16 channels.
 *
 * @see COMPLIANCE.md §5.2 for the reasoning behind the bit order.
 */
void crsf_channels_unpack(const uint8_t *payload, crsf_channels_t *out);

/**
 * @brief Pack 16 channel values into 22 bytes. Inverse of crsf_channels_unpack().
 *
 * @param out At least CRSF_CHANNELS_PAYLOAD_SIZE writable bytes.
 * @param in  Channels to pack; only the low 11 bits of each are used.
 */
void crsf_channels_pack(uint8_t *out, const crsf_channels_t *in);

/**
 * @brief Encode a 0x16 payload.
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Channels to encode.
 * @return CRSF_CHANNELS_PAYLOAD_SIZE.
 */
size_t crsf_encode_channels(uint8_t *out, const crsf_channels_t *in);

/**
 * @brief Decode a 0x16 payload.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the channels.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_CHANNELS_PAYLOAD_SIZE.
 */
bool crsf_decode_channels(const uint8_t *pl, size_t len, crsf_channels_t *out);

/** @} */

/**
 * @name 0x09 packed altitude / vertical speed helpers (crsf.md:316-378)
 * 0x09 squeezes an altitude and a vertical speed into three bytes by switching
 * the altitude between decimetre and metre units on bit 15, and by storing the
 * vertical speed logarithmically. These helpers expose that packing on its own so
 * it can be tested against the spec's own worked examples.
 * @{
 */

/**
 * @brief Decode the packed altitude field to decimetres (crsf.md:334).
 * @param packed Wire value; bit 15 selects metre units.
 * @return Altitude in decimetres.
 */
int32_t crsf_altitude_unpack(uint16_t packed);

/**
 * @brief Encode decimetres into the packed altitude field (crsf.md:343).
 *
 * @param altitude_dm Altitude in decimetres.
 * @return Wire value, switching to metre units when the range demands it.
 *
 * @note Saturating, not failing: an altitude below the representable minimum
 *       yields 0 (which reads back as -1000 m) and one above the maximum yields
 *       0xFFFE. 0xFFFF is avoided because OpenTX reads it as "invalid". Every
 *       return value is therefore a legal altitude — there is no error code.
 */
uint16_t crsf_altitude_pack(int32_t altitude_dm);

/**
 * @brief Decode the logarithmically packed vertical speed to cm/s (crsf.md:371).
 * @param packed Wire byte.
 * @return Vertical speed in cm/s. The curve bounds this to roughly
 *         -2690..2617 cm/s across the whole int8 input range, so the result
 *         never approaches the int16 limits.
 */
#if CRSF_ENABLE_FLOAT_MATH
int16_t crsf_vspeed_unpack(int8_t packed);

/**
 * @brief Encode cm/s into the logarithmic vertical-speed byte (crsf.md:365).
 * @param v_cm_s Vertical speed in cm/s.
 * @return Wire byte; resolution coarsens as the magnitude grows.
 */
int8_t crsf_vspeed_pack(int16_t v_cm_s);
#endif /* CRSF_ENABLE_FLOAT_MATH */

/** @} */

/**
 * @name Telemetry codecs
 * All follow the conventions documented at the top of this file.
 * @{
 */

/**
 * @brief Encode 0x02 GPS (crsf.md:259).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Position and velocity to encode.
 * @return CRSF_PAYLOAD_SIZE_GPS.
 */
size_t crsf_encode_gps(uint8_t *out, const crsf_gps_t *in);

/**
 * @brief Decode 0x02 GPS.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the values.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_GPS.
 */
bool crsf_decode_gps(const uint8_t *pl, size_t len, crsf_gps_t *out);

/**
 * @brief Encode 0x03 GPS Time (crsf.md:270).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  UTC timestamp to encode.
 * @return CRSF_PAYLOAD_SIZE_GPS_TIME.
 */
size_t crsf_encode_gps_time(uint8_t *out, const crsf_gps_time_t *in);

/**
 * @brief Decode 0x03 GPS Time.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the timestamp.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_GPS_TIME.
 */
bool crsf_decode_gps_time(const uint8_t *pl, size_t len, crsf_gps_time_t *out);

/**
 * @brief Encode 0x06 GPS Extended (crsf.md:284).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Accuracy and velocity detail to encode.
 * @return CRSF_PAYLOAD_SIZE_GPS_EXTENDED.
 */
size_t crsf_encode_gps_extended(uint8_t *out, const crsf_gps_extended_t *in);

/**
 * @brief Decode 0x06 GPS Extended.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the values.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_GPS_EXTENDED.
 */
bool crsf_decode_gps_extended(const uint8_t *pl, size_t len, crsf_gps_extended_t *out);

/**
 * @brief Encode 0x07 Variometer (crsf.md:301).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Vertical speed to encode.
 * @return CRSF_PAYLOAD_SIZE_VARIO.
 */
size_t crsf_encode_vario(uint8_t *out, const crsf_vario_t *in);

/**
 * @brief Decode 0x07 Variometer.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the vertical speed.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_VARIO.
 */
bool crsf_decode_vario(const uint8_t *pl, size_t len, crsf_vario_t *out);

/**
 * @brief Encode 0x08 Battery Sensor (crsf.md:307).
 *
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Battery state to encode; note the deliberate unit deviation
 *            documented at @ref crsf_battery_t.
 * @return CRSF_PAYLOAD_SIZE_BATTERY.
 */
size_t crsf_encode_battery(uint8_t *out, const crsf_battery_t *in);

/**
 * @brief Decode 0x08 Battery Sensor.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the battery state.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_BATTERY.
 */
bool crsf_decode_battery(const uint8_t *pl, size_t len, crsf_battery_t *out);

/**
 * @brief Encode 0x09 Barometric Altitude and Vertical Speed (crsf.md:316).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Altitude and vertical speed; both are packed lossily.
 * @return CRSF_PAYLOAD_SIZE_BARO_ALTITUDE.
 */
#if CRSF_ENABLE_FLOAT_MATH
size_t crsf_encode_baro_altitude(uint8_t *out, const crsf_baro_altitude_t *in);

/**
 * @brief Decode 0x09 Barometric Altitude and Vertical Speed.
 *
 * The vertical-speed byte is treated as optional: some senders emit only the
 * 2-byte altitude, and crsf.md:177 forbids reading an absent optional field. A
 * 2-byte payload therefore decodes successfully with @c out->v_speed_cm_s set
 * to 0 — which is indistinguishable from a genuine zero vertical speed.
 *
 * @param pl  Payload bytes.
 * @param len Payload length; 2 is enough, 3 carries the vertical speed.
 * @param out Receives the unpacked values.
 * @retval true  Decoded.
 * @retval false Fewer than 2 bytes.
 */
bool crsf_decode_baro_altitude(const uint8_t *pl, size_t len, crsf_baro_altitude_t *out);
#endif /* CRSF_ENABLE_FLOAT_MATH */

/**
 * @brief Encode 0x0A Airspeed (crsf.md:380).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Airspeed to encode.
 * @return CRSF_PAYLOAD_SIZE_AIRSPEED.
 */
size_t crsf_encode_airspeed(uint8_t *out, const crsf_airspeed_t *in);

/**
 * @brief Decode 0x0A Airspeed.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the airspeed.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_AIRSPEED.
 */
bool crsf_decode_airspeed(const uint8_t *pl, size_t len, crsf_airspeed_t *out);

/**
 * @brief Encode 0x0B Heartbeat (crsf.md:386).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Origin address to announce.
 * @return CRSF_PAYLOAD_SIZE_HEARTBEAT.
 */
size_t crsf_encode_heartbeat(uint8_t *out, const crsf_heartbeat_t *in);

/**
 * @brief Decode 0x0B Heartbeat.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the origin address.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_HEARTBEAT.
 */
bool crsf_decode_heartbeat(const uint8_t *pl, size_t len, crsf_heartbeat_t *out);

/**
 * @brief Encode 0x0C RPM (crsf.md:392). Variable length: 1 + 3 * count.
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Source id and up to CRSF_RPM_MAX_VALUES values.
 * @return Payload size, or 0 when @c in->count is 0 or above the maximum.
 */
size_t crsf_encode_rpm(uint8_t *out, const crsf_rpm_t *in);

/**
 * @brief Decode 0x0C RPM. The value count follows from the payload length.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives source id, count and values.
 * @retval true  Decoded.
 * @retval false Too short to hold a source id and one value.
 */
bool crsf_decode_rpm(const uint8_t *pl, size_t len, crsf_rpm_t *out);

/**
 * @brief Encode 0x0D Temperature (crsf.md:401). Variable length: 1 + 2 * count.
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Source id and up to CRSF_TEMP_MAX_VALUES values.
 * @return Payload size, or 0 when @c in->count is 0 or above the maximum.
 */
size_t crsf_encode_temp(uint8_t *out, const crsf_temp_t *in);

/**
 * @brief Decode 0x0D Temperature. The value count follows from the payload length.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives source id, count and values.
 * @retval true  Decoded.
 * @retval false Too short to hold a source id and one value.
 */
bool crsf_decode_temp(const uint8_t *pl, size_t len, crsf_temp_t *out);

/**
 * @brief Encode 0x0E Voltages (crsf.md:415). Variable length: 1 + 2 * count.
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Source id and up to CRSF_VOLTAGES_MAX_VALUES values.
 * @return Payload size, or 0 when @c in->count is 0 or above the maximum.
 */
size_t crsf_encode_voltages(uint8_t *out, const crsf_voltages_t *in);

/**
 * @brief Decode 0x0E Voltages. The value count follows from the payload length.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives source id, count and values.
 * @retval true  Decoded.
 * @retval false Too short to hold a source id and one value.
 */
bool crsf_decode_voltages(const uint8_t *pl, size_t len, crsf_voltages_t *out);

/**
 * @brief Encode 0x10 VTX Telemetry (crsf.md:431).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  VTX state; the pitmode fields are packed LSB-first.
 * @return CRSF_PAYLOAD_SIZE_VTX.
 */
size_t crsf_encode_vtx_telemetry(uint8_t *out, const crsf_vtx_telemetry_t *in);

/**
 * @brief Decode 0x10 VTX Telemetry.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the VTX state.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_VTX.
 */
bool crsf_decode_vtx_telemetry(const uint8_t *pl, size_t len, crsf_vtx_telemetry_t *out);

/**
 * @brief Encode 0x11 Barometer (crsf.md:442).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Pressure and temperature to encode.
 * @return CRSF_PAYLOAD_SIZE_BAROMETER.
 */
size_t crsf_encode_barometer(uint8_t *out, const crsf_barometer_t *in);

/**
 * @brief Decode 0x11 Barometer.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the values.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_BAROMETER.
 */
bool crsf_decode_barometer(const uint8_t *pl, size_t len, crsf_barometer_t *out);

/**
 * @brief Encode 0x12 Magnetometer (crsf.md:449).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Field vector to encode.
 * @return CRSF_PAYLOAD_SIZE_MAGNETOMETER.
 */
size_t crsf_encode_magnetometer(uint8_t *out, const crsf_magnetometer_t *in);

/**
 * @brief Decode 0x12 Magnetometer.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the field vector.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_MAGNETOMETER.
 */
bool crsf_decode_magnetometer(const uint8_t *pl, size_t len, crsf_magnetometer_t *out);

/**
 * @brief Encode 0x13 Accel Gyro (crsf.md:457).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  IMU sample to encode.
 * @return CRSF_PAYLOAD_SIZE_ACCEL_GYRO.
 */
size_t crsf_encode_accel_gyro(uint8_t *out, const crsf_accel_gyro_t *in);

/**
 * @brief Decode 0x13 Accel Gyro.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the IMU sample.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_ACCEL_GYRO.
 */
bool crsf_decode_accel_gyro(const uint8_t *pl, size_t len, crsf_accel_gyro_t *out);

/**
 * @brief Encode 0x14 Link Statistics (crsf.md:481).
 *
 * The same payload serves 0x15 Link Statistics Repeater; only the frame type
 * differs.
 *
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Link quality figures to encode.
 * @return CRSF_PAYLOAD_SIZE_LINK_STATS.
 */
size_t crsf_encode_link_statistics(uint8_t *out, const crsf_link_statistics_t *in);

/**
 * @brief Decode 0x14 or 0x15 Link Statistics.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the link quality figures.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_LINK_STATS.
 */
bool crsf_decode_link_statistics(const uint8_t *pl, size_t len, crsf_link_statistics_t *out);

/**
 * @brief Encode 0x1C Link Statistics RX (crsf.md:579).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Receiver-side figures to encode.
 * @return CRSF_PAYLOAD_SIZE_LINK_STATS_RX.
 */
size_t crsf_encode_link_statistics_rx(uint8_t *out, const crsf_link_statistics_rx_t *in);

/**
 * @brief Decode 0x1C Link Statistics RX.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the figures.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_LINK_STATS_RX.
 */
bool crsf_decode_link_statistics_rx(const uint8_t *pl, size_t len, crsf_link_statistics_rx_t *out);

/**
 * @brief Encode 0x1D Link Statistics TX (crsf.md:589).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Transmitter-side figures to encode.
 * @return CRSF_PAYLOAD_SIZE_LINK_STATS_TX.
 */
size_t crsf_encode_link_statistics_tx(uint8_t *out, const crsf_link_statistics_tx_t *in);

/**
 * @brief Decode 0x1D Link Statistics TX.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the figures.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_LINK_STATS_TX.
 */
bool crsf_decode_link_statistics_tx(const uint8_t *pl, size_t len, crsf_link_statistics_tx_t *out);

/**
 * @brief Encode 0x1E Attitude (crsf.md:600).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Angles to encode; not clamped, see @ref crsf_attitude_t.
 * @return CRSF_PAYLOAD_SIZE_ATTITUDE.
 */
size_t crsf_encode_attitude(uint8_t *out, const crsf_attitude_t *in);

/**
 * @brief Decode 0x1E Attitude.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the angles.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_ATTITUDE.
 */
bool crsf_decode_attitude(const uint8_t *pl, size_t len, crsf_attitude_t *out);

/**
 * @brief Encode 0x1F MAVLink FC (crsf.md:611).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Autopilot mode summary to encode.
 * @return CRSF_PAYLOAD_SIZE_MAVLINK_FC.
 */
size_t crsf_encode_mavlink_fc(uint8_t *out, const crsf_mavlink_fc_t *in);

/**
 * @brief Decode 0x1F MAVLink FC.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the mode summary.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_MAVLINK_FC.
 */
bool crsf_decode_mavlink_fc(const uint8_t *pl, size_t len, crsf_mavlink_fc_t *out);

/**
 * @brief Encode 0x21 Flight Mode (crsf.md:627). Variable length.
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Mode name; truncated to CRSF_FLIGHT_MODE_MAX_LEN - 1 characters.
 * @return Payload size including the NUL terminator.
 */
size_t crsf_encode_flight_mode(uint8_t *out, const crsf_flight_mode_t *in);

/**
 * @brief Decode 0x21 Flight Mode.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives a NUL-terminated mode name even if the wire data was not.
 * @retval true  Decoded.
 * @retval false Empty payload.
 */
bool crsf_decode_flight_mode(const uint8_t *pl, size_t len, crsf_flight_mode_t *out);

/**
 * @brief Encode 0x29 Device Information (crsf.md:653). Variable length.
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Device identity and parameter count.
 * @return Payload size.
 */
size_t crsf_encode_device_info(uint8_t *out, const crsf_device_info_t *in);

/**
 * @brief Decode 0x29 Device Information.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the identity; the name is always NUL-terminated.
 * @retval true  Decoded.
 * @retval false The name runs to the end of the payload without a terminator, or
 *               the payload is too short to hold the name plus the fixed fields.
 */
bool crsf_decode_device_info(const uint8_t *pl, size_t len, crsf_device_info_t *out);

/**
 * @brief Encode 0x3A.0x10 Timing Correction (crsf.md:1173).
 *
 * The sub-type byte is part of the payload, which is why the size is 9 rather
 * than 8.
 *
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Interval and offset to encode.
 * @return CRSF_PAYLOAD_SIZE_TIMING_CORR.
 */
size_t crsf_encode_timing_correction(uint8_t *out, const crsf_timing_correction_t *in);

/**
 * @brief Decode 0x3A.0x10 Timing Correction.
 * @param pl  Payload bytes, sub-type byte first.
 * @param len Payload length.
 * @param out Receives interval and offset.
 * @retval true  Decoded.
 * @retval false Too short, or the sub-type is not
 *               CRSF_REMOTE_SUBTYPE_TIMING_CORRECTION.
 */
bool crsf_decode_timing_correction(const uint8_t *pl, size_t len, crsf_timing_correction_t *out);

/** @} */

/**
 * @name 0x17 / 0x18 Subset RC Channels
 * @{
 */

/**
 * @brief Encode a 0x17 Subset RC Channels payload (crsf.md:548).
 *
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Channels, resolution and starting channel number.
 * @return Payload size, or 0 when @c in->channel_count is 0, exceeds
 *         CRSF_SUBSET_MAX_CHANNELS, or would not fit one frame at the requested
 *         resolution.
 *
 * @warning The frame is discouraged by the spec; prefer 0x16.
 */
size_t crsf_encode_subset_channels(uint8_t *out, const crsf_subset_channels_t *in);

/**
 * @brief Decode a 0x17 payload.
 *
 * The channel count is not transmitted: it follows from the frame length and the
 * resolution (crsf.md:565). Digital switch channels are never read -- see the
 * warning below; this is a documented limitation, not an oversight.
 *
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the header fields and all readable channels.
 * @retval true  Decoded.
 * @retval false Fewer than 2 bytes, or the length and resolution work out to
 *               zero complete channels.
 *
 * @warning When the digital-switch flag is set, analogue and switch channels
 *          share one bit budget and the split cannot be recovered from the frame
 *          alone. All readable channels are reported and @c out->switch_count is
 *          left at 0. See COMPLIANCE.md §7.
 */
bool crsf_decode_subset_channels(const uint8_t *pl, size_t len,
                                 crsf_subset_channels_t *out);

/**
 * @brief Encode a 0x18 payload: the 0x16 bit layout with 0x17 scaling (crsf.md:573).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Channels to encode.
 * @return CRSF_CHANNELS_PAYLOAD_SIZE.
 */
size_t crsf_encode_channels_11bit(uint8_t *out, const crsf_channels_t *in);

/**
 * @brief Decode a 0x18 payload.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the channels.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_CHANNELS_PAYLOAD_SIZE.
 */
bool crsf_decode_channels_11bit(const uint8_t *pl, size_t len, crsf_channels_t *out);

/** @} */

/**
 * @name 0x22 / 0x34 / 0x3C / 0xAC / 0xAA
 * @{
 */

/**
 * @brief Encode 0x22 ESP_NOW (crsf.md:633).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Race message; strings are padded, not terminated, on the wire.
 * @return CRSF_PAYLOAD_SIZE_ESP_NOW.
 */
size_t crsf_encode_esp_now(uint8_t *out, const crsf_esp_now_t *in);

/**
 * @brief Decode 0x22 ESP_NOW.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the message; all strings are NUL-terminated here.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_ESP_NOW.
 */
bool crsf_decode_esp_now(const uint8_t *pl, size_t len, crsf_esp_now_t *out);

/**
 * @brief Encode 0x34 Logging (crsf.md:1152). Variable length.
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Event id, timestamp and up to CRSF_LOG_MAX_PARAMS values.
 * @return Payload size, or 0 when the parameter count exceeds the maximum.
 */
size_t crsf_encode_logging(uint8_t *out, const crsf_logging_t *in);

/**
 * @brief Decode 0x34 Logging. The parameter count follows from the payload length.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the event.
 * @retval true  Decoded.
 * @retval false Too short to hold the event id and timestamp.
 */
bool crsf_decode_logging(const uint8_t *pl, size_t len, crsf_logging_t *out);

/**
 * @brief Encode 0x3C Game (crsf.md:1185).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Sub-type and its value.
 * @return Payload size, or 0 for an unknown sub-type.
 */
size_t crsf_encode_game(uint8_t *out, const crsf_game_t *in);

/**
 * @brief Decode 0x3C Game.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives sub-type and value.
 * @retval true  Decoded.
 * @retval false Too short, or an unknown sub-type.
 */
bool crsf_decode_game(const uint8_t *pl, size_t len, crsf_game_t *out);

/**
 * @brief Encode 0xAC MAVLink System Status (crsf.md:1324).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  The three sensor bitmasks.
 * @return CRSF_PAYLOAD_SIZE_MAVLINK_STATUS.
 */
size_t crsf_encode_mavlink_status(uint8_t *out, const crsf_mavlink_status_t *in);

/**
 * @brief Decode 0xAC MAVLink System Status.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the bitmasks.
 * @retval true  Decoded.
 * @retval false Shorter than CRSF_PAYLOAD_SIZE_MAVLINK_STATUS.
 */
bool crsf_decode_mavlink_status(const uint8_t *pl, size_t len, crsf_mavlink_status_t *out);

/**
 * @brief Encode one 0xAA MAVLink Envelope chunk (crsf.md:1295).
 *
 * Only the chunk itself is encoded; splitting a frame into chunks is
 * crsf_mavlink_split_next()'s job.
 *
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  One chunk, indices already assigned.
 * @return Payload size (2 + data_size), or 0 when @c in->data_size exceeds
 *         CRSF_MAVLINK_CHUNK_MAX.
 */
size_t crsf_encode_mavlink_envelope(uint8_t *out, const crsf_mavlink_envelope_t *in);

/**
 * @brief Decode one 0xAA MAVLink Envelope chunk.
 * @param pl  Payload bytes.
 * @param len Payload length.
 * @param out Receives the chunk, ready for crsf_mavlink_reasm_push().
 * @retval true  Decoded.
 * @retval false Too short, or the declared data size exceeds what is present.
 */
bool crsf_decode_mavlink_envelope(const uint8_t *pl, size_t len,
                                  crsf_mavlink_envelope_t *out);

/** @} */

/**
 * @name 0x32 command payloads
 * These encode the *arguments* of a direct command. The command id, sub id and
 * the two CRCs are added by crsf_build_command().
 * @{
 */

/**
 * @brief Pack an HSV colour into 3 bytes (crsf.md:1026).
 *
 * MSB-first: hue in the top 9 bits.
 *
 * @param out Three writable bytes.
 * @param in  Colour to pack.
 *
 * @warning The bit order is not specified by the spec. See the warning on
 *          @ref crsf_hsv_t.
 */
void crsf_pack_hsv(uint8_t out[3], const crsf_hsv_t *in);

/**
 * @brief Unpack 3 bytes into an HSV colour.
 * @param in  Three readable bytes.
 * @param out Receives the colour.
 */
void crsf_unpack_hsv(const uint8_t in[3], crsf_hsv_t *out);

/**
 * @brief Pack the 0x32.0x08.0x04 PitMode configuration byte (crsf.md:1008).
 * @param in Configuration to pack.
 * @return The packed byte, LSB-first.
 */
uint8_t crsf_pack_vtx_pitmode(const crsf_vtx_pitmode_t *in);

/**
 * @brief Unpack the 0x32.0x08.0x04 PitMode configuration byte.
 * @param b   The packed byte.
 * @param out Receives the configuration.
 */
void crsf_unpack_vtx_pitmode(uint8_t b, crsf_vtx_pitmode_t *out);

/**
 * @brief Encode the arguments of a 0x32.0x22.0x01 pop-up message (crsf.md:1113).
 *
 * Optional sections are emitted only when the corresponding @c has_* flag is
 * set. When @c in->has_add_data is false a single NUL is written for the empty
 * selection text, which is the shortest legal form (crsf.md:1120).
 *
 * @param out      Destination buffer.
 * @param out_size Capacity of @p out.
 * @param in       Message to encode.
 * @return Argument size, or 0 if the result would not fit @p out_size or one
 *         CRSF frame.
 */
size_t crsf_encode_popup_message(uint8_t *out, size_t out_size,
                                 const crsf_popup_message_t *in);

/**
 * @brief Decode a pop-up message, tolerating truncation of the optional parts.
 *
 * @param args Argument bytes, after the command and sub-command ids.
 * @param len  Argument length.
 * @param out  Receives the message; fields belonging to an absent optional
 *             section are left zeroed.
 * @retval true  The mandatory fields were complete.
 * @retval false The mandatory fields are incomplete. Missing *optional* sections
 *               are reported via @c out->has_add_data and
 *               @c out->has_possible_values rather than as an error, which is
 *               what crsf.md:1139 requires.
 */
bool crsf_decode_popup_message(const uint8_t *args, size_t len,
                               crsf_popup_message_t *out);

/**
 * @brief Encode the arguments of a 0x32.0x22.0x02 selection return (crsf.md:1141).
 * @param out Destination, CRSF_MAX_PAYLOAD_SIZE bytes.
 * @param in  Value and process/cancel flag.
 * @return Argument size.
 */
size_t crsf_encode_popup_response(uint8_t *out, const crsf_popup_response_t *in);

/**
 * @brief Decode a 0x32.0x22.0x02 selection return.
 * @param args Argument bytes.
 * @param len  Argument length.
 * @param out  Receives value and flag.
 * @retval true  Decoded.
 * @retval false Too short.
 */
bool crsf_decode_popup_response(const uint8_t *args, size_t len,
                                crsf_popup_response_t *out);

/** @} */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CRSF_CODEC_H */
