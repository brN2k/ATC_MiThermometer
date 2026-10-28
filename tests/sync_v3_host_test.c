#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "../src/sync_v3_logic.h"
#include "../src/eu_timezone.h"

static void packet(uint8_t b[18], uint32_t id, uint32_t timestamp, uint8_t role) {
    static const uint8_t start[] = {0x40, 0x00, 0x37, 0x50};
    memcpy(b, start, sizeof(start));
    b[4] = (uint8_t)timestamp;
    b[5] = (uint8_t)(timestamp >> 8);
    b[6] = (uint8_t)(timestamp >> 16);
    b[7] = (uint8_t)(timestamp >> 24);
    b[8] = 0x54; b[9] = 8; b[10] = 'T'; b[11] = 'S'; b[12] = 1; b[13] = role;
    b[14] = (uint8_t)id; b[15] = (uint8_t)(id >> 8);
    b[16] = (uint8_t)(id >> 16); b[17] = (uint8_t)(id >> 24);
}
int main(void) {
    const uint32_t id = 0xA1B2C3D4u;
    const uint32_t timestamp = 1792882800u;
    uint8_t p[18]; uint32_t got = 0;
    packet(p,id,timestamp,SYNC_V3_ROLE_RESPONSE);
    assert(sync_v3_decode_response(p,sizeof(p),id,&got) == 1 && got == timestamp);
    for (unsigned len = 0; len < sizeof(p); ++len) {
        /* NULL is intentional: the length guard must precede every read. */
        assert(!sync_v3_decode_response(NULL,len,id,&got));
    }
    assert(!sync_v3_decode_response(p,sizeof(p)+1,id,&got));
    for (unsigned i = 0; i < sizeof(p); ++i) {
        if (i == 2 || (i >= 4 && i <= 7)) continue; /* counter/time may vary */
        uint8_t old = p[i];
        p[i] ^= 0x80;
        assert(!sync_v3_decode_response(p,sizeof(p),id,&got));
        p[i] = old;
    }
    assert(sync_v3_decode_response(p,sizeof(p),id+1,&got) == 0);
    p[13] = SYNC_V3_ROLE_REQUEST;
    assert(sync_v3_decode_response(p,sizeof(p),id,&got) == 0);
    packet(p,id,UINT32_MAX,SYNC_V3_ROLE_RESPONSE);
    assert(!sync_v3_decode_response(p,sizeof(p),id,&got));
    packet(p,id,SYNC_V3_MIN_VALID_UTC,SYNC_V3_ROLE_RESPONSE);
    assert(sync_v3_decode_response(p,sizeof(p),id,&got));
    packet(p,id,SYNC_V3_MAX_VALID_UTC,SYNC_V3_ROLE_RESPONSE);
    assert(sync_v3_decode_response(p,sizeof(p),id,&got));
    p[13] = SYNC_V3_ROLE_RESPONSE;
    p[10] = 'X'; assert(sync_v3_decode_response(p,sizeof(p),id,&got) == 0);
    packet(p,id,0,SYNC_V3_ROLE_RESPONSE);
    assert(sync_v3_decode_response(p,sizeof(p),id,&got) == 0);
    assert(sync_v3_interval_sec(0) == 15u*86400u);
    assert(sync_v3_interval_sec(1) == 15u*86400u);
    assert(sync_v3_interval_sec(5800) > 3u*86400u);  /* ~58 ppm */
    assert(sync_v3_interval_sec(-5800) == sync_v3_interval_sec(5800));
    assert(sync_v3_interval_sec(999999) == 6u*3600u);
    assert(sync_v3_interval_sec(INT32_MIN) == 6u*3600u);
    assert(sync_v3_interval_sec(INT32_MAX) == 6u*3600u);
    assert(sync_v3_abs32(INT32_MIN) == UINT32_C(2147483648));
    assert(sync_v3_drift_sample_x100(2,86400) == 2314);
    assert(sync_v3_drift_sample_x100(-2,86400) == -2314);
    assert(sync_v3_valid_sample(2,86400));
    assert(!sync_v3_valid_sample(2,60));
    assert(!sync_v3_valid_sample(4000,86400));
    assert(!sync_v3_valid_sample(INT32_MIN,86400));
    assert(!sync_v3_valid_sample(0,SYNC_V3_LEARN_MAX_SEC+1));
    assert(!sync_v3_valid_sample(0,0));
    assert(sync_v3_drift_sample_x100(300,0) == 0);
    /* Compare the TC32-friendly arithmetic against an independent 64-bit oracle. */
    for (uint32_t elapsed = SYNC_V3_LEARN_MIN_SEC; elapsed <= SYNC_V3_LEARN_MAX_SEC; elapsed += 137) {
        for (int32_t error = -300; error <= 300; ++error) {
            int32_t expected = (int32_t)((int64_t)error * INT64_C(100000000) / elapsed);
            assert(sync_v3_drift_sample_x100(error,elapsed) == expected);
        }
    }
    assert(sync_v3_smooth_x100(-5000,1000) == -3500);
    assert(sync_v3_smooth_x100(INT32_MIN,INT32_MIN) == INT32_MIN);
    assert(sync_v3_smooth_x100(INT32_MAX,INT32_MAX) == INT32_MAX);
    /* Regression: opposite signed rates must not cancel the planning rate. */
    const uint32_t elapsed = sync_v3_interval_sec(4000);
    const int32_t latest = sync_v3_drift_sample_x100(-69,elapsed);
    const int32_t estimate = sync_v3_smooth_x100(4000,latest);
    assert(latest == -11960 && estimate == 10);
    assert(sync_v3_conservative_interval_sec(estimate,latest) == sync_v3_interval_sec(latest));
    assert(sync_v3_conservative_interval_sec(estimate,latest) < 3u*86400u);
    /* Faster worsening, slower relaxation, zero drift and boundary signs. */
    assert(sync_v3_conservative_interval_sec(1000,10000) == sync_v3_interval_sec(10000));
    assert(sync_v3_conservative_interval_sec(10000,1000) == sync_v3_interval_sec(10000));
    assert(sync_v3_conservative_interval_sec(0,0) == SYNC_V3_MAX_INTERVAL_SEC);
    assert(sync_v3_conservative_interval_sec(INT32_MIN,INT32_MAX) == SYNC_V3_MIN_INTERVAL_SEC);
    assert(sync_v3_due(200,199) && !sync_v3_due(198,199));
    assert(sync_v3_delta(100,101) == -1);
    assert(sync_v3_due(4,UINT32_MAX-3));
    assert(!sync_v3_due(UINT32_MAX-3,4));
    /* 2026-03-29 00:59:59 -> 01:00:00 UTC, begin DST */
    const uint32_t mar = 1774746000u;
    assert(!eu_tz_is_dst(mar-1));
    assert(eu_tz_is_dst(mar));
    assert(eu_tz_local_epoch(mar-1) == mar-1 + 3600u);
    assert(eu_tz_local_epoch(mar) == mar + 7200u);
    /* 2026-10-25 00:59:59 -> 01:00:00 UTC, end DST */
    const uint32_t oct = 1792890000u;
    assert(eu_tz_is_dst(oct-1));
    assert(!eu_tz_is_dst(oct));
    assert(eu_tz_local_epoch(oct-1) == oct-1 + 7200u);
    assert(eu_tz_local_epoch(oct) == oct + 3600u);
    assert(!eu_tz_is_dst(1767268800u)); /* January 2026 */
    puts("sync_v3 core tests: OK");
    return 0;
}
