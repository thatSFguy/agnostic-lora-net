// node_table.cpp — NodeTable + the one crypto-dependent NodeId helper.
#include "node_table.h"
#include "monocypher.h"

namespace mesh {

NodeId nid_from_pubkey(const uint8_t pubkey[32]) {
    uint8_t full[16];
    crypto_blake2b(full, sizeof(full), pubkey, 32);   // 16-byte digest
    NodeId id;
    memcpy(id.b, full, 16);
    return id;
}

int NodeTable::find(const NodeId& id) const {
    for (int i = 0; i < NODE_TABLE_CAP; i++)
        if ((s_[i].flags & F_USED) && s_[i].id == id) return i;
    return -1;
}

int NodeTable::free_or_evict(uint32_t now_ms) {
    // Prefer a never-used slot.
    for (int i = 0; i < NODE_TABLE_CAP; i++)
        if (!(s_[i].flags & F_USED)) return i;
    // Else evict the oldest unpinned slot (LRU on last_ms, wrap-aware).
    int victim = -1;
    for (int i = 0; i < NODE_TABLE_CAP; i++) {
        if (s_[i].flags & F_PINNED) continue;
        if (victim < 0 || (int32_t)(s_[i].last_ms - s_[victim].last_ms) < 0) victim = i;
    }
    if (victim >= 0) {
        s_[victim].gen++;                 // invalidate any lingering refs to the old occupant
        s_[victim].flags = 0;             // F_USED/F_VERIFIED cleared; gen preserved
        last_evicted_ = true;
    }
    return victim;
}

node_ref NodeTable::intern(const NodeId& id, uint32_t now_ms) {
    last_evicted_ = false;
    int i = find(id);
    if (i < 0) {
        i = free_or_evict(now_ms);
        if (i < 0) return NODE_REF_NONE;  // saturated with pinned entries
        s_[i].id = id;
        s_[i].flags = F_USED;
    }
    s_[i].last_ms = now_ms;
    return node_ref{(uint8_t)i, s_[i].gen};
}

bool NodeTable::resolve(node_ref r, NodeId& out) const {
    if (r.idx >= NODE_TABLE_CAP) return false;
    const Slot& s = s_[r.idx];
    if (!(s.flags & F_USED) || s.gen != r.gen) return false;
    out = s.id;
    return true;
}

bool NodeTable::verified(node_ref r) const {
    if (r.idx >= NODE_TABLE_CAP) return false;
    const Slot& s = s_[r.idx];
    return (s.flags & F_USED) && s.gen == r.gen && (s.flags & F_VERIFIED);
}

void NodeTable::mark_verified(node_ref r) {
    if (r.idx < NODE_TABLE_CAP && (s_[r.idx].flags & F_USED) && s_[r.idx].gen == r.gen)
        s_[r.idx].flags |= F_VERIFIED;
}

void NodeTable::pin(node_ref r, bool on) {
    if (r.idx >= NODE_TABLE_CAP) return;
    Slot& s = s_[r.idx];
    if (!(s.flags & F_USED) || s.gen != r.gen) return;
    if (on) s.flags |= F_PINNED; else s.flags &= ~F_PINNED;
}

} // namespace mesh
