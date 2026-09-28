/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_codec.c
 * @brief CRSF payload encoders/decoders.
 *
 * All multi-byte fields go through the explicit crsf_get_beN / crsf_put_beN
 * helpers. Decoders check a per-type minimum length before touching any field
 * and tolerate extra trailing bytes (crsf.md:175).
 */

#include "crsf_codec.h"
#include "crsf_bits.h"
#include <string.h>
#include <math.h>

/**
 * @brief Clamp helper used by the packed-altitude and vertical-speed conversions.
 * @param v  Value to clamp.
 * @param lo Lower bound, inclusive.
 * @param hi Upper bound, inclusive.
 * @return @p v confined to [@p lo, @p hi].
 */
static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/**
 * @brief Bounded string length.
 *
 * strnlen() is not exposed under a strict `-std=c11` feature set on every
 * toolchain, and we only ever need it over small fixed buffers, so keep it local.
 *
 * @param s   String to measure; need not be NUL-terminated within @p max.
 * @param max Bytes to inspect at most.
 * @return Characters before the first NUL, or @p max if there is none.
 */
static size_t crsf_strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* 0x16 RC Channels                                                          */
/* ------------------------------------------------------------------------- */

void crsf_channels_unpack(const uint8_t *payload, crsf_channels_t *out)
{
    crsf_bitreader_t r;
    crsf_br_init(&r, payload, CRSF_CHANNELS_PAYLOAD_SIZE);
    for (int ch = 0; ch < CRSF_NUM_CHANNELS; ch++) {
        out->channel[ch] = (uint16_t)crsf_br_get_lsb(&r, 11);
    }
}

void crsf_channels_pack(uint8_t *out, const crsf_channels_t *in)
{
    crsf_bitwriter_t w;
    crsf_bw_init(&w, out, CRSF_CHANNELS_PAYLOAD_SIZE);
    for (int ch = 0; ch < CRSF_NUM_CHANNELS; ch++) {
        crsf_bw_put_lsb(&w, in->channel[ch] & 0x7FF, 11);
    }
    /* 16 * 11 == 176 bits is byte-aligned, so nothing is left over. */
}

size_t crsf_encode_channels(uint8_t *out, const crsf_channels_t *in)
{
    crsf_channels_pack(out, in);
    return CRSF_CHANNELS_PAYLOAD_SIZE;
}

bool crsf_decode_channels(const uint8_t *pl, size_t len, crsf_channels_t *out)
{
    if (len < CRSF_CHANNELS_PAYLOAD_SIZE) {
        return false;
    }
    crsf_channels_unpack(pl, out);
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x09 packed altitude / vertical speed (crsf.md:316-378)                    */
/* ------------------------------------------------------------------------- */

/** Offset applied in decimetre mode, so -1000 m is the lowest value (crsf.md:334). */
#define CRSF_ALT_MIN_DM 10000

/** Altitude above which the field switches to whole metres. Equals 22768. */
#define CRSF_ALT_THRESHOLD_DM (0x8000 - CRSF_ALT_MIN_DM)

/** Highest representable altitude in decimetres. Equals 327655. */
#define CRSF_ALT_MAX_DM (0x7ffe * 10 - 5)

/** Vertical-speed curve: linear scale in cm/s (crsf.md:360-362). */
#define CRSF_VS_KL 100.0

/** Vertical-speed curve: exponential rate per wire count (crsf.md:360-362). */
#define CRSF_VS_KR 0.026

int32_t crsf_altitude_unpack(uint16_t packed)
{
    /* MSB set means "metres, no offset"; clear means "decimetres - 10000dm". */
    return (packed & 0x8000) ? (int32_t)(packed & 0x7fff) * 10
                             : (int32_t)packed - CRSF_ALT_MIN_DM;
}

uint16_t crsf_altitude_pack(int32_t altitude_dm)
{
    if (altitude_dm < -CRSF_ALT_MIN_DM) {
        return 0;
    }
    if (altitude_dm > CRSF_ALT_MAX_DM) {
        /* 0xFFFF reads as "invalid" in OpenTX, so the spec caps at 0xFFFE. */
        return 0xfffe;
    }
    if (altitude_dm < CRSF_ALT_THRESHOLD_DM) {
        return (uint16_t)(altitude_dm + CRSF_ALT_MIN_DM);
    }
    return (uint16_t)(((altitude_dm + 5) / 10) | 0x8000);
}

int16_t crsf_vspeed_unpack(int8_t packed)
{
    const int sign = (packed < 0) ? -1 : 1;
    const double mag = fabs((double)packed * CRSF_VS_KR);
    const double cm_s = (exp(mag) - 1.0) * CRSF_VS_KL;
    /*
     * The clamp cannot currently fire: the widest int8 input, -128, yields about
     * -2690 cm/s, nowhere near the int16 limits. It stays as a guard on the cast
     * so that a later change to KL or KR cannot turn an overflow into undefined
     * behaviour. Do not write a test expecting to reach it.
     */
    return (int16_t)clamp_i32((int32_t)(cm_s * sign), -32768, 32767);
}

int8_t crsf_vspeed_pack(int16_t v_cm_s)
{
    const int sign = (v_cm_s < 0) ? -1 : 1;
    const double mag = fabs((double)v_cm_s);
    const double packed = log(mag / CRSF_VS_KL + 1.0) / CRSF_VS_KR;
    return (int8_t)clamp_i32((int32_t)packed * sign, -128, 127);
}

/* ------------------------------------------------------------------------- */
/* 0x02 GPS                                                                  */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_gps(uint8_t *out, const crsf_gps_t *in)
{
    crsf_put_be32(&out[0], (uint32_t)in->latitude);
    crsf_put_be32(&out[4], (uint32_t)in->longitude);
    crsf_put_be16(&out[8], in->groundspeed);
    crsf_put_be16(&out[10], in->heading);
    crsf_put_be16(&out[12], in->altitude);
    out[14] = in->satellites;
    return CRSF_PAYLOAD_SIZE_GPS;
}

bool crsf_decode_gps(const uint8_t *pl, size_t len, crsf_gps_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_GPS) {
        return false;
    }
    out->latitude = (int32_t)crsf_get_be32(&pl[0]);
    out->longitude = (int32_t)crsf_get_be32(&pl[4]);
    out->groundspeed = crsf_get_be16(&pl[8]);
    out->heading = crsf_get_be16(&pl[10]);
    out->altitude = crsf_get_be16(&pl[12]);
    out->satellites = pl[14];
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x03 GPS Time                                                             */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_gps_time(uint8_t *out, const crsf_gps_time_t *in)
{
    crsf_put_be16(&out[0], (uint16_t)in->year);
    out[2] = in->month;
    out[3] = in->day;
    out[4] = in->hour;
    out[5] = in->minute;
    out[6] = in->second;
    crsf_put_be16(&out[7], in->millisecond);
    return CRSF_PAYLOAD_SIZE_GPS_TIME;
}

bool crsf_decode_gps_time(const uint8_t *pl, size_t len, crsf_gps_time_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_GPS_TIME) {
        return false;
    }
    out->year = (int16_t)crsf_get_be16(&pl[0]);
    out->month = pl[2];
    out->day = pl[3];
    out->hour = pl[4];
    out->minute = pl[5];
    out->second = pl[6];
    out->millisecond = crsf_get_be16(&pl[7]);
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x06 GPS Extended                                                         */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_gps_extended(uint8_t *out, const crsf_gps_extended_t *in)
{
    out[0] = in->fix_type;
    crsf_put_be16(&out[1], (uint16_t)in->n_speed);
    crsf_put_be16(&out[3], (uint16_t)in->e_speed);
    crsf_put_be16(&out[5], (uint16_t)in->v_speed);
    crsf_put_be16(&out[7], (uint16_t)in->h_speed_acc);
    crsf_put_be16(&out[9], (uint16_t)in->track_acc);
    crsf_put_be16(&out[11], (uint16_t)in->alt_ellipsoid);
    crsf_put_be16(&out[13], (uint16_t)in->h_acc);
    crsf_put_be16(&out[15], (uint16_t)in->v_acc);
    out[17] = in->reserved;
    out[18] = in->hdop;
    out[19] = in->vdop;
    return CRSF_PAYLOAD_SIZE_GPS_EXTENDED;
}

bool crsf_decode_gps_extended(const uint8_t *pl, size_t len, crsf_gps_extended_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_GPS_EXTENDED) {
        return false;
    }
    out->fix_type = pl[0];
    out->n_speed = (int16_t)crsf_get_be16(&pl[1]);
    out->e_speed = (int16_t)crsf_get_be16(&pl[3]);
    out->v_speed = (int16_t)crsf_get_be16(&pl[5]);
    out->h_speed_acc = (int16_t)crsf_get_be16(&pl[7]);
    out->track_acc = (int16_t)crsf_get_be16(&pl[9]);
    out->alt_ellipsoid = (int16_t)crsf_get_be16(&pl[11]);
    out->h_acc = (int16_t)crsf_get_be16(&pl[13]);
    out->v_acc = (int16_t)crsf_get_be16(&pl[15]);
    out->reserved = pl[17];
    out->hdop = pl[18];
    out->vdop = pl[19];
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x07 Variometer                                                           */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_vario(uint8_t *out, const crsf_vario_t *in)
{
    crsf_put_be16(&out[0], (uint16_t)in->v_speed);
    return CRSF_PAYLOAD_SIZE_VARIO;
}

bool crsf_decode_vario(const uint8_t *pl, size_t len, crsf_vario_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_VARIO) {
        return false;
    }
    out->v_speed = (int16_t)crsf_get_be16(&pl[0]);
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x08 Battery                                                              */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_battery(uint8_t *out, const crsf_battery_t *in)
{
    crsf_put_be16(&out[0], (uint16_t)in->voltage);
    crsf_put_be16(&out[2], (uint16_t)in->current);
    /*
     * True 24-bit big-endian. The old code did `__bswap16(capacity) << 8` on a
     * 24-bit bitfield, which dropped the most significant byte for anything at
     * or above 65536 mAh.
     */
    /* Saturate rather than mask: masking wraps 16777216 mAh back to 0. */
    crsf_put_be24(&out[4], in->capacity_used > 0xFFFFFFu ? 0xFFFFFFu
                                                         : in->capacity_used);
    out[7] = in->remaining;
    return CRSF_PAYLOAD_SIZE_BATTERY;
}

bool crsf_decode_battery(const uint8_t *pl, size_t len, crsf_battery_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_BATTERY) {
        return false;
    }
    out->voltage = (int16_t)crsf_get_be16(&pl[0]);
    out->current = (int16_t)crsf_get_be16(&pl[2]);
    out->capacity_used = crsf_get_be24(&pl[4]);
    out->remaining = pl[7];
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x09 Barometric Altitude & Vertical Speed                                 */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_baro_altitude(uint8_t *out, const crsf_baro_altitude_t *in)
{
    crsf_put_be16(&out[0], crsf_altitude_pack(in->altitude_dm));
    out[2] = (uint8_t)crsf_vspeed_pack(in->v_speed_cm_s);
    return CRSF_PAYLOAD_SIZE_BARO_ALTITUDE;
}

bool crsf_decode_baro_altitude(const uint8_t *pl, size_t len, crsf_baro_altitude_t *out)
{
    /*
     * Some senders emit only the 2-byte altitude without the vertical-speed
     * byte. crsf.md:177 says not to read optional fields beyond the payload,
     * so accept the short form and report zero vertical speed.
     */
    if (len < 2) {
        return false;
    }
    out->altitude_dm = crsf_altitude_unpack(crsf_get_be16(&pl[0]));
    out->v_speed_cm_s = (len >= 3) ? crsf_vspeed_unpack((int8_t)pl[2]) : 0;
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x0A Airspeed / 0x0B Heartbeat                                            */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_airspeed(uint8_t *out, const crsf_airspeed_t *in)
{
    crsf_put_be16(&out[0], in->speed);
    return CRSF_PAYLOAD_SIZE_AIRSPEED;
}

bool crsf_decode_airspeed(const uint8_t *pl, size_t len, crsf_airspeed_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_AIRSPEED) {
        return false;
    }
    out->speed = crsf_get_be16(&pl[0]);
    return true;
}

size_t crsf_encode_heartbeat(uint8_t *out, const crsf_heartbeat_t *in)
{
    crsf_put_be16(&out[0], (uint16_t)in->origin_address);
    return CRSF_PAYLOAD_SIZE_HEARTBEAT;
}

bool crsf_decode_heartbeat(const uint8_t *pl, size_t len, crsf_heartbeat_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_HEARTBEAT) {
        return false;
    }
    out->origin_address = (int16_t)crsf_get_be16(&pl[0]);
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x0C RPM — variable count of int24 values (crsf.md:392)                    */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_rpm(uint8_t *out, const crsf_rpm_t *in)
{
    /*
     * Refuse rather than invent a count. A caller passing 0 has almost
     * certainly not filled the array either, and rounding up to one emitted
     * rpm[0] -- a slot the caller had no reason to set -- as real telemetry.
     * A count past the maximum is refused too rather than silently sending
     * fewer values than asked for. Both are what crsf_codec.h promises, and
     * every call site already treats 0 as "could not encode".
     */
    const uint8_t n = in->count;
    if (n == 0 || n > CRSF_RPM_MAX_VALUES) {
        return 0;
    }

    out[0] = in->source_id;
    for (uint8_t i = 0; i < n; i++) {
        /*
         * rpm[] is int32_t but the wire field is int24. Masking alone inverts
         * the sign of anything out of range — 8388608 masks to 0x800000, which
         * crsf_get_be24_signed() reads back as -8388608, turning a high RPM
         * into a large negative one. Clamp to the int24 range instead.
         */
        const int32_t v = clamp_i32(in->rpm[i], -8388608, 8388607);
        crsf_put_be24(&out[1 + i * 3], (uint32_t)v & 0xFFFFFFu);
    }
    return (size_t)(1 + n * 3);
}

bool crsf_decode_rpm(const uint8_t *pl, size_t len, crsf_rpm_t *out)
{
    if (len < 4) { /* source id + at least one int24 */
        return false;
    }
    size_t n = (len - 1) / 3;
    if (n > CRSF_RPM_MAX_VALUES) {
        n = CRSF_RPM_MAX_VALUES;
    }

    memset(out, 0, sizeof(*out));
    out->source_id = pl[0];
    out->count = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        out->rpm[i] = crsf_get_be24_signed(&pl[1 + i * 3]);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x0D Temperature — variable count of int16 (crsf.md:401)                   */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_temp(uint8_t *out, const crsf_temp_t *in)
{
    /* See crsf_encode_rpm() on why 0 and over-max are refused, not adjusted. */
    const uint8_t n = in->count;
    if (n == 0 || n > CRSF_TEMP_MAX_VALUES) {
        return 0;
    }

    out[0] = in->source_id;
    for (uint8_t i = 0; i < n; i++) {
        crsf_put_be16(&out[1 + i * 2], (uint16_t)in->temperature[i]);
    }
    return (size_t)(1 + n * 2);
}

bool crsf_decode_temp(const uint8_t *pl, size_t len, crsf_temp_t *out)
{
    if (len < 3) {
        return false;
    }
    size_t n = (len - 1) / 2;
    if (n > CRSF_TEMP_MAX_VALUES) {
        n = CRSF_TEMP_MAX_VALUES;
    }

    memset(out, 0, sizeof(*out));
    out->source_id = pl[0];
    out->count = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        out->temperature[i] = (int16_t)crsf_get_be16(&pl[1 + i * 2]);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x0E Voltages — variable count of uint16 (crsf.md:415)                     */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_voltages(uint8_t *out, const crsf_voltages_t *in)
{
    /* See crsf_encode_rpm() on why 0 and over-max are refused, not adjusted. */
    const uint8_t n = in->count;
    if (n == 0 || n > CRSF_VOLTAGES_MAX_VALUES) {
        return 0;
    }

    out[0] = in->source_id;
    for (uint8_t i = 0; i < n; i++) {
        crsf_put_be16(&out[1 + i * 2], in->voltage[i]);
    }
    return (size_t)(1 + n * 2);
}

bool crsf_decode_voltages(const uint8_t *pl, size_t len, crsf_voltages_t *out)
{
    if (len < 3) {
        return false;
    }
    size_t n = (len - 1) / 2;
    if (n > CRSF_VOLTAGES_MAX_VALUES) {
        n = CRSF_VOLTAGES_MAX_VALUES;
    }

    memset(out, 0, sizeof(*out));
    out->source_id = pl[0];
    out->count = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        out->voltage[i] = crsf_get_be16(&pl[1 + i * 2]);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x10 VTX Telemetry                                                        */
/* ------------------------------------------------------------------------- */
/*
 * Byte 4 packs pit_mode:1, pitmode_control:2, pitmode_switch:4 (crsf.md:437).
 * The spec never states the bit order for this field, so we use LSB-first,
 * matching how the same three fields are treated in 0x32.0x08.0x04.
 */

size_t crsf_encode_vtx_telemetry(uint8_t *out, const crsf_vtx_telemetry_t *in)
{
    out[0] = in->origin_address;
    out[1] = in->power_dbm;
    crsf_put_be16(&out[2], in->frequency_mhz);
    out[4] = (uint8_t)((in->pit_mode & 0x01) |
                       ((in->pitmode_control & 0x03) << 1) |
                       ((in->pitmode_switch & 0x0F) << 3));
    return CRSF_PAYLOAD_SIZE_VTX;
}

bool crsf_decode_vtx_telemetry(const uint8_t *pl, size_t len, crsf_vtx_telemetry_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_VTX) {
        return false;
    }
    out->origin_address = pl[0];
    out->power_dbm = pl[1];
    out->frequency_mhz = crsf_get_be16(&pl[2]);
    out->pit_mode = pl[4] & 0x01;
    out->pitmode_control = (pl[4] >> 1) & 0x03;
    out->pitmode_switch = (pl[4] >> 3) & 0x0F;
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x11 Barometer / 0x12 Magnetometer / 0x13 Accel Gyro                       */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_barometer(uint8_t *out, const crsf_barometer_t *in)
{
    crsf_put_be32(&out[0], (uint32_t)in->pressure_pa);
    crsf_put_be32(&out[4], (uint32_t)in->baro_temp);
    return CRSF_PAYLOAD_SIZE_BAROMETER;
}

bool crsf_decode_barometer(const uint8_t *pl, size_t len, crsf_barometer_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_BAROMETER) {
        return false;
    }
    out->pressure_pa = (int32_t)crsf_get_be32(&pl[0]);
    out->baro_temp = (int32_t)crsf_get_be32(&pl[4]);
    return true;
}

size_t crsf_encode_magnetometer(uint8_t *out, const crsf_magnetometer_t *in)
{
    crsf_put_be16(&out[0], (uint16_t)in->field_x);
    crsf_put_be16(&out[2], (uint16_t)in->field_y);
    crsf_put_be16(&out[4], (uint16_t)in->field_z);
    return CRSF_PAYLOAD_SIZE_MAGNETOMETER;
}

bool crsf_decode_magnetometer(const uint8_t *pl, size_t len, crsf_magnetometer_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_MAGNETOMETER) {
        return false;
    }
    out->field_x = (int16_t)crsf_get_be16(&pl[0]);
    out->field_y = (int16_t)crsf_get_be16(&pl[2]);
    out->field_z = (int16_t)crsf_get_be16(&pl[4]);
    return true;
}

size_t crsf_encode_accel_gyro(uint8_t *out, const crsf_accel_gyro_t *in)
{
    crsf_put_be32(&out[0], in->sample_time);
    crsf_put_be16(&out[4], (uint16_t)in->gyro_x);
    crsf_put_be16(&out[6], (uint16_t)in->gyro_y);
    crsf_put_be16(&out[8], (uint16_t)in->gyro_z);
    crsf_put_be16(&out[10], (uint16_t)in->acc_x);
    crsf_put_be16(&out[12], (uint16_t)in->acc_y);
    crsf_put_be16(&out[14], (uint16_t)in->acc_z);
    crsf_put_be16(&out[16], (uint16_t)in->gyro_temp);
    return CRSF_PAYLOAD_SIZE_ACCEL_GYRO;
}

bool crsf_decode_accel_gyro(const uint8_t *pl, size_t len, crsf_accel_gyro_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_ACCEL_GYRO) {
        return false;
    }
    out->sample_time = crsf_get_be32(&pl[0]);
    out->gyro_x = (int16_t)crsf_get_be16(&pl[4]);
    out->gyro_y = (int16_t)crsf_get_be16(&pl[6]);
    out->gyro_z = (int16_t)crsf_get_be16(&pl[8]);
    out->acc_x = (int16_t)crsf_get_be16(&pl[10]);
    out->acc_y = (int16_t)crsf_get_be16(&pl[12]);
    out->acc_z = (int16_t)crsf_get_be16(&pl[14]);
    out->gyro_temp = (int16_t)crsf_get_be16(&pl[16]);
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x14 / 0x15 Link Statistics                                               */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_link_statistics(uint8_t *out, const crsf_link_statistics_t *in)
{
    out[0] = in->up_rssi_ant1;
    out[1] = in->up_rssi_ant2;
    out[2] = in->up_link_quality;
    out[3] = (uint8_t)in->up_snr;
    out[4] = in->active_antenna;
    out[5] = in->rf_profile;
    out[6] = in->up_rf_power;
    out[7] = in->down_rssi;
    out[8] = in->down_link_quality;
    out[9] = (uint8_t)in->down_snr;
    return CRSF_PAYLOAD_SIZE_LINK_STATS;
}

bool crsf_decode_link_statistics(const uint8_t *pl, size_t len, crsf_link_statistics_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_LINK_STATS) {
        return false;
    }
    out->up_rssi_ant1 = pl[0];
    out->up_rssi_ant2 = pl[1];
    out->up_link_quality = pl[2];
    out->up_snr = (int8_t)pl[3];
    out->active_antenna = pl[4];
    out->rf_profile = pl[5];
    out->up_rf_power = pl[6];
    out->down_rssi = pl[7];
    out->down_link_quality = pl[8];
    out->down_snr = (int8_t)pl[9];
    return true;
}

size_t crsf_encode_link_statistics_rx(uint8_t *out, const crsf_link_statistics_rx_t *in)
{
    out[0] = in->rssi_db;
    out[1] = in->rssi_percent;
    out[2] = in->link_quality;
    out[3] = (uint8_t)in->snr;
    out[4] = in->rf_power_db;
    return CRSF_PAYLOAD_SIZE_LINK_STATS_RX;
}

bool crsf_decode_link_statistics_rx(const uint8_t *pl, size_t len,
                                    crsf_link_statistics_rx_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_LINK_STATS_RX) {
        return false;
    }
    out->rssi_db = pl[0];
    out->rssi_percent = pl[1];
    out->link_quality = pl[2];
    out->snr = (int8_t)pl[3];
    out->rf_power_db = pl[4];
    return true;
}

size_t crsf_encode_link_statistics_tx(uint8_t *out, const crsf_link_statistics_tx_t *in)
{
    out[0] = in->rssi_db;
    out[1] = in->rssi_percent;
    out[2] = in->link_quality;
    out[3] = (uint8_t)in->snr;
    out[4] = in->rf_power_db;
    out[5] = in->fps;
    return CRSF_PAYLOAD_SIZE_LINK_STATS_TX;
}

bool crsf_decode_link_statistics_tx(const uint8_t *pl, size_t len,
                                    crsf_link_statistics_tx_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_LINK_STATS_TX) {
        return false;
    }
    out->rssi_db = pl[0];
    out->rssi_percent = pl[1];
    out->link_quality = pl[2];
    out->snr = (int8_t)pl[3];
    out->rf_power_db = pl[4];
    out->fps = pl[5];
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x1E Attitude / 0x1F MAVLink FC                                           */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_attitude(uint8_t *out, const crsf_attitude_t *in)
{
    crsf_put_be16(&out[0], (uint16_t)in->pitch);
    crsf_put_be16(&out[2], (uint16_t)in->roll);
    crsf_put_be16(&out[4], (uint16_t)in->yaw);
    return CRSF_PAYLOAD_SIZE_ATTITUDE;
}

bool crsf_decode_attitude(const uint8_t *pl, size_t len, crsf_attitude_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_ATTITUDE) {
        return false;
    }
    out->pitch = (int16_t)crsf_get_be16(&pl[0]);
    out->roll = (int16_t)crsf_get_be16(&pl[2]);
    out->yaw = (int16_t)crsf_get_be16(&pl[4]);
    return true;
}

size_t crsf_encode_mavlink_fc(uint8_t *out, const crsf_mavlink_fc_t *in)
{
    crsf_put_be16(&out[0], (uint16_t)in->airspeed);
    out[2] = in->base_mode;
    crsf_put_be32(&out[3], in->custom_mode);
    out[7] = in->autopilot_type;
    out[8] = in->firmware_type;
    return CRSF_PAYLOAD_SIZE_MAVLINK_FC;
}

bool crsf_decode_mavlink_fc(const uint8_t *pl, size_t len, crsf_mavlink_fc_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_MAVLINK_FC) {
        return false;
    }
    out->airspeed = (int16_t)crsf_get_be16(&pl[0]);
    out->base_mode = pl[2];
    out->custom_mode = crsf_get_be32(&pl[3]);
    out->autopilot_type = pl[7];
    out->firmware_type = pl[8];
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x21 Flight Mode — null-terminated string                                 */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_flight_mode(uint8_t *out, const crsf_flight_mode_t *in)
{
    size_t n = crsf_strnlen(in->flight_mode, CRSF_FLIGHT_MODE_MAX_LEN - 1);
    memcpy(out, in->flight_mode, n);
    out[n] = '\0';
    return n + 1;
}

bool crsf_decode_flight_mode(const uint8_t *pl, size_t len, crsf_flight_mode_t *out)
{
    if (len < 1) {
        return false;
    }
    /* Copy at most our buffer minus the terminator, never past the payload. */
    size_t n = len < (CRSF_FLIGHT_MODE_MAX_LEN - 1) ? len
                                                    : (CRSF_FLIGHT_MODE_MAX_LEN - 1);
    memcpy(out->flight_mode, pl, n);
    out->flight_mode[n] = '\0';
    /* A sender may pad past its own terminator; trust the first NUL. */
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x29 Device Information                                                   */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_device_info(uint8_t *out, const crsf_device_info_t *in)
{
    size_t n = crsf_strnlen(in->device_name, CRSF_DEVICE_NAME_MAX_LEN - 1);
    memcpy(out, in->device_name, n);
    out[n++] = '\0';

    crsf_put_be32(&out[n], in->serial_number);
    n += 4;
    crsf_put_be32(&out[n], in->hardware_id);
    n += 4;
    crsf_put_be32(&out[n], in->firmware_id);
    n += 4;
    out[n++] = in->parameters_total;
    out[n++] = in->parameter_version;
    return n;
}

bool crsf_decode_device_info(const uint8_t *pl, size_t len, crsf_device_info_t *out)
{
    /* Find the name terminator without running off the payload. */
    size_t name_len = 0;
    while (name_len < len && pl[name_len] != '\0') {
        name_len++;
    }
    if (name_len >= len) {
        return false; /* unterminated string */
    }

    const size_t fixed = 4 + 4 + 4 + 1 + 1;
    if (len < name_len + 1 + fixed) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    size_t copy = name_len < (CRSF_DEVICE_NAME_MAX_LEN - 1)
                      ? name_len
                      : (CRSF_DEVICE_NAME_MAX_LEN - 1);
    memcpy(out->device_name, pl, copy);
    out->device_name[copy] = '\0';

    size_t i = name_len + 1;
    out->serial_number = crsf_get_be32(&pl[i]);
    i += 4;
    out->hardware_id = crsf_get_be32(&pl[i]);
    i += 4;
    out->firmware_id = crsf_get_be32(&pl[i]);
    i += 4;
    out->parameters_total = pl[i++];
    out->parameter_version = pl[i];
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x3A.0x10 Timing Correction                                               */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_timing_correction(uint8_t *out, const crsf_timing_correction_t *in)
{
    out[0] = CRSF_REMOTE_SUBTYPE_TIMING_CORRECTION;
    crsf_put_be32(&out[1], in->update_interval);
    crsf_put_be32(&out[5], (uint32_t)in->offset);
    return CRSF_PAYLOAD_SIZE_TIMING_CORR;
}

bool crsf_decode_timing_correction(const uint8_t *pl, size_t len,
                                   crsf_timing_correction_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_TIMING_CORR) {
        return false;
    }
    if (pl[0] != CRSF_REMOTE_SUBTYPE_TIMING_CORRECTION) {
        return false;
    }
    out->update_interval = crsf_get_be32(&pl[1]);
    out->offset = (int32_t)crsf_get_be32(&pl[5]);
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x17 Subset RC Channels (crsf.md:548)                                     */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_subset_channels(uint8_t *out, const crsf_subset_channels_t *in)
{
    if (in->channel_count == 0 || in->channel_count > CRSF_SUBSET_MAX_CHANNELS) {
        return 0;
    }
    const unsigned res_bits = crsf_subset_res_bits(in->res_configuration);
    const uint8_t switches = in->digital_switch ? in->switch_count : 0;
    if (switches > CRSF_SUBSET_MAX_SWITCHES) {
        return 0;
    }

    /* header byte + channels + 10-bit switch channels, rounded up to bytes */
    const size_t total_bits = 8u + (size_t)in->channel_count * res_bits +
                              (size_t)switches * 10u;
    if ((total_bits + 7u) / 8u > CRSF_MAX_PAYLOAD_SIZE) {
        return 0;
    }

    crsf_bitwriter_t w;
    crsf_bw_init(&w, out, CRSF_MAX_PAYLOAD_SIZE);

    /*
     * Header byte, LSB-first to match the channel packing that follows:
     * starting_channel:5, res_configuration:2, digital_switch_flag:1.
     */
    crsf_bw_put_lsb(&w, in->starting_channel & 0x1F, 5);
    crsf_bw_put_lsb(&w, in->res_configuration & 0x03, 2);
    crsf_bw_put_lsb(&w, in->digital_switch ? 1u : 0u, 1);

    for (uint8_t i = 0; i < in->channel_count; i++) {
        crsf_bw_put_lsb(&w, in->channel[i], res_bits);
    }
    for (uint8_t i = 0; i < switches; i++) {
        crsf_bw_put_lsb(&w, in->switch_channel[i] & 0x3FF, 10);
    }

    if (w.overflow) {
        return 0;
    }
    return crsf_bw_bytes(&w);
}

bool crsf_decode_subset_channels(const uint8_t *pl, size_t len,
                                 crsf_subset_channels_t *out)
{
    if (len < 2) { /* header byte plus at least part of one channel */
        return false;
    }

    memset(out, 0, sizeof(*out));

    crsf_bitreader_t r;
    crsf_br_init(&r, pl, len);

    out->starting_channel = (uint8_t)crsf_br_get_lsb(&r, 5);
    out->res_configuration = (uint8_t)crsf_br_get_lsb(&r, 2);
    out->digital_switch = (uint8_t)crsf_br_get_lsb(&r, 1);

    const unsigned res_bits = crsf_subset_res_bits(out->res_configuration);

    /*
     * The channel count is not transmitted; it follows from the frame size
     * (crsf.md:565). Read as many whole channels as the remaining bits allow.
     * Any leftover bits shorter than one channel are padding.
     */
    while (out->channel_count < CRSF_SUBSET_MAX_CHANNELS &&
           crsf_br_remaining(&r) >= res_bits) {
        const uint16_t v = (uint16_t)crsf_br_get_lsb(&r, res_bits);
        if (r.overflow) {
            break;
        }
        out->channel[out->channel_count++] = v;
    }

    if (out->channel_count == 0) {
        return false;
    }

    /*
     * Digital switch channels share the same bit budget as the analogue ones, so
     * with a flag set we cannot tell where the analogue channels stop from the
     * length alone. Report what we read and let the caller reinterpret using
     * starting_channel; this ambiguity is part of why the spec discourages 0x17
     * (crsf.md:550).
     */
    out->switch_count = 0;
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x18 RC Channels Packed 11-bit (crsf.md:573)                              */
/* ------------------------------------------------------------------------- */
/*
 * Bit layout is identical to 0x16; only the value scaling differs (0x17 style).
 * The codec therefore only moves bits — converting between ticks and
 * microseconds is the caller's job via crsf_subset_to_us() with an 11-bit
 * resolution selector.
 */

size_t crsf_encode_channels_11bit(uint8_t *out, const crsf_channels_t *in)
{
    return crsf_encode_channels(out, in);
}

bool crsf_decode_channels_11bit(const uint8_t *pl, size_t len, crsf_channels_t *out)
{
    return crsf_decode_channels(pl, len, out);
}

/* ------------------------------------------------------------------------- */
/* 0x22 ESP_NOW (crsf.md:633)                                                */
/* ------------------------------------------------------------------------- */
/*
 * The three text fields are fixed-width and NOT null-terminated on the wire, so
 * encode pads with NUL and decode copies exactly the field width before adding
 * our own terminator.
 */

/**
 * @brief Copy @p n bytes of a fixed-width wire field and terminate it.
 * @param dst Destination of at least @p n + 1 bytes.
 * @param src Wire field, not NUL-terminated.
 * @param n   Field width on the wire.
 */
static void copy_fixed_field(char *dst, const uint8_t *src, size_t n)
{
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/**
 * @brief Write a fixed-width wire field from a C string, NUL-padded.
 * @param dst Destination of exactly @p n bytes.
 * @param src Source string; truncated at @p n characters, no terminator emitted.
 * @param n   Field width on the wire.
 */
static void put_fixed_field(uint8_t *dst, const char *src, size_t n)
{
    const size_t l = crsf_strnlen(src, n);
    memcpy(dst, src, l);
    if (l < n) {
        memset(dst + l, 0, n - l);
    }
}

size_t crsf_encode_esp_now(uint8_t *out, const crsf_esp_now_t *in)
{
    out[0] = in->seat_position;
    out[1] = in->current_lap;
    put_fixed_field(&out[2], in->lap_time_a, CRSF_ESP_NOW_LAP_LEN);
    put_fixed_field(&out[17], in->lap_time_b, CRSF_ESP_NOW_LAP_LEN);
    put_fixed_field(&out[32], in->free_text, CRSF_ESP_NOW_FREE_TEXT_LEN);
    return CRSF_PAYLOAD_SIZE_ESP_NOW;
}

bool crsf_decode_esp_now(const uint8_t *pl, size_t len, crsf_esp_now_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_ESP_NOW) {
        return false;
    }
    out->seat_position = pl[0];
    out->current_lap = pl[1];
    copy_fixed_field(out->lap_time_a, &pl[2], CRSF_ESP_NOW_LAP_LEN);
    copy_fixed_field(out->lap_time_b, &pl[17], CRSF_ESP_NOW_LAP_LEN);
    copy_fixed_field(out->free_text, &pl[32], CRSF_ESP_NOW_FREE_TEXT_LEN);
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x34 Logging (crsf.md:1152)                                               */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_logging(uint8_t *out, const crsf_logging_t *in)
{
    /*
     * Unlike the other variable-count frames, 0x34's header stands alone, so
     * param_count == 0 is a legitimate bare-header frame. Only an over-max
     * count is refused, which is what crsf_codec.h promises here.
     */
    const uint8_t n = in->param_count;
    if (n > CRSF_LOG_MAX_PARAMS) {
        return 0;
    }

    crsf_put_be16(&out[0], in->logtype);
    crsf_put_be32(&out[2], in->timestamp);
    for (uint8_t i = 0; i < n; i++) {
        crsf_put_be32(&out[6 + i * 4], in->param[i]);
    }
    return (size_t)(6 + n * 4);
}

bool crsf_decode_logging(const uint8_t *pl, size_t len, crsf_logging_t *out)
{
    if (len < 6) { /* logtype + timestamp */
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->logtype = crsf_get_be16(&pl[0]);
    out->timestamp = crsf_get_be32(&pl[2]);

    size_t n = (len - 6) / 4;
    if (n > CRSF_LOG_MAX_PARAMS) {
        n = CRSF_LOG_MAX_PARAMS;
    }
    out->param_count = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        out->param[i] = crsf_get_be32(&pl[6 + i * 4]);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x3C Game (crsf.md:1185)                                                  */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_game(uint8_t *out, const crsf_game_t *in)
{
    out[0] = in->sub_type;
    switch (in->sub_type) {
    case CRSF_GAME_ADD_POINTS:
        crsf_put_be16(&out[1], (uint16_t)in->u.points);
        return 3;
    case CRSF_GAME_COMMAND_CODE:
        crsf_put_be16(&out[1], in->u.code);
        return 3;
    default:
        return 0;
    }
}

bool crsf_decode_game(const uint8_t *pl, size_t len, crsf_game_t *out)
{
    if (len < 3) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->sub_type = pl[0];
    switch (pl[0]) {
    case CRSF_GAME_ADD_POINTS:
        out->u.points = (int16_t)crsf_get_be16(&pl[1]);
        return true;
    case CRSF_GAME_COMMAND_CODE:
        out->u.code = crsf_get_be16(&pl[1]);
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------- */
/* 0xAC MAVLink System Status Sensor (crsf.md:1324)                          */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_mavlink_status(uint8_t *out, const crsf_mavlink_status_t *in)
{
    crsf_put_be32(&out[0], in->sensor_present);
    crsf_put_be32(&out[4], in->sensor_enabled);
    crsf_put_be32(&out[8], in->sensor_health);
    return CRSF_PAYLOAD_SIZE_MAVLINK_STATUS;
}

bool crsf_decode_mavlink_status(const uint8_t *pl, size_t len,
                                crsf_mavlink_status_t *out)
{
    if (len < CRSF_PAYLOAD_SIZE_MAVLINK_STATUS) {
        return false;
    }
    out->sensor_present = crsf_get_be32(&pl[0]);
    out->sensor_enabled = crsf_get_be32(&pl[4]);
    out->sensor_health = crsf_get_be32(&pl[8]);
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0xAA MAVLink Envelope (crsf.md:1295)                                      */
/* ------------------------------------------------------------------------- */

size_t crsf_encode_mavlink_envelope(uint8_t *out, const crsf_mavlink_envelope_t *in)
{
    if (in->data_size > CRSF_MAVLINK_CHUNK_MAX) {
        return 0;
    }
    /* Both indices are 4 bits: total in the high nibble (crsf.md:1305). */
    if (in->total_chunks > 0x0F || in->current_chunk > 0x0F) {
        return 0;
    }
    out[0] = (uint8_t)(((in->total_chunks & 0x0F) << 4) | (in->current_chunk & 0x0F));
    out[1] = in->data_size;
    memcpy(&out[2], in->data, in->data_size);
    return (size_t)(2 + in->data_size);
}

bool crsf_decode_mavlink_envelope(const uint8_t *pl, size_t len,
                                  crsf_mavlink_envelope_t *out)
{
    if (len < 2) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->total_chunks = (uint8_t)(pl[0] >> 4);
    out->current_chunk = (uint8_t)(pl[0] & 0x0F);
    out->data_size = pl[1];

    if (out->data_size > CRSF_MAVLINK_CHUNK_MAX) {
        return false;
    }
    /* A truncated frame must not be read beyond its payload. */
    if ((size_t)out->data_size + 2u > len) {
        return false;
    }
    /* A chunk index above the declared last index is malformed. */
    if (out->current_chunk > out->total_chunks) {
        return false;
    }
    memcpy(out->data, &pl[2], out->data_size);
    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x32 packed command payloads                                              */
/* ------------------------------------------------------------------------- */

void crsf_pack_hsv(uint8_t out[3], const crsf_hsv_t *in)
{
    crsf_bitwriter_t w;
    crsf_bw_init(&w, out, 3);
    crsf_bw_put_msb(&w, in->h & 0x1FF, 9);
    crsf_bw_put_msb(&w, in->s & 0x7F, 7);
    crsf_bw_put_msb(&w, in->v, 8);
}

void crsf_unpack_hsv(const uint8_t in[3], crsf_hsv_t *out)
{
    crsf_bitreader_t r;
    crsf_br_init(&r, in, 3);
    out->h = (uint16_t)crsf_br_get_msb(&r, 9);
    out->s = (uint8_t)crsf_br_get_msb(&r, 7);
    out->v = (uint8_t)crsf_br_get_msb(&r, 8);
}

uint8_t crsf_pack_vtx_pitmode(const crsf_vtx_pitmode_t *in)
{
    /* pit_mode:1, pitmode_control:2, pitmode_switch:4 — same LSB-first order as
     * the equivalent field in the 0x10 VTX telemetry frame. */
    return (uint8_t)((in->pit_mode & 0x01) |
                     ((in->pitmode_control & 0x03) << 1) |
                     ((in->pitmode_switch & 0x0F) << 3));
}

void crsf_unpack_vtx_pitmode(uint8_t b, crsf_vtx_pitmode_t *out)
{
    out->pit_mode = b & 0x01;
    out->pitmode_control = (b >> 1) & 0x03;
    out->pitmode_switch = (b >> 3) & 0x0F;
}

/* --- 0x32.0x22.0x01 Pop-up Message (crsf.md:1113) --- */

/**
 * @brief Append a NUL-terminated string to a growing payload.
 * @param out      Destination buffer.
 * @param out_size Capacity of @p out.
 * @param pos      Write cursor; advanced past the string and its terminator.
 * @param s        String to append, truncated at CRSF_POPUP_STR_MAX - 1.
 * @retval true  Written.
 * @retval false It would not fit; @p out and @p pos are left unchanged.
 */
static bool put_cstr(uint8_t *out, size_t out_size, size_t *pos, const char *s)
{
    const size_t l = crsf_strnlen(s, CRSF_POPUP_STR_MAX - 1);
    if (*pos + l + 1u > out_size) {
        return false;
    }
    memcpy(&out[*pos], s, l);
    *pos += l;
    out[(*pos)++] = '\0';
    return true;
}

/**
 * @brief Read a NUL-terminated string, bounded by the payload.
 *
 * @param in       Payload.
 * @param len      Payload length.
 * @param pos      Read cursor; advanced past the string and its terminator.
 * @param dst      Destination; always NUL-terminated. A longer source string is
 *                 truncated, never overflowed.
 * @param dst_size Capacity of @p dst including the terminator.
 * @retval true  A terminated string was read.
 * @retval false No terminator appears before the end of the payload, which
 *               crsf.md:1139 forbids reading past.
 */
static bool get_cstr(const uint8_t *in, size_t len, size_t *pos, char *dst,
                     size_t dst_size)
{
    size_t n = 0;
    while (*pos + n < len && in[*pos + n] != '\0') {
        n++;
    }
    if (*pos + n >= len) {
        return false; /* unterminated */
    }
    const size_t copy = n < (dst_size - 1) ? n : (dst_size - 1);
    memcpy(dst, &in[*pos], copy);
    dst[copy] = '\0';
    *pos += n + 1;
    return true;
}

size_t crsf_encode_popup_message(uint8_t *out, size_t out_size,
                                 const crsf_popup_message_t *in)
{
    size_t pos = 0;

    if (!put_cstr(out, out_size, &pos, in->header)) {
        return 0;
    }
    if (!put_cstr(out, out_size, &pos, in->info_message)) {
        return 0;
    }
    if (pos + 2u > out_size) {
        return 0;
    }
    out[pos++] = in->max_timeout_interval;
    out[pos++] = in->close_button_option ? 1u : 0u;

    if (in->has_add_data) {
        /*
         * The block is introduced by its selection text, and an empty selection
         * text collapses the block to a single NUL (see the else branch and
         * crsf.md:1120). Emitting the block with an empty selection text is
         * therefore not representable: the decoder reads that lone NUL as
         * "block absent", skips the four value bytes and the unit that are
         * really on the wire, and parses them as possible_values instead.
         * Refuse rather than encode something that cannot round-trip.
         */
        if (in->selection_text[0] == '\0') {
            return 0;
        }
        if (!put_cstr(out, out_size, &pos, in->selection_text)) {
            return 0;
        }
        if (pos + 4u > out_size) {
            return 0;
        }
        out[pos++] = in->value;
        out[pos++] = in->min_value;
        out[pos++] = in->max_value;
        out[pos++] = in->default_value;
        if (!put_cstr(out, out_size, &pos, in->unit)) {
            return 0;
        }
    } else {
        /* Empty selection text collapses the whole block to one NUL byte
         * (crsf.md:1120). */
        if (pos + 1u > out_size) {
            return 0;
        }
        out[pos++] = '\0';
    }

    if (in->has_possible_values) {
        if (!put_cstr(out, out_size, &pos, in->possible_values)) {
            return 0;
        }
    }

    return pos;
}

bool crsf_decode_popup_message(const uint8_t *args, size_t len,
                               crsf_popup_message_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t pos = 0;

    /* Mandatory part. */
    if (!get_cstr(args, len, &pos, out->header, sizeof(out->header))) {
        return false;
    }
    if (!get_cstr(args, len, &pos, out->info_message, sizeof(out->info_message))) {
        return false;
    }
    if (pos + 2u > len) {
        return false;
    }
    out->max_timeout_interval = args[pos++];
    out->close_button_option = args[pos++] != 0;

    /*
     * Everything below is optional and may simply be absent — crsf.md:1139
     * requires analysing the frame size rather than reading ahead.
     */
    if (pos >= len) {
        return true;
    }

    if (args[pos] == '\0') {
        /* Empty selection text: the add_data block is not present. */
        pos++;
    } else {
        if (!get_cstr(args, len, &pos, out->selection_text,
                      sizeof(out->selection_text))) {
            return true; /* truncated mid-string; keep what we have */
        }
        if (pos + 4u > len) {
            return true;
        }
        out->value = args[pos++];
        out->min_value = args[pos++];
        out->max_value = args[pos++];
        out->default_value = args[pos++];
        if (!get_cstr(args, len, &pos, out->unit, sizeof(out->unit))) {
            return true;
        }
        out->has_add_data = true;
    }

    if (pos < len) {
        if (get_cstr(args, len, &pos, out->possible_values,
                     sizeof(out->possible_values))) {
            out->has_possible_values = true;
        }
    }
    return true;
}

size_t crsf_encode_popup_response(uint8_t *out, const crsf_popup_response_t *in)
{
    out[0] = in->value;
    out[1] = in->process ? 1u : 0u;
    return 2;
}

bool crsf_decode_popup_response(const uint8_t *args, size_t len,
                                crsf_popup_response_t *out)
{
    if (len < 2) {
        return false;
    }
    out->value = args[0];
    out->process = args[1] != 0;
    return true;
}
