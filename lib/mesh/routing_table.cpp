#include "routing_table.h"

namespace mesh {

Route* RoutingTable::find_mut(node_id_t dst) {
    for (uint8_t i = 0; i < MAX_ROUTES; i++) {
        if (slots_[i].used && slots_[i].dst == dst) return &slots_[i];
    }
    return nullptr;
}

const Route* RoutingTable::find(node_id_t dst) const {
    for (uint8_t i = 0; i < MAX_ROUTES; i++) {
        if (slots_[i].used && slots_[i].dst == dst) return &slots_[i];
    }
    return nullptr;
}

Route* RoutingTable::alloc(node_id_t dst) {
    for (uint8_t i = 0; i < MAX_ROUTES; i++) {
        if (!slots_[i].used) {
            slots_[i] = Route{};
            slots_[i].used = true;
            slots_[i].dst  = dst;
            return &slots_[i];
        }
    }
    return nullptr;  // table full
}

void RoutingTable::set_self(node_id_t me, uint32_t now_ms) {
    self_ = me;
    Route* r = find_mut(me);
    if (!r) r = alloc(me);
    if (!r) return;
    r->dst        = me;
    r->next_hop   = me;
    r->cost       = 0.0f;
    r->hops       = 0;
    r->updated_ms = now_ms;
}

bool RoutingTable::offer(node_id_t dst, node_id_t next_hop, float cost,
                         uint8_t hops, uint32_t now_ms) {
    if (dst == self_) return false;            // never override the self-route
    if (hops > MAX_HOPS || cost >= COST_INF) {
        // Unreachable advertisement. If it came from our current next hop, treat
        // it as a withdrawal of that route.
        Route* r = find_mut(dst);
        if (r && r->next_hop == next_hop) { r->used = false; return true; }
        return false;
    }

    Route* r = find_mut(dst);
    if (!r) {
        r = alloc(dst);
        if (!r) return false;                  // table full
        r->next_hop = next_hop;
        r->cost     = cost;
        r->hops     = hops;
        r->updated_ms = now_ms;
        return true;
    }

    const float EPS = 1e-4f;
    if (r->next_hop == next_hop) {
        // Refresh from the in-use next hop, even if cost got worse.
        r->cost = cost;
        r->hops = hops;
        r->updated_ms = now_ms;
        return true;
    }
    if (cost < r->cost - EPS) {                 // strictly better alternative
        r->next_hop = next_hop;
        r->cost     = cost;
        r->hops     = hops;
        r->updated_ms = now_ms;
        return true;
    }
    return false;
}

node_id_t RoutingTable::next_hop(node_id_t dst) const {
    const Route* r = find(dst);
    return r ? r->next_hop : node_id_t{};
}

void RoutingTable::prune(uint32_t now_ms, uint32_t timeout_ms) {
    for (uint8_t i = 0; i < MAX_ROUTES; i++) {
        if (!slots_[i].used) continue;
        if (slots_[i].dst == self_) continue;
        if (now_ms - slots_[i].updated_ms > timeout_ms) slots_[i].used = false;
    }
}

void RoutingTable::drop_via(node_id_t via) {
    for (uint8_t i = 0; i < MAX_ROUTES; i++) {
        if (slots_[i].used && slots_[i].dst != self_ && slots_[i].next_hop == via) {
            slots_[i].used = false;
        }
    }
}

uint8_t RoutingTable::count() const {
    uint8_t c = 0;
    for (uint8_t i = 0; i < MAX_ROUTES; i++) if (slots_[i].used) c++;
    return c;
}

} // namespace mesh
