/*
 * Private BTHome time sync (USE_SYNC_SCAN=3): pure scheduling/drift functions.
 * Keep this header independent of the Telink SDK so it can be tested on host.
 * Integrates with the upstream firmware; retain its applicable license.
 */
#ifndef SYNC_V3_LOGIC_H
#define SYNC_V3_LOGIC_H

#include <stdint.h>

#define SYNC_V3_PROTOCOL_VERSION       1U
#define SYNC_V3_ROLE_REQUEST           1U
#define SYNC_V3_ROLE_RESPONSE          2U
#define SYNC_V3_SERVICE_LENGTH         18U
#define SYNC_V3_MIN_VALID_UTC          1704067200UL /* 2024-01-01 UTC */
/* PCF85163 stores only the years 2000..2099; also leaves room for CET. */
#define SYNC_V3_MAX_VALID_UTC          4102441199UL /* 2099-12-31 22:59:59 UTC */
#define SYNC_V3_FIRST_INTERVAL_SEC     86400UL
#define SYNC_V3_BOOT_RETRY_SEC         3600UL
#define SYNC_V3_FAILED_RETRY_SEC       86400UL
#define SYNC_V3_LEARN_MIN_SEC          43200UL
#define SYNC_V3_LEARN_MAX_SEC          (16UL * 86400UL)
#define SYNC_V3_MIN_INTERVAL_SEC       (6UL * 3600UL)
#define SYNC_V3_MAX_INTERVAL_SEC       (15UL * 86400UL)
#define SYNC_V3_MAX_TIME_ERROR_SEC     30UL
/* At least ~1 second/day model+quantization reserve (12 ppm). */
#define SYNC_V3_UNCERTAINTY_PPM_X100   1200UL
/* Reject nonsensical drift samples, e.g. from a manual clock correction. */
#define SYNC_V3_MAX_SAMPLE_ERROR_SEC   300L

static inline int32_t sync_v3_delta(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

static inline uint32_t sync_v3_abs32(int32_t x) {
    return x < 0 ? UINT32_C(0) - (uint32_t)x : (uint32_t)x;
}

static inline int sync_v3_due(uint32_t now, uint32_t due) {
    return sync_v3_delta(now, due) >= 0;
}

static inline int sync_v3_valid_sample(int32_t clock_error, uint32_t elapsed) {
    return elapsed >= SYNC_V3_LEARN_MIN_SEC &&
           elapsed <= SYNC_V3_LEARN_MAX_SEC &&
           sync_v3_abs32(clock_error) <= (uint32_t)SYNC_V3_MAX_SAMPLE_ERROR_SEC;
}

/* Exact truncated ppm * 100 without the TC32 runtime's missing 64-bit
 * multiply/divide helpers. Validated bounds keep both products < 2^32. */
static inline int32_t sync_v3_drift_sample_x100(int32_t clock_error, uint32_t elapsed) {
    uint32_t magnitude = sync_v3_abs32(clock_error);
    uint32_t sample;
    if (!sync_v3_valid_sample(clock_error, elapsed))
        return 0;
    sample = magnitude * (UINT32_C(100000000) / elapsed) +
             magnitude * (UINT32_C(100000000) % elapsed) / elapsed;
    return clock_error < 0 ? -(int32_t)sample : (int32_t)sample;
}

static inline int32_t sync_v3_smooth_x100(int32_t previous, int32_t current) {
    return (int32_t)(((int64_t)3 * previous + current) / 4);
}

/*
 * Set the *next* interval from estimated clock speed, not from the last
 * scheduled interval. A zero measured drift expands to 15 days, not 1 hour.
 * The extra 12 ppm accounts roughly for one second/day timestamp uncertainty.
 * The 30-second budget is an engineering target, not a guarantee after
 * temperature changes, timer faults or a lost time server.
 */
static inline uint32_t sync_v3_interval_sec(int32_t ppm_x100) {
    uint32_t abs_ppm_x100 = sync_v3_abs32(ppm_x100);
    uint32_t denominator = abs_ppm_x100 + SYNC_V3_UNCERTAINTY_PPM_X100;
    /* 30 seconds * 1,000,000 ppm * 100 scale = 3,000,000,000. */
    uint32_t interval = UINT32_C(3000000000) / denominator;
    if (interval < SYNC_V3_MIN_INTERVAL_SEC)
        return SYNC_V3_MIN_INTERVAL_SEC;
    if (interval > SYNC_V3_MAX_INTERVAL_SEC)
        return SYNC_V3_MAX_INTERVAL_SEC;
    return interval;
}

/* React immediately to a worse measured rate, including a sign reversal.
 * Let the EMA delay interval growth when the latest measurement improves. */
static inline uint32_t sync_v3_conservative_interval_sec(int32_t estimate, int32_t latest) {
    uint32_t estimated_interval = sync_v3_interval_sec(estimate);
    uint32_t latest_interval = sync_v3_interval_sec(latest);
    return estimated_interval < latest_interval ? estimated_interval : latest_interval;
}

/* Strict, fixed-sized service-data parser. Source MAC intentionally ignored. */
static inline int sync_v3_decode_response(const uint8_t *data, uint32_t length, uint32_t server_id,
                                          uint32_t *utc_out) {
    if (length != SYNC_V3_SERVICE_LENGTH)
        return 0;
    if (data[0] != 0x40 || data[1] != 0x00 || data[3] != 0x50 ||
        data[8] != 0x54 || data[9] != 8 ||
        data[10] != 'T' || data[11] != 'S' ||
        data[12] != SYNC_V3_PROTOCOL_VERSION ||
        data[13] != SYNC_V3_ROLE_RESPONSE)
        return 0;
    uint32_t id = (uint32_t)data[14] | ((uint32_t)data[15] << 8) |
                  ((uint32_t)data[16] << 16) | ((uint32_t)data[17] << 24);
    if (id != server_id)
        return 0;
    uint32_t utc = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                   ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    if (utc < SYNC_V3_MIN_VALID_UTC || utc > SYNC_V3_MAX_VALID_UTC)
        return 0;
    *utc_out = utc;
    return 1;
}

#endif /* SYNC_V3_LOGIC_H */
