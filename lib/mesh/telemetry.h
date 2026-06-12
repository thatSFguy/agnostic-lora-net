// telemetry.h — sparse node-health telemetry (battery + remote status).
//
// Two mechanisms, both deliberately stingy with airtime (Agent.md §2.4 and
// docs/node-map-webapp-plan.md §6):
//
//   * BATT flood — each node floods its battery (~7 B payload) every ~6 h,
//     jittered. Every node caches every node's last report, so connecting to
//     ANY gateway shows last-known battery for the whole mesh (`battdump`)
//     without querying anyone. 4 floods/node/day ≈ nothing, even at SF11.
//   * STATUS query/reply — on-demand multi-hop telemetry for the map app:
//     a TTL-bounded QUERY flood names a target; the target answers the asker
//     with a DV-routed unicast REPLY (battery, uptime, fw, neighbour qualities).
//     Read-only by construction; the REPLY is rate-limited BY TARGET (one per
//     cooldown regardless of asker) so query floods can't be used to make a
//     node burn airtime — see the plan's security posture.
//
// Wire layout (LE) inside a PKT_TELEM payload:
//   BATT   : u8 kind=1 | u16 mv | u8 pct_plus1
//   QUERY  : u8 kind=2 | u32 target
//   REPLY  : u8 kind=3 | u16 mv | u8 pct_plus1 | u16 uptime_min |
//            u8 fw_len | fw_len×char | u8 n_nbrs | n×{u32 id, u8 q_rx, u8 q_tx}
// pct_plus1: 0 = unknown/uncalibrated, 1..101 = 0..100 % (matches the beacon byte).
#pragma once

#include <stdint.h>
#include <string.h>
#include "mesh_types.h"

namespace mesh {

enum TelemKind : uint8_t { TELEM_BATT = 1, TELEM_QUERY = 2, TELEM_REPLY = 3 };

constexpr uint8_t  TELEM_FW_MAX   = 12;   // fw version string cap in a REPLY
constexpr uint8_t  TELEM_NBR_MAX  = 16;   // neighbour entries cap in a REPLY
constexpr uint16_t TELEM_CACHE_CAP = 16;  // battery reports cached per node

struct TelemNbr { node_id_t id; uint8_t q_rx; uint8_t q_tx; };   // q ×100

// A parsed TELEM message (fields populated per kind).
struct TelemMsg {
    uint8_t   kind        = 0;
    uint16_t  mv          = 0;     // BATT / REPLY
    uint8_t   pct_plus1   = 0;     // BATT / REPLY
    node_id_t target      = 0;     // QUERY
    uint16_t  uptime_min  = 0;     // REPLY
    int8_t    power_dbm   = 0;     // REPLY — current TX power
    uint8_t   sf          = 0;     // REPLY — current spreading factor
    char      fw[TELEM_FW_MAX + 1] = {};
    uint8_t   n_nbrs      = 0;     // REPLY
    TelemNbr  nbrs[TELEM_NBR_MAX];
};

// Builders return bytes written, or 0 if `cap` is too small / args out of range.
uint16_t telem_build_batt(uint16_t mv, uint8_t pct_plus1, uint8_t* out, uint16_t cap);
uint16_t telem_build_query(node_id_t target, uint8_t* out, uint16_t cap);
uint16_t telem_build_reply(uint16_t mv, uint8_t pct_plus1, uint16_t uptime_min,
                           int8_t power_dbm, uint8_t sf,
                           const char* fw, const TelemNbr* nbrs, uint8_t n_nbrs,
                           uint8_t* out, uint16_t cap);
// Parse any TELEM message. Returns true only for a well-formed message.
bool telem_parse(const uint8_t* msg, uint16_t len, TelemMsg* out);

// Per-node cache of last-known battery per origin (LRU on overflow).
class TelemCache {
public:
    void upsert(node_id_t origin, uint16_t mv, uint8_t pct_plus1, uint32_t now_ms);

    struct View { node_id_t origin; uint16_t mv; uint8_t pct_plus1; uint32_t age_ms; };
    // Copy up to `cap` entries into `out`; returns how many were written.
    uint16_t snapshot(View* out, uint16_t cap, uint32_t now_ms) const;

private:
    struct Entry { node_id_t origin = 0; uint16_t mv = 0; uint8_t pct_plus1 = 0;
                   uint32_t ms = 0; bool used = false; };
    Entry e_[TELEM_CACHE_CAP];
};

} // namespace mesh
