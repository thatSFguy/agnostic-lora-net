// Host unit tests for link-local neighbour alias negotiation (Agent.md §5).
//
//   pio test -e native
//
// Aliases are 1-byte, link-local, and each node numbers its own neighbours. The
// contract that must hold: the alias I stamp as next_hop to reach a neighbour is
// the alias THAT neighbour assigned to me — so it lands in its own alias space.

#include <unity.h>
#include "router.h"

using namespace mesh;

static const node_id_t NA = 0xAu, NB = 0xBu, NC = 0xCu;

// Exchange announces between A and B until aliases settle.
static void converge_pair(Router& A, Router& B, int rounds) {
    uint32_t now = 1000;
    for (int r = 0; r < rounds; r++) {
        Announce aA, aB;
        A.build_announce(aA);
        B.build_announce(aB);
        A.on_beacon(B.id(), 0.9f, aB, now);
        B.on_beacon(A.id(), 0.9f, aA, now);
        now += 10000;
    }
}

// Distinct neighbours get distinct, non-zero local aliases.
static void test_alias_allocation_distinct() {
    Router A(NA);
    Announce empty;
    A.on_beacon(NB, 0.9f, empty, 1000);
    A.on_beacon(NC, 0.9f, empty, 1000);

    const Neighbor* nb = A.neighbors().find(NB);
    const Neighbor* nc = A.neighbors().find(NC);
    TEST_ASSERT_NOT_NULL(nb);
    TEST_ASSERT_NOT_NULL(nc);
    TEST_ASSERT_NOT_EQUAL(ALIAS_NONE, nb->my_alias);
    TEST_ASSERT_NOT_EQUAL(ALIAS_NONE, nc->my_alias);
    TEST_ASSERT_NOT_EQUAL(nb->my_alias, nc->my_alias);
}

// After negotiation, the alias A uses to reach B equals the alias B assigned to A
// (and vice versa) — frames land in the receiver's own alias space.
static void test_alias_negotiation_symmetry() {
    Router A(NA), B(NB);
    converge_pair(A, B, 4);

    link_addr_t b_for_a = B.neighbors().find(NA)->my_alias;  // alias B assigned to A
    link_addr_t a_for_b = A.neighbors().find(NB)->my_alias;  // alias A assigned to B

    // What A stamps to reach B == B's alias for A.
    TEST_ASSERT_EQUAL_UINT8(b_for_a, A.link_addr_for(NB));
    // What B stamps to reach A == A's alias for B.
    TEST_ASSERT_EQUAL_UINT8(a_for_b, B.link_addr_for(NA));
}

// The receive-side resolvers: a frame addressed with the alias A will stamp is
// recognised by B as "for me, from A".
static void test_resolver_receive_side() {
    Router A(NA), B(NB);
    converge_pair(A, B, 4);

    link_addr_t stamped_by_a = A.link_addr_for(NB);   // == B.my_alias(A)
    TEST_ASSERT_TRUE(B.is_my_alias(stamped_by_a));
    TEST_ASSERT_EQUAL_HEX32(NA, B.neighbors().neighbor_by_my_alias(stamped_by_a));

    // An alias nobody was assigned is not ours.
    TEST_ASSERT_FALSE(B.is_my_alias(200));
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_alias_allocation_distinct);
    RUN_TEST(test_alias_negotiation_symmetry);
    RUN_TEST(test_resolver_receive_side);
    return UNITY_END();
}
