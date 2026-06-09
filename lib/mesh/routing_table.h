// routing_table.h — distance-vector routes with per-direction cost (Agent.md §6).
//
// Babel-inspired DV (Babel is proven on exactly this kind of lossy/asymmetric
// link). The key to Req 3 (independent forward/return paths) is that the link
// cost used to reach a neighbour is DIRECTIONAL — it's the cost of frames
// travelling *toward* that neighbour (1/q_tx). Because each node computes its own
// routes using its own directional costs, the path A->C and the path C->A fall
// out independently; they are never required to match.
//
// Loop avoidance for this first cut: split horizon (a receiver ignores an
// advertised route whose next hop is the receiver itself — see Router) plus a
// hop-count ceiling that bounds count-to-infinity. Babel's full feasibility /
// sequence-number machinery is the documented follow-up.
#pragma once

#include <stdint.h>
#include "mesh_types.h"

namespace mesh {

constexpr uint8_t MAX_ROUTES = 64;
constexpr uint8_t MAX_HOPS   = 16;     // hop ceiling == "infinity" for this DV
constexpr float   COST_INF   = 1.0e6f; // unreachable

struct Route {
    node_id_t dst        = 0;
    node_id_t next_hop   = 0;
    float     cost       = COST_INF;
    uint8_t   hops       = 0;
    uint32_t  updated_ms = 0;
    bool      used       = false;
};

class RoutingTable {
public:
    // Pin the self-route (dst == me, cost 0). Never overwritten by offers.
    void set_self(node_id_t me, uint32_t now_ms);

    // Offer a candidate route to `dst` via `next_hop`. Accepted if it's strictly
    // better, OR if it comes from the current next hop (so worsening/withdrawal of
    // the in-use path is honoured). Rejected if it exceeds the hop/cost ceiling.
    // Returns true if the table changed.
    bool offer(node_id_t dst, node_id_t next_hop, float cost, uint8_t hops, uint32_t now_ms);

    const Route* find(node_id_t dst) const;

    // Next hop to reach `dst`, or 0 if no route.
    node_id_t next_hop(node_id_t dst) const;

    // Drop routes older than `timeout_ms` (self-route exempt).
    void prune(uint32_t now_ms, uint32_t timeout_ms);

    // Drop every route whose next hop is `via` (used when a neighbour dies, for
    // immediate reroute instead of waiting for the timeout).
    void drop_via(node_id_t via);

    uint8_t count() const;
    const Route* at(uint8_t i) const { return &slots_[i]; }  // iterate 0..MAX_ROUTES, check .used

private:
    Route*   find_mut(node_id_t dst);
    Route*   alloc(node_id_t dst);

    node_id_t self_ = 0;
    Route     slots_[MAX_ROUTES];
};

} // namespace mesh
