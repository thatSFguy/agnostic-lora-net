// forwarder.h — the relay decision: deliver, forward, or drop a packet.
//
// This is the data-plane counterpart to the routing brain (router.h, the control
// plane). The router decides WHERE the next hop is; the forwarder decides what to
// DO with a packet that just arrived, using that next hop. It is pure logic — no
// radio, no buffers of its own beyond a small dedup cache — so the relay policy is
// unit-testable host-side and identical on every node.
//
// Loop / storm control (critical on a broadcast medium where every neighbour hears
// every frame): a fixed-size seen-cache drops any (src, pkt_id) we've already
// handled, and the TTL bounds how far a packet can travel. Together they stop the
// rebroadcast-forwarding used here (no link-layer unicast yet) from melting down.
#pragma once

#include <stdint.h>
#include "mesh_types.h"
#include "router.h"

namespace mesh {

// Identity of a packet for routing/dedup purposes, lifted from the network header.
struct PacketRef {
    node_id_t src;
    node_id_t dst;
    uint16_t  pkt_id;
    uint8_t   ttl;
};

enum class Action : uint8_t {
    DELIVER,        // for us — hand to the app
    FORWARD,        // relay onward (see Decision.next_hop / out_ttl)
    DROP_OWN,       // our own packet looped back
    DROP_DUP,       // already handled this (src, pkt_id)
    DROP_TTL,       // hop limit reached, can't relay further
    DROP_NO_ROUTE,  // no known next hop toward dst
};

struct Decision {
    Action    action;
    node_id_t next_hop;  // valid when action == FORWARD
    uint8_t   out_ttl;   // TTL to stamp on the forwarded frame
};

// Broadcast value matching packet.h's NODE_ID_BROADCAST (kept local so the mesh
// library stays independent of the wire-format header).
constexpr node_id_t BCAST_ID = 0xFFFFFFFFu;

constexpr uint8_t SEEN_CACHE_SIZE = 64;

class Forwarder {
public:
    Forwarder(node_id_t me, Router& router) : me_(me), router_(router) {}

    // Decide what to do with a freshly received packet. Records it in the dedup
    // cache as a side effect (so an immediate re-hear is DROP_DUP).
    Decision decide(const PacketRef& p);

    // Originate-side helper: register a packet we're about to send so our own
    // rebroadcast (heard back over the air) is recognised as a duplicate.
    void mark_seen(node_id_t src, uint16_t pkt_id);

private:
    bool seen_or_insert(node_id_t src, uint16_t pkt_id);

    struct SeenEntry { node_id_t src; uint16_t pkt_id; bool used; };

    node_id_t me_;
    Router&   router_;
    SeenEntry seen_[SEEN_CACHE_SIZE] = {};
    uint8_t   seen_head_ = 0;  // ring insertion point
};

} // namespace mesh
