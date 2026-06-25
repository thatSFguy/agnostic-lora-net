// Host unit tests for the portable routing core (lib/mesh). No hardware, no
// Arduino — the same logic that runs on the nRF52, driven by synthetic topologies.
//
//   pio test -e native
//
// What's proven here is the project's core novelty: link-quality
// routing, and independent forward/return paths over asymmetric links (Req 3).

#include <unity.h>
#include <vector>
#include <functional>
#include "router.h"
#include "link_metric.h"

using namespace mesh;

// Node IDs used across the tests.
static const node_id_t NA = nid_from_u32(0xA), NB = nid_from_u32(0xB), NC = nid_from_u32(0xC), ND = nid_from_u32(0xD);

// A tiny synchronous distance-vector simulator. `q(from,to)` is the DIRECTIONAL
// receive quality of a frame sent by `from` and heard at `to` (0 == no link).
struct Sim {
    std::vector<Router*> nodes;
    std::function<float(node_id_t, node_id_t)> q;
    uint32_t now = 1000;

    Router* get(node_id_t id) {
        for (Router* n : nodes) if (n->id() == id) return n;
        return nullptr;
    }

    // One DV round: every active node builds its announce from current state,
    // then all announces are delivered (broadcast) to in-range neighbours.
    void round(const std::vector<node_id_t>& active) {
        std::vector<Announce> ann(active.size());
        for (size_t i = 0; i < active.size(); i++) get(active[i])->build_announce(ann[i]);

        for (size_t i = 0; i < active.size(); i++) {
            node_id_t s = active[i];
            for (node_id_t r : active) {
                if (r == s) continue;
                float qsr = q(s, r);              // quality heard at r from s
                if (qsr <= 0.0f) continue;        // out of range
                get(r)->on_beacon(s, qsr, ann[i], now);
            }
        }
        now += 10000;                            // one beacon period
    }

    void converge(const std::vector<node_id_t>& active, int rounds) {
        for (int i = 0; i < rounds; i++) round(active);
    }
};

// --- link metric -----------------------------------------------------------
static void test_link_metric_monotonic_and_clamped() {
    float q_low  = quality_from_rf(-30.0f, -100.0f);  // below demod floor
    float q_mid  = quality_from_rf(-5.0f,  -100.0f);
    float q_high = quality_from_rf(15.0f,  -100.0f);   // above "good"

    TEST_ASSERT_TRUE(q_low <= q_mid);
    TEST_ASSERT_TRUE(q_mid <= q_high);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, Q_MIN, q_low);    // clamped to floor
    TEST_ASSERT_FLOAT_WITHIN(0.001f, Q_MAX, q_high);   // clamped to ceiling
}

static void test_link_metric_ewma() {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, ewma_quality(-1.0f, 0.5f));   // first sample taken as-is
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, ewma_quality(0.5f, 0.5f));    // steady state
    float up = ewma_quality(0.2f, 1.0f);                                  // moves toward sample
    TEST_ASSERT_TRUE(up > 0.2f && up < 1.0f);
}

// --- routing: line topology A-B-C ------------------------------------------
static void test_line_topology() {
    Router A(NA), B(NB), C(NC);
    Sim sim; sim.nodes = {&A, &B, &C};
    sim.q = [](node_id_t f, node_id_t t) -> float {
        auto link = [&](node_id_t x, node_id_t y) {
            return (f == x && t == y) || (f == y && t == x);
        };
        if (link(NA, NB)) return 0.9f;
        if (link(NB, NC)) return 0.9f;
        return 0.0f;                              // A and C cannot hear each other
    };

    sim.converge({NA, NB, NC}, 8);

    // A reaches C only through B; the return is symmetric here.
    TEST_ASSERT_TRUE(NB == A.next_hop(NC));
    TEST_ASSERT_TRUE(NB == C.next_hop(NA));
    TEST_ASSERT_TRUE(NB == A.next_hop(NB));   // direct neighbour
}

// --- routing: asymmetric ring, independent per-direction paths (Req 3) -----
// Strong "clockwise" ring A->B->C->D->A; weak counter-clockwise. The best path
// A->C must differ from the best path C->A.
static void test_asymmetric_per_direction() {
    Router A(NA), B(NB), C(NC), D(ND);
    Sim sim; sim.nodes = {&A, &B, &C, &D};
    sim.q = [](node_id_t f, node_id_t t) -> float {
        // Strong direction = 0.9, weak reverse = 0.1, around the ring.
        if (f == NA && t == NB) return 0.9f;
        if (f == NB && t == NA) return 0.1f;
        if (f == NB && t == NC) return 0.9f;
        if (f == NC && t == NB) return 0.1f;
        if (f == NC && t == ND) return 0.9f;
        if (f == ND && t == NC) return 0.1f;
        if (f == ND && t == NA) return 0.9f;
        if (f == NA && t == ND) return 0.1f;
        return 0.0f;
    };

    sim.converge({NA, NB, NC, ND}, 16);

    // Forward A->C goes the strong clockwise way (via B); return C->A goes the
    // strong clockwise way from C's perspective (via D). Different paths — exactly
    // the asymmetric-routing premise.
    TEST_ASSERT_TRUE(NB == A.next_hop(NC));
    TEST_ASSERT_TRUE(ND == C.next_hop(NA));
}

// --- routing: reroute when the preferred relay goes silent ------------------
static void test_reroute_on_relay_loss() {
    Router A(NA), B(NB), C(NC), D(ND);
    Sim sim; sim.nodes = {&A, &B, &C, &D};
    sim.q = [](node_id_t f, node_id_t t) -> float {
        auto link = [&](node_id_t x, node_id_t y) {
            return (f == x && t == y) || (f == y && t == x);
        };
        if (link(NA, NB)) return 0.9f;   // cheap relay
        if (link(NB, NC)) return 0.9f;
        if (link(NA, ND)) return 0.6f;   // pricier backup relay
        if (link(ND, NC)) return 0.6f;
        return 0.0f;
    };

    sim.converge({NA, NB, NC, ND}, 12);
    TEST_ASSERT_TRUE(NB == A.next_hop(NC));   // prefers the cheap path via B

    // B goes silent. Age everyone past the neighbour timeout and let routes that
    // depended on B fall away.
    sim.now += NEIGHBOR_TIMEOUT_MS + 10000;
    for (Router* n : sim.nodes) n->tick(sim.now);

    sim.converge({NA, NC, ND}, 12);                // B no longer transmits
    TEST_ASSERT_TRUE(ND == A.next_hop(NC));   // rerouted onto the backup
    TEST_ASSERT_TRUE(ND == C.next_hop(NA));
}

// --- administrative link block forces a multi-hop path (Tier-1 "block a link") ---
// All three nodes are in direct range of each other (full triangle). Blocking the
// A<->C link makes A and C route to each other *through B*, even though they could
// hear each other directly.
static void test_blocked_link_forces_relay() {
    Router A(NA), B(NB), C(NC);
    Sim sim; sim.nodes = {&A, &B, &C};
    sim.q = [](node_id_t f, node_id_t t) -> float {
        auto link = [&](node_id_t x, node_id_t y) {
            return (f == x && t == y) || (f == y && t == x);
        };
        if (link(NA, NB) || link(NB, NC) || link(NA, NC)) return 0.9f;  // full mesh
        return 0.0f;
    };

    // Block the direct A<->C link at both ends (what the controller would push to
    // each endpoint to block that link).
    TEST_ASSERT_TRUE(A.block(NC));
    TEST_ASSERT_TRUE(C.block(NA));

    sim.converge({NA, NB, NC}, 8);

    // A and C now reach each other only via B; B still reaches both directly.
    TEST_ASSERT_TRUE(NB == A.next_hop(NC));
    TEST_ASSERT_TRUE(NB == C.next_hop(NA));
    TEST_ASSERT_TRUE(NC == B.next_hop(NC));
    TEST_ASSERT_TRUE(A.is_blocked(NC));
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_link_metric_monotonic_and_clamped);
    RUN_TEST(test_link_metric_ewma);
    RUN_TEST(test_line_topology);
    RUN_TEST(test_asymmetric_per_direction);
    RUN_TEST(test_reroute_on_relay_loss);
    RUN_TEST(test_blocked_link_forces_relay);
    return UNITY_END();
}
