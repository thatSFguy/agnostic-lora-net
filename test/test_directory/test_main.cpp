// Host unit tests for the distributed locator directory (lib/mesh/locator_dir).
//
//   pio test -e native

#include <unity.h>
#include <string.h>
#include "locator_dir.h"

using namespace mesh;

// A deterministic 16-byte "identity hash" from a seed.
static void make_id(uint8_t* id, uint8_t seed) {
    for (int i = 0; i < LOC_ID_MAX; i++) id[i] = (uint8_t)(seed * 17 + i * 3 + 1);
}

// ---- codec ----------------------------------------------------------------------
static void test_codec_register_roundtrip() {
    uint8_t id[LOC_ID_MAX]; make_id(id, 1);
    uint8_t buf[64];
    uint16_t n = loc_build_register(0xBEEF, 0x1234, 300, id, LOC_ID_MAX, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    LocMsg m;
    TEST_ASSERT_TRUE(loc_parse(buf, n, &m));
    TEST_ASSERT_EQUAL_UINT8(LOC_REGISTER, m.kind);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, m.epoch);
    TEST_ASSERT_EQUAL_HEX16(0x1234, m.seq);
    TEST_ASSERT_EQUAL_UINT16(300, m.ttl_s);
    TEST_ASSERT_EQUAL_UINT8(LOC_ID_MAX, m.id_len);
    TEST_ASSERT_EQUAL_MEMORY(id, m.id, LOC_ID_MAX);
}

static void test_codec_query_reply_roundtrip() {
    uint8_t id[LOC_ID_MAX]; make_id(id, 2);
    uint8_t buf[64];

    uint16_t qn = loc_build_query(0x77, id, 8, buf, sizeof(buf));   // shorter id ok
    TEST_ASSERT_TRUE(qn > 0);
    LocMsg q;
    TEST_ASSERT_TRUE(loc_parse(buf, qn, &q));
    TEST_ASSERT_EQUAL_UINT8(LOC_QUERY, q.kind);
    TEST_ASSERT_EQUAL_HEX16(0x77, q.qid);
    TEST_ASSERT_EQUAL_UINT8(8, q.id_len);
    TEST_ASSERT_EQUAL_MEMORY(id, q.id, 8);

    uint16_t rn = loc_build_reply(0x77, 0xAA, 0xBB, 120, 0x9828F51Bu, id, 8, buf, sizeof(buf));
    TEST_ASSERT_TRUE(rn > 0);
    LocMsg r;
    TEST_ASSERT_TRUE(loc_parse(buf, rn, &r));
    TEST_ASSERT_EQUAL_UINT8(LOC_REPLY, r.kind);
    TEST_ASSERT_EQUAL_HEX16(0x77, r.qid);
    TEST_ASSERT_EQUAL_HEX32(0x9828F51Bu, r.loc);
    TEST_ASSERT_EQUAL_UINT8(8, r.id_len);
}

static void test_codec_rejects_malformed() {
    uint8_t id[LOC_ID_MAX]; make_id(id, 3);
    uint8_t buf[64];
    LocMsg m;
    // too short
    TEST_ASSERT_FALSE(loc_parse(buf, 1, &m));
    // unknown kind
    buf[0] = 99; buf[1] = 0;
    TEST_ASSERT_FALSE(loc_parse(buf, 2, &m));
    // valid register, then corrupt the id_len so it doesn't match the trailing length
    uint16_t n = loc_build_register(1, 1, 10, id, LOC_ID_MAX, buf, sizeof(buf));
    buf[7] = LOC_ID_MAX - 2;                       // claim fewer id bytes than present
    TEST_ASSERT_FALSE(loc_parse(buf, n, &m));      // trailing-junk check rejects it
    // truncated (drop the last byte)
    n = loc_build_register(1, 1, 10, id, LOC_ID_MAX, buf, sizeof(buf));
    TEST_ASSERT_FALSE(loc_parse(buf, n - 1, &m));
    // id_len = 0 is invalid on build
    TEST_ASSERT_EQUAL_UINT16(0, loc_build_register(1, 1, 10, id, 0, buf, sizeof(buf)));
    // cap too small
    TEST_ASSERT_EQUAL_UINT16(0, loc_build_register(1, 1, 10, id, LOC_ID_MAX, buf, 4));
}

// ---- cache: hit / miss / remove -------------------------------------------------
static void test_cache_hit_miss_remove() {
    LocatorDir d;
    uint8_t a[LOC_ID_MAX]; make_id(a, 10);
    uint8_t b[LOC_ID_MAX]; make_id(b, 11);
    node_id_t loc = 0;

    TEST_ASSERT_FALSE(d.lookup(a, LOC_ID_MAX, &loc, 1000));      // miss
    TEST_ASSERT_TRUE(d.upsert(a, LOC_ID_MAX, 0xAAAA, 1, 1, 60, 1000));
    TEST_ASSERT_TRUE(d.lookup(a, LOC_ID_MAX, &loc, 1000));       // hit
    TEST_ASSERT_EQUAL_HEX32(0xAAAA, loc);
    TEST_ASSERT_FALSE(d.lookup(b, LOC_ID_MAX, &loc, 1000));      // different id misses
    TEST_ASSERT_EQUAL_UINT16(1, d.size(1000));

    TEST_ASSERT_TRUE(d.remove(a, LOC_ID_MAX));                   // NO_CONSUMER drop
    TEST_ASSERT_FALSE(d.lookup(a, LOC_ID_MAX, &loc, 1000));
    TEST_ASSERT_FALSE(d.remove(a, LOC_ID_MAX));                  // already gone
}

// ---- cache: TTL expiry ----------------------------------------------------------
static void test_cache_ttl_expiry() {
    LocatorDir d;
    uint8_t a[LOC_ID_MAX]; make_id(a, 20);
    node_id_t loc = 0;
    TEST_ASSERT_TRUE(d.upsert(a, LOC_ID_MAX, 0x1234, 1, 1, 10, 1000));   // ttl 10s
    TEST_ASSERT_TRUE(d.lookup(a, LOC_ID_MAX, &loc, 1000 + 9000));        // still live at +9s
    TEST_ASSERT_FALSE(d.lookup(a, LOC_ID_MAX, &loc, 1000 + 10001));      // expired at +10.001s
    TEST_ASSERT_EQUAL_UINT16(0, d.size(1000 + 10001));
}

// ---- cache: seq anti-replay + mobility ------------------------------------------
static void test_cache_seq_and_mobility() {
    LocatorDir d;
    uint8_t id[LOC_ID_MAX]; make_id(id, 30);
    node_id_t loc = 0;
    const uint16_t EP = 5;

    TEST_ASSERT_TRUE (d.upsert(id, LOC_ID_MAX, 0x0A, EP, 1, 60, 0));     // at node A, seq 1
    TEST_ASSERT_FALSE(d.upsert(id, LOC_ID_MAX, 0x0A, EP, 1, 60, 0));     // replay same seq -> reject
    d.lookup(id, LOC_ID_MAX, &loc, 0); TEST_ASSERT_EQUAL_HEX32(0x0A, loc);

    TEST_ASSERT_TRUE (d.upsert(id, LOC_ID_MAX, 0x0B, EP, 2, 60, 0));     // moved to B, seq 2 -> accept
    d.lookup(id, LOC_ID_MAX, &loc, 0); TEST_ASSERT_EQUAL_HEX32(0x0B, loc);

    TEST_ASSERT_FALSE(d.upsert(id, LOC_ID_MAX, 0x0C, EP, 1, 60, 0));     // stale seq 1 -> reject
    d.lookup(id, LOC_ID_MAX, &loc, 0); TEST_ASSERT_EQUAL_HEX32(0x0B, loc);  // still B
}

// ---- cache: post-reboot re-register (review note F) -----------------------------
static void test_cache_reboot_reregister() {
    LocatorDir d;
    uint8_t id[LOC_ID_MAX]; make_id(id, 40);
    node_id_t loc = 0;
    // Endpoint at high seq under epoch 100.
    TEST_ASSERT_TRUE(d.upsert(id, LOC_ID_MAX, 0xAA, 100, 5000, 600, 0));
    // It reboots: NEW epoch, seq resets LOW. Must be accepted promptly (not rejected as
    // a replay), else the node is unreachable until TTL expiry.
    TEST_ASSERT_TRUE(d.upsert(id, LOC_ID_MAX, 0xBB, 101, 1, 600, 0));
    d.lookup(id, LOC_ID_MAX, &loc, 0);
    TEST_ASSERT_EQUAL_HEX32(0xBB, loc);
}

// ---- cache: LRU eviction at capacity --------------------------------------------
static void test_cache_lru_eviction() {
    LocatorDir d;
    uint8_t id[LOC_ID_MAX];
    const uint16_t CAP = LocatorDir::capacity();
    for (uint16_t i = 0; i < CAP; i++) {                 // fill to capacity
        make_id(id, (uint8_t)i);
        TEST_ASSERT_TRUE(d.upsert(id, LOC_ID_MAX, 0x1000 + i, 1, 1, 600, 100));
    }
    TEST_ASSERT_EQUAL_UINT16(CAP, d.size(100));

    node_id_t loc = 0;
    make_id(id, 0); TEST_ASSERT_TRUE(d.lookup(id, LOC_ID_MAX, &loc, 100));  // touch id 0 (now MRU)

    make_id(id, (uint8_t)CAP);                            // one more -> evicts the LRU (id 1)
    TEST_ASSERT_TRUE(d.upsert(id, LOC_ID_MAX, 0xDEAD, 1, 1, 600, 100));
    TEST_ASSERT_EQUAL_UINT16(CAP, d.size(100));           // still full, not grown

    make_id(id, 0);   TEST_ASSERT_TRUE (d.lookup(id, LOC_ID_MAX, &loc, 100)); // id 0 survived (MRU)
    make_id(id, 1);   TEST_ASSERT_FALSE(d.lookup(id, LOC_ID_MAX, &loc, 100)); // id 1 was evicted
    make_id(id, CAP); TEST_ASSERT_TRUE (d.lookup(id, LOC_ID_MAX, &loc, 100)); // newcomer present
}

// ---- seq wraparound -------------------------------------------------------------
static void test_seq_wraparound() {
    TEST_ASSERT_TRUE (loc_seq_newer(0, 65535));   // 0 is "after" 65535
    TEST_ASSERT_FALSE(loc_seq_newer(65535, 0));
    TEST_ASSERT_TRUE (loc_seq_newer(2, 1));
    TEST_ASSERT_FALSE(loc_seq_newer(1, 1));
}

// ---- lookup_full: binding for building a REPLY ----------------------------------
static void test_lookup_full() {
    LocatorDir d;
    uint8_t id[LOC_ID_MAX]; make_id(id, 55);
    TEST_ASSERT_TRUE(d.upsert(id, LOC_ID_MAX, 0xCAFE, 9, 4, 100, 1000));  // ttl 100s at t=1000
    LocBinding b{};
    TEST_ASSERT_TRUE(d.lookup_full(id, LOC_ID_MAX, &b, 1000 + 30000));    // 30s later
    TEST_ASSERT_EQUAL_HEX32(0xCAFE, b.loc);
    TEST_ASSERT_EQUAL_HEX16(9, b.epoch);
    TEST_ASSERT_EQUAL_HEX16(4, b.seq);
    TEST_ASSERT_EQUAL_UINT16(70, b.ttl_s);                               // ~70s remaining
    TEST_ASSERT_FALSE(d.lookup_full(id, LOC_ID_MAX, &b, 1000 + 200000)); // expired
}

// ---- end-to-end: QUERY -> REPLY -> cache (the resolve path) ----------------------
static void test_query_reply_resolves() {
    uint8_t id[LOC_ID_MAX]; make_id(id, 50);
    const node_id_t NX = 0x9828F51Bu;     // node serving the id
    uint8_t wire[64];

    // Node X already serves the id (it registered locally).
    LocatorDir dirX;
    TEST_ASSERT_TRUE(dirX.upsert(id, LOC_ID_MAX, NX, 7, 3, 300, 0));

    // Node Y wants it: builds a QUERY, X parses it, looks it up, builds a REPLY.
    uint16_t qn = loc_build_query(0x42, id, LOC_ID_MAX, wire, sizeof(wire));
    LocMsg q; TEST_ASSERT_TRUE(loc_parse(wire, qn, &q));
    node_id_t served = 0;
    TEST_ASSERT_TRUE(dirX.lookup(q.id, q.id_len, &served, 0));

    // (X needs the binding's epoch/seq to answer authoritatively; here we re-state them.)
    uint16_t rn = loc_build_reply(q.qid, 7, 3, 300, served, q.id, q.id_len, wire, sizeof(wire));
    LocMsg r; TEST_ASSERT_TRUE(loc_parse(wire, rn, &r));

    // Node Y caches the binding from the REPLY and can now resolve directly.
    LocatorDir dirY;
    TEST_ASSERT_TRUE(dirY.upsert(r.id, r.id_len, r.loc, r.epoch, r.seq, r.ttl_s, 0));
    node_id_t got = 0;
    TEST_ASSERT_TRUE(dirY.lookup(id, LOC_ID_MAX, &got, 0));
    TEST_ASSERT_EQUAL_HEX32(NX, got);
}

// ---- resolver: dedup + reply + timeout ------------------------------------------
static void test_resolver_lifecycle() {
    LocatorResolver rs;
    uint8_t id[LOC_ID_MAX]; make_id(id, 60);

    uint16_t q1 = rs.begin(id, LOC_ID_MAX, 0, 1000);
    TEST_ASSERT_TRUE(q1 != 0);
    TEST_ASSERT_TRUE(rs.pending(id, LOC_ID_MAX, 100));
    // Re-begin while pending -> same qid, no duplicate QUERY.
    TEST_ASSERT_EQUAL_UINT16(q1, rs.begin(id, LOC_ID_MAX, 100, 1000));

    // A REPLY clears it.
    TEST_ASSERT_TRUE(rs.on_reply(q1));
    TEST_ASSERT_FALSE(rs.pending(id, LOC_ID_MAX, 200));
    TEST_ASSERT_FALSE(rs.on_reply(q1));               // already cleared
    // New resolution gets a fresh qid.
    uint16_t q2 = rs.begin(id, LOC_ID_MAX, 200, 1000);
    TEST_ASSERT_TRUE(q2 != 0 && q2 != q1);

    // Timeout path.
    uint8_t id2[LOC_ID_MAX]; make_id(id2, 61);
    rs.begin(id2, LOC_ID_MAX, 0, 100);
    TEST_ASSERT_TRUE(rs.pending(id2, LOC_ID_MAX, 50));
    TEST_ASSERT_TRUE(rs.tick(150) >= 1);              // expires the timed-out one(s)
    TEST_ASSERT_FALSE(rs.pending(id2, LOC_ID_MAX, 150));
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_codec_register_roundtrip);
    RUN_TEST(test_codec_query_reply_roundtrip);
    RUN_TEST(test_codec_rejects_malformed);
    RUN_TEST(test_cache_hit_miss_remove);
    RUN_TEST(test_cache_ttl_expiry);
    RUN_TEST(test_cache_seq_and_mobility);
    RUN_TEST(test_cache_reboot_reregister);
    RUN_TEST(test_cache_lru_eviction);
    RUN_TEST(test_seq_wraparound);
    RUN_TEST(test_lookup_full);
    RUN_TEST(test_query_reply_resolves);
    RUN_TEST(test_resolver_lifecycle);
    return UNITY_END();
}
