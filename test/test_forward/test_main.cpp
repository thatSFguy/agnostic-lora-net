// Host unit tests for the relay decision engine (lib/mesh/forwarder), driven by a
// real Router populated over a line topology A-B-C.
//
//   pio test -e native

#include <unity.h>
#include <vector>
#include "router.h"
#include "forwarder.h"

using namespace mesh;

static const node_id_t NA = 0xAu, NB = 0xBu, NC = 0xCu;

// Minimal synchronous DV driver for the line A-B-C (q = 0.9 on each hop).
static void converge_line(Router& A, Router& B, Router& C, int rounds) {
    std::vector<Router*> nodes = {&A, &B, &C};
    auto q = [](node_id_t f, node_id_t t) -> float {
        auto link = [&](node_id_t x, node_id_t y) {
            return (f == x && t == y) || (f == y && t == x);
        };
        if (link(NA, NB) || link(NB, NC)) return 0.9f;
        return 0.0f;
    };
    uint32_t now = 1000;
    for (int r = 0; r < rounds; r++) {
        std::vector<Announce> ann(nodes.size());
        for (size_t i = 0; i < nodes.size(); i++) nodes[i]->build_announce(ann[i]);
        for (size_t i = 0; i < nodes.size(); i++) {
            node_id_t s = nodes[i]->id();
            for (Router* rcv : nodes) {
                if (rcv->id() == s) continue;
                float qsr = q(s, rcv->id());
                if (qsr <= 0.0f) continue;
                rcv->on_beacon(s, qsr, ann[i], now);
            }
        }
        now += 10000;
    }
}

// A packet addressed to us is delivered.
static void test_deliver_to_self() {
    Router A(NA), B(NB), C(NC);
    converge_line(A, B, C, 8);
    Forwarder fb(NB, B);
    Decision d = fb.decide({NA, NB, 1, 8});
    TEST_ASSERT_EQUAL(Action::DELIVER, d.action);
}

// At B, a packet A->C is relayed to C with TTL decremented.
static void test_forward_with_ttl_decrement() {
    Router A(NA), B(NB), C(NC);
    converge_line(A, B, C, 8);
    Forwarder fb(NB, B);
    Decision d = fb.decide({NA, NC, 2, 8});
    TEST_ASSERT_EQUAL(Action::FORWARD, d.action);
    TEST_ASSERT_EQUAL_HEX32(NC, d.next_hop);   // B reaches C directly
    TEST_ASSERT_EQUAL_UINT8(7, d.out_ttl);

    // At A, the same destination relays via B.
    Forwarder fa(NA, A);
    Decision da = fa.decide({0x99u, NC, 3, 8});
    TEST_ASSERT_EQUAL(Action::FORWARD, da.action);
    TEST_ASSERT_EQUAL_HEX32(NB, da.next_hop);
}

// No route -> dropped.
static void test_drop_no_route() {
    Router A(NA), B(NB), C(NC);
    converge_line(A, B, C, 8);
    Forwarder fb(NB, B);
    Decision d = fb.decide({NA, 0xDEADu, 4, 8});
    TEST_ASSERT_EQUAL(Action::DROP_NO_ROUTE, d.action);
}

// TTL too low to relay -> dropped.
static void test_drop_ttl_exhausted() {
    Router A(NA), B(NB), C(NC);
    converge_line(A, B, C, 8);
    Forwarder fb(NB, B);
    TEST_ASSERT_EQUAL(Action::DROP_TTL, fb.decide({NA, NC, 5, 1}).action);
    TEST_ASSERT_EQUAL(Action::DROP_TTL, fb.decide({NA, NC, 6, 0}).action);
}

// The same (src, pkt_id) heard twice -> second is a duplicate.
static void test_dedup() {
    Router A(NA), B(NB), C(NC);
    converge_line(A, B, C, 8);
    Forwarder fb(NB, B);
    Decision first  = fb.decide({NA, NC, 7, 8});
    Decision second = fb.decide({NA, NC, 7, 8});
    TEST_ASSERT_EQUAL(Action::FORWARD, first.action);
    TEST_ASSERT_EQUAL(Action::DROP_DUP, second.action);
}

// Our own packet looping back over the air -> dropped, never re-forwarded.
static void test_drop_own_source() {
    Router A(NA), B(NB), C(NC);
    converge_line(A, B, C, 8);
    Forwarder fb(NB, B);
    TEST_ASSERT_EQUAL(Action::DROP_OWN, fb.decide({NB, NC, 8, 8}).action);
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_deliver_to_self);
    RUN_TEST(test_forward_with_ttl_decrement);
    RUN_TEST(test_drop_no_route);
    RUN_TEST(test_drop_ttl_exhausted);
    RUN_TEST(test_dedup);
    RUN_TEST(test_drop_own_source);
    return UNITY_END();
}
