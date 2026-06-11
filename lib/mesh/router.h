// router.h — the routing brain: neighbours + per-direction DV, driven by beacons.
//
// This is the part Agent.md §9 calls out as genuinely ours to build ("asymmetric
// routing is ours to build, not inherited"). It is deliberately transport- and
// platform-agnostic: it consumes an Announce (whatever carried it — a LoRa beacon
// on-device, a synthetic topology in a host test) and answers "what's the next hop
// to D". It performs no I/O and knows nothing about RadioLib, BLE, or MeshCore;
// the firmware feeds it and the MeshCore-fork seam will call next_hop() to forward.
#pragma once

#include <stdint.h>
#include "mesh_types.h"
#include "neighbor_table.h"
#include "routing_table.h"

namespace mesh {

// Default liveness windows. A neighbour/route unseen for this long is dropped.
constexpr uint32_t NEIGHBOR_TIMEOUT_MS = 90000;   // ~9 missed 10 s beacons
constexpr uint32_t ROUTE_TIMEOUT_MS    = 90000;

// Max links a node can have administratively blocked at once.
constexpr uint8_t MAX_BLOCKED = 8;

// What a node tells its neighbours each beacon: how well it hears each of them
// (so they can learn their reverse/TX quality) plus its current routing table
// (so distance-vector can propagate). Fixed-size; serialised into the beacon
// payload on-air (serialisation is the next integration step).
struct Announce {
    // `alias` is the alias the sender assigned to this neighbour (my_alias). The
    // neighbour reads its own entry to learn the alias to address the sender by.
    struct Report { node_id_t id; float q; link_addr_t alias; };
    struct RouteAdv { node_id_t dst; node_id_t next_hop; float cost; uint8_t hops; };

    node_id_t origin = 0;
    Report    reports[MAX_NEIGHBORS];
    uint8_t   n_reports = 0;
    RouteAdv  routes[MAX_ROUTES];
    uint8_t   n_routes = 0;
};

class Router {
public:
    explicit Router(node_id_t my_id);

    node_id_t id() const { return my_id_; }

    // Process a received beacon: update the sender's link quality (both
    // directions), then relax routes from its advertised table.
    void on_beacon(node_id_t from, float instant_q_rx, const Announce& a, uint32_t now_ms);

    // Produce this node's outgoing announce.
    void build_announce(Announce& out) const;

    // Age out dead neighbours and the routes that depended on them.
    void tick(uint32_t now_ms,
              uint32_t neighbor_timeout_ms = NEIGHBOR_TIMEOUT_MS,
              uint32_t route_timeout_ms    = ROUTE_TIMEOUT_MS);

    // Forwarding decision: next hop toward `dst`, or 0 if unknown.
    node_id_t next_hop(node_id_t dst) const { return routes_.next_hop(dst); }

    // --- administrative link blocking (Agent.md §4/§6 Tier-1 "block a bad link") ---
    // Block the link to neighbour `id`: its beacons are ignored and existing routes
    // through it are torn down, so the node routes *around* it. This is the node-local
    // half of the controller's "block a bad/asymmetric link" command; the same API is
    // driven now by local config and later by an authenticated control packet. Also
    // used on the bench to force a multi-hop path between in-range nodes.
    bool block(node_id_t id);
    void unblock(node_id_t id);
    bool is_blocked(node_id_t id) const;
    uint8_t blocked_count() const { return n_blocked_; }

    // Link addressing (Agent.md §5): resolve aliases for directed sends.
    link_addr_t link_addr_for(node_id_t dst) const { return neighbors_.their_alias_for(dst); }
    link_addr_t my_alias_for(node_id_t dst)  const { return neighbors_.my_alias_for(dst); }
    bool        is_my_alias(link_addr_t a)   const { return neighbors_.is_my_alias(a); }
    // Strict directed-frame ownership: the neighbour matching BOTH header fields
    // (next_hop = my alias for the link, prev_hop = its alias for me), else 0.
    node_id_t   link_sender(link_addr_t next_hop, link_addr_t prev_hop) const {
        return neighbors_.neighbor_by_link(next_hop, prev_hop);
    }

    const NeighborTable& neighbors() const { return neighbors_; }
    const RoutingTable&  routes()    const { return routes_; }

private:
    node_id_t     my_id_;
    NeighborTable neighbors_;
    RoutingTable  routes_;
    node_id_t     blocked_[MAX_BLOCKED] = {};
    uint8_t       n_blocked_ = 0;
};

} // namespace mesh
