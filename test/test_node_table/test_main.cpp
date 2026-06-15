// Host unit tests for the interned node-id directory (docs/node-id-widening-impl.md §1).
//
//   pio test -e native
//
// The NodeTable is the only place a full 16-byte NodeId lives in RAM; everything
// else keys on a 2-byte node_ref. These tests pin the contract the routing core
// relies on: stable interning, dedup, gen-tagged stale-ref detection on eviction,
// pin-protection of live entries, and once-per-id verification state.

#include <unity.h>
#include "node_table.h"

using namespace mesh;

static NodeId idn(uint32_t v) { return nid_from_u32(v); }

// intern() returns a ref that resolves back to the same id.
static void test_intern_resolve_roundtrip() {
    NodeTable t;
    node_ref r = t.intern(idn(0x1111), 100);
    TEST_ASSERT_FALSE(ref_is_none(r));
    NodeId out;
    TEST_ASSERT_TRUE(t.resolve(r, out));
    TEST_ASSERT_TRUE(out == idn(0x1111));
}

// Interning the same id twice yields the same ref (dedup); distinct ids differ.
static void test_dedup_and_distinct() {
    NodeTable t;
    node_ref a1 = t.intern(idn(0xAA), 1);
    node_ref a2 = t.intern(idn(0xAA), 2);
    node_ref b  = t.intern(idn(0xBB), 3);
    TEST_ASSERT_TRUE(a1 == a2);
    TEST_ASSERT_TRUE(a1 != b);
}

// verified state is off until set, then sticks for that id.
static void test_verified_flag() {
    NodeTable t;
    node_ref r = t.intern(idn(0x42), 1);
    TEST_ASSERT_FALSE(t.verified(r));
    uint8_t pub[32]; for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(0x42 + i);
    t.mark_verified(r, pub);
    TEST_ASSERT_TRUE(t.verified(r));
    // re-interning the same id keeps the verified flag (same slot).
    node_ref r2 = t.intern(idn(0x42), 2);
    TEST_ASSERT_TRUE(t.verified(r2));
}

// Filling past capacity evicts the LRU slot; the bumped gen makes the stale ref
// resolve false (it does NOT silently alias to the new occupant).
static void test_eviction_bumps_gen() {
    NodeTable t;
    node_ref first = t.intern(idn(1), 10);   // oldest
    for (uint32_t i = 2; i <= NODE_TABLE_CAP; i++) t.intern(idn(i), 10 + i);  // fills the table
    // One more distinct id forces an eviction of the LRU (id 1).
    node_ref overflow = t.intern(idn(0xDEAD), 100000);
    TEST_ASSERT_FALSE(ref_is_none(overflow));
    TEST_ASSERT_TRUE(t.last_evicted());

    NodeId out;
    TEST_ASSERT_FALSE(t.resolve(first, out));   // stale ref to the evicted slot
}

// A pinned slot is protected from eviction even when it is the LRU.
static void test_pin_protects_lru() {
    NodeTable t;
    node_ref pinned = t.intern(idn(1), 10);   // would be the LRU
    t.pin(pinned, true);
    node_ref second = t.intern(idn(2), 11);   // next-oldest, unpinned
    for (uint32_t i = 3; i <= NODE_TABLE_CAP; i++) t.intern(idn(i), 10 + i);  // fills the table
    t.intern(idn(0xBEEF), 100000);            // forces an eviction

    NodeId out;
    TEST_ASSERT_TRUE(t.resolve(pinned, out));  // pinned survived
    TEST_ASSERT_TRUE(out == idn(1));
    TEST_ASSERT_FALSE(t.resolve(second, out)); // the unpinned LRU got evicted
}

// nid_from_pubkey is deterministic and separates distinct keys.
static void test_nid_from_pubkey() {
    uint8_t k1[32]; uint8_t k2[32];
    for (int i = 0; i < 32; i++) { k1[i] = (uint8_t)i; k2[i] = (uint8_t)(i + 1); }
    NodeId a = nid_from_pubkey(k1);
    NodeId a2 = nid_from_pubkey(k1);
    NodeId b = nid_from_pubkey(k2);
    TEST_ASSERT_TRUE(a == a2);     // deterministic
    TEST_ASSERT_TRUE(a != b);      // distinct keys -> distinct ids
    TEST_ASSERT_FALSE(a.is_zero());
}

// for_each_verified re-emits exactly the verified slots, with the retained pubkey.
struct DumpCtx { int n; NodeId ids[8]; uint8_t pubs[8][32]; };
static void dump_cb(void* ctx, const NodeId& id, const uint8_t pub[32]) {
    DumpCtx* d = (DumpCtx*)ctx;
    if (d->n < 8) { d->ids[d->n] = id; memcpy(d->pubs[d->n], pub, 32); d->n++; }
}
static void test_for_each_verified_reemits_pub() {
    NodeTable t;
    uint8_t pa[32], pb[32];
    for (int i = 0; i < 32; i++) { pa[i] = (uint8_t)(0xA0 + i); pb[i] = (uint8_t)(0xB0 + i); }
    node_ref ra = t.intern(idn(0xAA), 1);
    node_ref rb = t.intern(idn(0xBB), 2);
    t.intern(idn(0xCC), 3);                 // interned but NOT verified — must be skipped
    t.mark_verified(ra, pa);
    t.mark_verified(rb, pb);
    DumpCtx d{}; t.for_each_verified(dump_cb, &d);
    TEST_ASSERT_EQUAL_INT(2, d.n);          // only the two verified
    // find AA's entry and check its pubkey round-tripped
    int ai = (d.ids[0] == idn(0xAA)) ? 0 : 1;
    TEST_ASSERT_TRUE(d.ids[ai] == idn(0xAA));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pa, d.pubs[ai], 32);
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_intern_resolve_roundtrip);
    RUN_TEST(test_dedup_and_distinct);
    RUN_TEST(test_verified_flag);
    RUN_TEST(test_eviction_bumps_gen);
    RUN_TEST(test_pin_protects_lru);
    RUN_TEST(test_nid_from_pubkey);
    RUN_TEST(test_for_each_verified_reemits_pub);
    return UNITY_END();
}
