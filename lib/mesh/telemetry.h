// telemetry.h — sparse node-health telemetry (battery + remote status).
//
// Two mechanisms, both deliberately stingy with airtime:
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
//   QUERY  : u8 kind=2 | id[16] target
//   REPLY  : u8 kind=3 | u16 mv | u8 pct_plus1 | u16 uptime_min | i8 power_dbm | u8 sf |
//            u8 flags | u8 fw_len | fw_len×char | u8 n_nbrs |
//            n×{id[16], u8 q_rx, u8 q_tx, i8 snr, i16 rssi}
// flags bit0 = node is mobile (operator-set on the node; the controller keeps its TX-power
//   headroom and never trims it). The flag lives on the node so any controller learns it.
// pct_plus1: 0 = unknown/uncalibrated, 1..101 = 0..100 % (matches the beacon byte).
// snr/rssi: the RF at which the reporting node last heard this neighbour (the q_rx
//   direction). 0/0 = unmeasured. Lets the controller put REMOTE links on a true SNR
//   margin (Phase 2) so it can trim their power, not just raise — see controller/.
// NOTE: the nbr entry grew 6->9 B in this revision; kind stays 3, so all nodes must run
//   matching firmware (a star bench reflash). Mixed versions reject each other's replies
//   via the length check rather than mis-parsing.
#pragma once

#include <stdint.h>
#include <string.h>
#include "mesh_types.h"

namespace mesh {

enum TelemKind : uint8_t { TELEM_BATT = 1, TELEM_QUERY = 2, TELEM_REPLY = 3 };

constexpr uint8_t  TELEM_FW_MAX   = 12;   // fw version string cap in a REPLY
constexpr uint8_t  TELEM_NAME_MAX = 20;   // operator "friendly" name cap in a REPLY
constexpr uint8_t  TELEM_NBR_MAX  = 16;   // neighbour entries cap in a REPLY
constexpr uint16_t TELEM_CACHE_CAP = 16;  // battery reports cached per node

struct TelemNbr {
    node_id_t id;
    uint8_t   q_rx;          // q ×100
    uint8_t   q_tx;          // q ×100
    int8_t    snr  = 0;      // dB, q_rx direction (0 = unmeasured)
    int16_t   rssi = 0;      // dBm, q_rx direction (0 = unmeasured)
};

// A parsed TELEM message (fields populated per kind).
struct TelemMsg {
    uint8_t   kind        = 0;
    uint16_t  mv          = 0;     // BATT / REPLY
    uint8_t   pct_plus1   = 0;     // BATT / REPLY
    node_id_t target      = {};    // QUERY
    uint16_t  uptime_min  = 0;     // REPLY
    int8_t    power_dbm   = 0;     // REPLY — current TX power
    uint8_t   sf          = 0;     // REPLY — current spreading factor
    uint8_t   flags       = 0;     // REPLY — bit0: node is mobile
    char      fw[TELEM_FW_MAX + 1] = {};
    char      name[TELEM_NAME_MAX + 1] = {};  // REPLY — operator friendly name ("" = none)
    uint8_t   n_nbrs      = 0;     // REPLY
    TelemNbr  nbrs[TELEM_NBR_MAX];
};

// Builders return bytes written, or 0 if `cap` is too small / args out of range.
uint16_t telem_build_batt(uint16_t mv, uint8_t pct_plus1, uint8_t* out, uint16_t cap);
uint16_t telem_build_query(node_id_t target, uint8_t* out, uint16_t cap);
uint16_t telem_build_reply(uint16_t mv, uint8_t pct_plus1, uint16_t uptime_min,
                           int8_t power_dbm, uint8_t sf, uint8_t flags,
                           const char* fw, const char* name,
                           const TelemNbr* nbrs, uint8_t n_nbrs,
                           uint8_t* out, uint16_t cap);

constexpr uint8_t TELEM_FLAG_MOBILE = 0x01;
constexpr uint8_t TELEM_FLAG_BLE    = 0x02;   // REPLY: node is currently BLE-advertising
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
    struct Entry { node_id_t origin = {}; uint16_t mv = 0; uint8_t pct_plus1 = 0;
                   uint32_t ms = 0; bool used = false; };
    Entry e_[TELEM_CACHE_CAP];
};

} // namespace mesh
