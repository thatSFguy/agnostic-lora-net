// Host unit tests for the sparse telemetry codec + cache (lib/mesh/telemetry).
//
//   pio test -e native

#include <unity.h>
#include "telemetry.h"

using namespace mesh;

static void test_batt_roundtrip() {
    uint8_t b[8];
    uint16_t n = telem_build_batt(4052, 90, b, sizeof(b));
    TEST_ASSERT_EQUAL_UINT16(4, n);
    TelemMsg m;
    TEST_ASSERT_TRUE(telem_parse(b, n, &m));
    TEST_ASSERT_EQUAL_UINT8(TELEM_BATT, m.kind);
    TEST_ASSERT_EQUAL_UINT16(4052, m.mv);
    TEST_ASSERT_EQUAL_UINT8(90, m.pct_plus1);
}

static void test_query_roundtrip() {
    uint8_t b[8];
    uint16_t n = telem_build_query(0x9828F51Bu, b, sizeof(b));
    TEST_ASSERT_EQUAL_UINT16(5, n);
    TelemMsg m;
    TEST_ASSERT_TRUE(telem_parse(b, n, &m));
    TEST_ASSERT_EQUAL_UINT8(TELEM_QUERY, m.kind);
    TEST_ASSERT_EQUAL_HEX32(0x9828F51Bu, m.target);
    // target 0 is not a node
    TEST_ASSERT_EQUAL_UINT16(0, telem_build_query(0, b, sizeof(b)));
}

static void test_reply_roundtrip() {
    TelemNbr nbrs[2] = { {0xD97EEC3Au, 99, 100}, {0xB51EEC13u, 87, 0} };
    uint8_t b[64];
    uint16_t n = telem_build_reply(4021, 86, 1234, "0.7.0", nbrs, 2, b, sizeof(b));
    TEST_ASSERT_TRUE(n > 0);
    TelemMsg m;
    TEST_ASSERT_TRUE(telem_parse(b, n, &m));
    TEST_ASSERT_EQUAL_UINT8(TELEM_REPLY, m.kind);
    TEST_ASSERT_EQUAL_UINT16(4021, m.mv);
    TEST_ASSERT_EQUAL_UINT8(86, m.pct_plus1);
    TEST_ASSERT_EQUAL_UINT16(1234, m.uptime_min);
    TEST_ASSERT_EQUAL_STRING("0.7.0", m.fw);
    TEST_ASSERT_EQUAL_UINT8(2, m.n_nbrs);
    TEST_ASSERT_EQUAL_HEX32(0xD97EEC3Au, m.nbrs[0].id);
    TEST_ASSERT_EQUAL_UINT8(100, m.nbrs[0].q_tx);
    TEST_ASSERT_EQUAL_HEX32(0xB51EEC13u, m.nbrs[1].id);
}

static void test_parse_rejects_malformed() {
    uint8_t junk[3] = { TELEM_REPLY, 0, 0 };
    TelemMsg m;
    TEST_ASSERT_FALSE(telem_parse(junk, sizeof(junk), &m));     // truncated reply
    uint8_t bad[5] = { 99, 0, 0, 0, 0 };
    TEST_ASSERT_FALSE(telem_parse(bad, sizeof(bad), &m));       // unknown kind
    // reply whose nbr count overruns the buffer
    TelemNbr nbrs[1] = { {1, 1, 1} };
    uint8_t b[64];
    uint16_t n = telem_build_reply(1, 1, 1, "x", nbrs, 1, b, sizeof(b));
    b[n - 7] = 5;                                               // claim 5 nbrs, carry 1
    TEST_ASSERT_FALSE(telem_parse(b, n, &m));
}

static void test_cache_upsert_and_eviction() {
    TelemCache c;
    c.upsert(0xAAAA0001u, 4000, 81, 1000);
    c.upsert(0xAAAA0001u, 3900, 71, 2000);          // refresh, not duplicate
    TelemCache::View v[TELEM_CACHE_CAP + 2];
    TEST_ASSERT_EQUAL_UINT16(1, c.snapshot(v, TELEM_CACHE_CAP, 3000));
    TEST_ASSERT_EQUAL_UINT16(3900, v[0].mv);
    TEST_ASSERT_EQUAL_UINT32(1000, v[0].age_ms);
    // fill beyond capacity: the stalest entry is evicted
    for (uint32_t i = 0; i < TELEM_CACHE_CAP + 1; i++)
        c.upsert(0xBBBB0000u + i, 3700, 51, 10000 + i);
    uint16_t n = c.snapshot(v, TELEM_CACHE_CAP + 2, 20000);
    TEST_ASSERT_EQUAL_UINT16(TELEM_CACHE_CAP, n);
    for (uint16_t i = 0; i < n; i++)
        TEST_ASSERT_NOT_EQUAL(0xAAAA0001u, v[i].origin);  // oldest (refreshed at 2000) evicted
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_batt_roundtrip);
    RUN_TEST(test_query_roundtrip);
    RUN_TEST(test_reply_roundtrip);
    RUN_TEST(test_parse_rejects_malformed);
    RUN_TEST(test_cache_upsert_and_eviction);
    return UNITY_END();
}
