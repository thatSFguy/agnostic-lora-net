// Host unit tests for the announce wire codec (lib/mesh/announce_codec).
//
//   pio test -e native
//
// Deserialisation parses untrusted bytes off the radio, so the malformed-input
// cases matter as much as the happy path.

#include <unity.h>
#include "announce_codec.h"

using namespace mesh;

static Announce make_sample() {
    Announce a;
    a.origin = nid_from_u32(0x11111111u);
    a.reports[0] = {nid_from_u32(0xAAAAAAAAu), 0.80f, 3};
    a.reports[1] = {nid_from_u32(0xBBBBBBBBu), 0.20f, 7};
    a.n_reports = 2;
    a.routes[0] = {nid_from_u32(0xAAAAAAAAu), nid_from_u32(0xAAAAAAAAu), 1.5f, 1};
    a.routes[1] = {nid_from_u32(0xCCCCCCCCu), nid_from_u32(0xBBBBBBBBu), 3.25f, 2};
    a.n_routes = 2;
    return a;
}

// Round-trip: fields survive serialise -> deserialise within quantisation error.
static void test_roundtrip() {
    Announce a = make_sample();
    uint8_t buf[256];
    uint16_t n = announce_serialize(a, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT16(ANNOUNCE_HDR_BYTES + 2 * ANNOUNCE_REPORT_BYTES + 2 * ANNOUNCE_ROUTE_BYTES, n);

    Announce b;
    TEST_ASSERT_TRUE(announce_deserialize(buf, n, b));
    TEST_ASSERT_EQUAL_UINT8(2, b.n_reports);
    TEST_ASSERT_EQUAL_UINT8(2, b.n_routes);

    TEST_ASSERT_TRUE(b.reports[0].id == nid_from_u32(0xAAAAAAAAu));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.80f, b.reports[0].q);   // 1/255 quantisation
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.20f, b.reports[1].q);
    TEST_ASSERT_EQUAL_UINT8(3, b.reports[0].alias);           // alias carried exactly
    TEST_ASSERT_EQUAL_UINT8(7, b.reports[1].alias);

    TEST_ASSERT_TRUE(b.routes[1].dst == nid_from_u32(0xCCCCCCCCu));
    TEST_ASSERT_TRUE(b.routes[1].next_hop == nid_from_u32(0xBBBBBBBBu));
    TEST_ASSERT_FLOAT_WITHIN(0.07f, 3.25f, b.routes[1].cost);  // 1/16 quantisation
    TEST_ASSERT_EQUAL_UINT8(2, b.routes[1].hops);
}

// An empty announce is still valid (just the two count bytes).
static void test_empty() {
    Announce a;  // 0 reports, 0 routes
    uint8_t buf[8];
    uint16_t n = announce_serialize(a, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT16(ANNOUNCE_HDR_BYTES, n);

    Announce b;
    TEST_ASSERT_TRUE(announce_deserialize(buf, n, b));
    TEST_ASSERT_EQUAL_UINT8(0, b.n_reports);
    TEST_ASSERT_EQUAL_UINT8(0, b.n_routes);
}

// A truncated buffer must be rejected, not read out of bounds.
static void test_truncated_rejected() {
    Announce a = make_sample();
    uint8_t buf[256];
    uint16_t n = announce_serialize(a, buf, sizeof(buf));

    Announce b;
    TEST_ASSERT_FALSE(announce_deserialize(buf, n - 1, b));   // one byte short
    TEST_ASSERT_FALSE(announce_deserialize(buf, 1, b));       // shorter than header
    TEST_ASSERT_EQUAL_UINT8(0, b.n_reports);                  // out cleared on failure
}

// A header claiming more records than the buffer holds (or than our arrays can
// hold) must be rejected.
static void test_malformed_counts_rejected() {
    uint8_t buf[4] = {0xFF, 0xFF, 0x00, 0x00};  // claims 255 reports + 255 routes
    Announce b;
    TEST_ASSERT_FALSE(announce_deserialize(buf, sizeof(buf), b));
}

// A too-small output buffer drops what doesn't fit instead of overflowing: the
// header still serialises, routes get truncated, and the result round-trips.
static void test_output_truncation_is_graceful() {
    Announce a = make_sample();
    // Room for the header + both reports + exactly ONE route.
    uint16_t cap = ANNOUNCE_HDR_BYTES + 2 * ANNOUNCE_REPORT_BYTES + 1 * ANNOUNCE_ROUTE_BYTES;
    uint8_t buf[128];
    uint16_t n = announce_serialize(a, buf, cap);
    TEST_ASSERT_EQUAL_UINT16(cap, n);

    Announce b;
    TEST_ASSERT_TRUE(announce_deserialize(buf, n, b));
    TEST_ASSERT_EQUAL_UINT8(2, b.n_reports);
    TEST_ASSERT_EQUAL_UINT8(1, b.n_routes);   // second route dropped, cleanly

    // And a buffer too small for even the header yields 0.
    TEST_ASSERT_EQUAL_UINT16(0, announce_serialize(a, buf, 1));
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_empty);
    RUN_TEST(test_truncated_rejected);
    RUN_TEST(test_malformed_counts_rejected);
    RUN_TEST(test_output_truncation_is_graceful);
    return UNITY_END();
}
