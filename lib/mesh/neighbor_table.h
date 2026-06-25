// neighbor_table.h — direct neighbours and their per-direction link quality.
//
// The asymmetry model: for each neighbour we track TWO qualities,
// independently:
//   q_rx  — how well *I hear them*  (learned locally from frames I receive)
//   q_tx  — how well *they hear me*  (learned from their report back to me)
// A link is flagged `asym` when the two differ by more than a threshold. One-way
// links (good in one direction, dead in the other) are first-class citizens —
// they get *used*, not discarded.
//
// Fixed-capacity, no heap: routing state lives in RAM and rebuilds from announces
// on boot (nothing here is ever persisted to flash).
#pragma once

#include <stdint.h>
#include "mesh_types.h"

namespace mesh {

constexpr uint8_t MAX_NEIGHBORS = 32;

// |q_tx - q_rx| above this marks the link asymmetric.
constexpr float ASYM_THRESHOLD = 0.35f;

// Reserved link-alias values. Real aliases are 1..254.
constexpr link_addr_t ALIAS_NONE      = 0x00;
constexpr link_addr_t ALIAS_BROADCAST = 0xFF;

struct Neighbor {
    node_id_t id        = {};
    float     q_rx      = -1.0f;  // <0 = unknown
    float     q_tx      = -1.0f;  // <0 = unknown (no report received yet)
    // Link-local aliases (1 byte each):
    //   my_alias    — the alias I assigned to this neighbour. I advertise it; the
    //                 neighbour stamps it as next_hop when sending to me.
    //   their_alias — the alias this neighbour assigned to me. I stamp it as
    //                 next_hop when sending to them. 0 until learned from their announce.
    link_addr_t my_alias    = ALIAS_NONE;
    link_addr_t their_alias = ALIAS_NONE;
    uint32_t  last_heard_ms = 0;
    bool      used      = false;

    bool asymmetric() const {
        if (q_rx < 0.0f || q_tx < 0.0f) return false;
        float d = q_rx - q_tx;
        if (d < 0) d = -d;
        return d > ASYM_THRESHOLD;
    }
};

class NeighborTable {
public:
    // Record that we heard `id` with instantaneous quality `q` (EWMA-folded into
    // q_rx). Inserts the neighbour if new. Returns nullptr if the table is full.
    Neighbor* heard(node_id_t id, float q, uint32_t now_ms);

    // Record a neighbour's report of how well it hears us (sets q_tx). No-op if
    // we don't know the neighbour yet.
    void set_tx_report(node_id_t id, float q);

    // Record the alias this neighbour assigned to us (their_alias), learned when
    // their announce reports us. No-op if we don't know the neighbour yet.
    void set_their_alias(node_id_t id, link_addr_t alias);

    // --- link addressing resolvers ---
    // Alias to stamp as next_hop to reach `id` (their_alias), 0 if not negotiated.
    link_addr_t their_alias_for(node_id_t id) const;
    // Alias I assigned to `id` (my_alias / what I stamp as prev_hop), 0 if unknown.
    link_addr_t my_alias_for(node_id_t id) const;
    // Is `a` one of the aliases I assigned? (i.e. a frame with next_hop==a is for me)
    bool is_my_alias(link_addr_t a) const;
    // Which neighbour did I assign alias `a` to? 0 if none.
    node_id_t neighbor_by_my_alias(link_addr_t a) const;
    // STRICT directed-frame match: the neighbour for which next_hop is the alias
    // I assigned AND prev_hop is the alias it assigned to me — both fields must
    // agree on the same entry, or 0. is_my_alias() alone is ambiguous on a
    // broadcast medium once 3+ nodes each run their own alias space.
    node_id_t neighbor_by_link(link_addr_t next_hop, link_addr_t prev_hop) const;

    // Seed for id-derived alias allocation (call once, before any heard()):
    // spreads each node's alias range so different assigners rarely hand out
    // numerically identical aliases. See alloc_alias().
    void set_alias_seed(node_id_t my_id) { alias_base_ = (uint16_t)(nid_fold(my_id) % 254); }

    Neighbor* find(node_id_t id);
    const Neighbor* find(node_id_t id) const;

    // Cost of sending TO `id` (a frame travelling me -> id), = 1 / q_tx, i.e. it
    // depends on how well *they* hear *me*. Returns a large finite cost when q_tx
    // is unknown/zero so the link is usable-but-expensive, never infinite.
    float tx_cost(node_id_t id) const;

    // Drop neighbours not heard within `timeout_ms`. Removed IDs are written to
    // `removed` (up to `cap`); returns how many were removed.
    uint8_t prune(uint32_t now_ms, uint32_t timeout_ms, node_id_t* removed, uint8_t cap);

    uint8_t count() const;
    const Neighbor* at(uint8_t i) const { return &slots_[i]; }  // iterate 0..MAX_NEIGHBORS, check .used

private:
    // First free alias in 1..254 walking cyclically from the id-derived base
    // (unique among current neighbours).
    link_addr_t alloc_alias() const;

    Neighbor slots_[MAX_NEIGHBORS];
    uint16_t  alias_base_ = 0;   // id-derived start of this node's alias range
};

} // namespace mesh
