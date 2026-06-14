#include "forwarder.h"

namespace mesh {

bool Forwarder::seen_or_insert(node_id_t src, uint16_t pkt_id) {
    for (uint8_t i = 0; i < SEEN_CACHE_SIZE; i++) {
        if (seen_[i].used && seen_[i].src == src && seen_[i].pkt_id == pkt_id) {
            return true;  // already known
        }
    }
    // Insert at the ring head, overwriting the oldest entry.
    seen_[seen_head_].src    = src;
    seen_[seen_head_].pkt_id = pkt_id;
    seen_[seen_head_].used   = true;
    seen_head_ = (uint8_t)((seen_head_ + 1) % SEEN_CACHE_SIZE);
    return false;
}

void Forwarder::mark_seen(node_id_t src, uint16_t pkt_id) {
    seen_or_insert(src, pkt_id);
}

Decision Forwarder::decide(const PacketRef& p) {
    // Our own packet came back to us (broadcast medium) — never act on it.
    if (p.src == me_) return {Action::DROP_OWN, {}, 0};

    // Dedup: drop anything we've already handled. Records it if new.
    if (seen_or_insert(p.src, p.pkt_id)) return {Action::DROP_DUP, {}, 0};

    // Destined for us (direct or broadcast) — deliver to the app.
    if (p.dst == me_ || p.dst == BCAST_ID) return {Action::DELIVER, {}, 0};

    // Not ours: it must be relayed. A TTL of 0 or 1 can't make another hop.
    if (p.ttl <= 1) return {Action::DROP_TTL, {}, 0};

    node_id_t nh = router_.next_hop(p.dst);
    if (nh.is_zero()) return {Action::DROP_NO_ROUTE, {}, 0};

    return {Action::FORWARD, nh, (uint8_t)(p.ttl - 1)};
}

} // namespace mesh
