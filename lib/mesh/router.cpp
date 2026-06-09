#include "router.h"

namespace mesh {

Router::Router(node_id_t my_id) : my_id_(my_id) {
    routes_.set_self(my_id_, 0);
}

bool Router::block(node_id_t id) {
    if (id == 0 || id == my_id_) return false;
    if (is_blocked(id)) return true;
    if (n_blocked_ >= MAX_BLOCKED) return false;
    blocked_[n_blocked_++] = id;
    routes_.drop_via(id);   // tear down any routes that used this link right now
    return true;
}

void Router::unblock(node_id_t id) {
    for (uint8_t i = 0; i < n_blocked_; i++) {
        if (blocked_[i] == id) {
            blocked_[i] = blocked_[n_blocked_ - 1];  // compact
            n_blocked_--;
            return;
        }
    }
}

bool Router::is_blocked(node_id_t id) const {
    for (uint8_t i = 0; i < n_blocked_; i++) {
        if (blocked_[i] == id) return true;
    }
    return false;
}

void Router::on_beacon(node_id_t from, float instant_q_rx, const Announce& a, uint32_t now_ms) {
    if (from == my_id_) return;       // never route off our own echo
    if (is_blocked(from)) return;     // administratively blocked link — treat as down

    // 1) Learn the link. q_rx from the frame we just received; q_tx from the
    //    sender's report of how well it hears us.
    neighbors_.heard(from, instant_q_rx, now_ms);
    for (uint8_t i = 0; i < a.n_reports; i++) {
        if (a.reports[i].id == my_id_) {
            neighbors_.set_tx_report(from, a.reports[i].q);
            neighbors_.set_their_alias(from, a.reports[i].alias);  // alias to address `from` by
            break;
        }
    }
    routes_.set_self(my_id_, now_ms);

    // 2) Relax routes. Cost of the first hop (me -> from) is directional: it
    //    depends on how well `from` hears me (1/q_tx). This is what makes the
    //    forward and reverse paths independent.
    const float link_cost = neighbors_.tx_cost(from);

    for (uint8_t i = 0; i < a.n_routes; i++) {
        const Announce::RouteAdv& adv = a.routes[i];
        if (adv.dst == my_id_)      continue;  // that's us
        if (adv.next_hop == my_id_) continue;  // split horizon: their path loops through us

        float   cand_cost = link_cost + adv.cost;
        uint8_t cand_hops = (uint8_t)(adv.hops + 1);
        routes_.offer(adv.dst, from, cand_cost, cand_hops, now_ms);
    }
}

void Router::build_announce(Announce& out) const {
    out.origin    = my_id_;
    out.n_reports = 0;
    out.n_routes  = 0;

    // Report how well we hear each neighbour, so they learn their TX quality.
    for (uint8_t i = 0; i < MAX_NEIGHBORS && out.n_reports < MAX_NEIGHBORS; i++) {
        const Neighbor* n = neighbors_.at(i);
        if (!n->used || n->q_rx < 0.0f) continue;
        out.reports[out.n_reports].id    = n->id;
        out.reports[out.n_reports].q     = n->q_rx;
        out.reports[out.n_reports].alias = n->my_alias;   // tell them the alias to use for me
        out.n_reports++;
    }

    // Advertise our routing table (including the self-route, dst==me cost 0).
    for (uint8_t i = 0; i < MAX_ROUTES && out.n_routes < MAX_ROUTES; i++) {
        const Route* r = routes_.at(i);
        if (!r->used) continue;
        out.routes[out.n_routes].dst      = r->dst;
        out.routes[out.n_routes].next_hop = r->next_hop;
        out.routes[out.n_routes].cost     = r->cost;
        out.routes[out.n_routes].hops     = r->hops;
        out.n_routes++;
    }
}

void Router::tick(uint32_t now_ms, uint32_t neighbor_timeout_ms, uint32_t route_timeout_ms) {
    node_id_t removed[MAX_NEIGHBORS];
    uint8_t n = neighbors_.prune(now_ms, neighbor_timeout_ms, removed, MAX_NEIGHBORS);
    for (uint8_t i = 0; i < n; i++) {
        routes_.drop_via(removed[i]);  // immediate reroute, don't wait for timeout
    }
    routes_.prune(now_ms, route_timeout_ms);
}

} // namespace mesh
