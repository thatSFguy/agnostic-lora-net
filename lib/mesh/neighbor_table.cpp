#include "neighbor_table.h"
#include "link_metric.h"

namespace mesh {

Neighbor* NeighborTable::find(node_id_t id) {
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (slots_[i].used && slots_[i].id == id) return &slots_[i];
    }
    return nullptr;
}

const Neighbor* NeighborTable::find(node_id_t id) const {
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (slots_[i].used && slots_[i].id == id) return &slots_[i];
    }
    return nullptr;
}

link_addr_t NeighborTable::alloc_alias() const {
    // Smallest value in 1..254 not already assigned to a live neighbour.
    for (uint16_t cand = 1; cand <= 254; cand++) {
        bool taken = false;
        for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
            if (slots_[i].used && slots_[i].my_alias == (link_addr_t)cand) { taken = true; break; }
        }
        if (!taken) return (link_addr_t)cand;
    }
    return ALIAS_NONE;  // table effectively full of aliases (shouldn't happen with 32 slots)
}

Neighbor* NeighborTable::heard(node_id_t id, float q, uint32_t now_ms) {
    Neighbor* n = find(id);
    if (!n) {
        for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
            if (!slots_[i].used) { n = &slots_[i]; break; }
        }
        if (!n) return nullptr;  // table full
        n->used        = true;
        n->id          = id;
        n->q_rx        = -1.0f;
        n->q_tx        = -1.0f;
        n->their_alias = ALIAS_NONE;
        n->my_alias    = alloc_alias();   // assign our local alias for this neighbour
    }
    n->q_rx = ewma_quality(n->q_rx, q);
    n->last_heard_ms = now_ms;
    return n;
}

void NeighborTable::set_tx_report(node_id_t id, float q) {
    Neighbor* n = find(id);
    if (n) n->q_tx = ewma_quality(n->q_tx, q);
}

void NeighborTable::set_their_alias(node_id_t id, link_addr_t alias) {
    Neighbor* n = find(id);
    if (n) n->their_alias = alias;
}

link_addr_t NeighborTable::their_alias_for(node_id_t id) const {
    const Neighbor* n = find(id);
    return n ? n->their_alias : ALIAS_NONE;
}

link_addr_t NeighborTable::my_alias_for(node_id_t id) const {
    const Neighbor* n = find(id);
    return n ? n->my_alias : ALIAS_NONE;
}

bool NeighborTable::is_my_alias(link_addr_t a) const {
    if (a == ALIAS_NONE) return false;
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (slots_[i].used && slots_[i].my_alias == a) return true;
    }
    return false;
}

node_id_t NeighborTable::neighbor_by_my_alias(link_addr_t a) const {
    if (a == ALIAS_NONE) return 0;
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (slots_[i].used && slots_[i].my_alias == a) return slots_[i].id;
    }
    return 0;
}

float NeighborTable::tx_cost(node_id_t id) const {
    const Neighbor* n = find(id);
    // Unknown q_tx -> assume the worst usable quality so a not-yet-confirmed
    // reverse link is expensive but still routable (asymmetric links are used).
    float q = (n && n->q_tx > 0.0f) ? n->q_tx : Q_MIN;
    if (q < Q_MIN) q = Q_MIN;
    return 1.0f / q;
}

uint8_t NeighborTable::prune(uint32_t now_ms, uint32_t timeout_ms,
                             node_id_t* removed, uint8_t cap) {
    uint8_t n_removed = 0;
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (!slots_[i].used) continue;
        if (now_ms - slots_[i].last_heard_ms > timeout_ms) {
            if (removed && n_removed < cap) removed[n_removed] = slots_[i].id;
            n_removed++;
            slots_[i].used = false;
        }
    }
    return n_removed;
}

uint8_t NeighborTable::count() const {
    uint8_t c = 0;
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) if (slots_[i].used) c++;
    return c;
}

} // namespace mesh
