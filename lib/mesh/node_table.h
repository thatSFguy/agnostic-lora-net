// node_table.h — the interned node-id directory.
//
// The ONLY place a full 16-byte NodeId is stored in RAM. Every other table keys on
// a 2-byte node_ref into here, so widening ids from 4→16 B stays ~RAM-neutral and a
// node's identity signature is verified ONCE (at intern), not per frame.
//
// Eviction: bounded (NODE_TABLE_CAP). Cold, unpinned slots are reused LRU; the
// slot's `gen` is bumped on reuse so any lingering node_ref resolves false rather
// than aliasing to the new occupant. Pin live neighbours / active route dsts.
#pragma once

#include <stdint.h>
#include "mesh_types.h"

namespace mesh {

#ifndef NODE_TABLE_CAP
#define NODE_TABLE_CAP 64   // distinct nodes tracked at once; ~54 B/slot. Tunable per target.
#endif

// node id = blake2b(pubkey)[0:16]. The one crypto-dependent NodeId helper.
NodeId nid_from_pubkey(const uint8_t pubkey[32]);

class NodeTable {
public:
    // Find `id`, or insert it (reusing a cold slot if full). Returns NODE_REF_NONE
    // only if the table is saturated with pinned entries. `now_ms` drives LRU.
    node_ref intern(const NodeId& id, uint32_t now_ms);

    // Look up a previously-interned id. Returns false if the ref is stale (the slot
    // was evicted/reused) or never valid.
    bool resolve(node_ref r, NodeId& out) const;

    // Convenience: true if both refs resolve to the same live slot.
    bool same(node_ref a, node_ref b) const { return !ref_is_none(a) && a == b; }

    // Identity verification state (set once after the self-sig check succeeds). The
    // verifying pubkey is retained so the binding can be RE-EMITTED on demand (anndump):
    // the gateway prints the controller `[ann] <id> sig=ok` proof only once per boot, so
    // a controller that (re)connects later would otherwise never learn it.
    bool verified(node_ref r) const;
    void mark_verified(node_ref r, const uint8_t pub[32]);

    // Re-emit every verified id↔pubkey binding (for `anndump` / controller resync on
    // connect). `fn` is called once per verified slot. Plain fn-ptr + ctx (no heap).
    void for_each_verified(void (*fn)(void* ctx, const NodeId& id, const uint8_t pub[32]),
                           void* ctx) const;

    // Protect a slot from eviction while it is a live neighbour / active route dst.
    void pin(node_ref r, bool on);

    // True if the last intern() had to evict (directory is under pressure) — caller
    // may log it; a saturated directory in a large mesh is a real signal.
    bool last_evicted() const { return last_evicted_; }

private:
    enum : uint8_t { F_USED = 1 << 0, F_VERIFIED = 1 << 1, F_PINNED = 1 << 2 };
    struct Slot { NodeId id; uint32_t last_ms; uint8_t flags; uint8_t gen; uint8_t pub[32]; };
    Slot s_[NODE_TABLE_CAP] = {};
    bool last_evicted_ = false;

    int find(const NodeId& id) const;        // slot index or -1
    int free_or_evict(uint32_t now_ms);      // slot index or -1 (all pinned)
};

} // namespace mesh
