// announce_codec.h — compact, airtime-conscious wire format for an Announce.
//
// Beacons carry the routing state (Agent.md §6: "Updates piggyback on beacons").
// Airtime is the scarcest resource (§2.4), so the encoding is tight and quantised:
//   * link quality q ∈ [0,1]  -> 1 byte  (q * 255)
//   * route cost              -> 2 bytes (cost * 16, saturating)
// The sender's node ID is NOT carried here — it's already in the network header,
// so the receiver knows who sent the announce.
//
// All multi-byte fields are little-endian, written byte-by-byte (no struct
// aliasing / alignment assumptions), so the format is identical on every target.
//
// On-wire layout (immediately follows BeaconPayload in the beacon):
//   u8  n_reports
//   u8  n_routes
//   n_reports × { id[16], u8 q_q, u8 alias }
//   n_routes  × { id[16] dst, id[16] next_hop, u16 cost_q, u8 hops }
#pragma once

#include <stdint.h>
#include "router.h"

namespace mesh {

constexpr uint16_t ANNOUNCE_REPORT_BYTES = 18;  // id[16] + u8 + u8 (id, q, alias)
constexpr uint16_t ANNOUNCE_ROUTE_BYTES  = 35;  // id[16] + id[16] + u16 + u8
constexpr uint16_t ANNOUNCE_HDR_BYTES    = 2;   // n_reports + n_routes

// Serialise `a` into `buf` (capacity `cap`). Reports are written first, then as
// many routes as still fit — so a too-small buffer degrades gracefully rather than
// failing (the count bytes reflect what was actually written). Returns the number
// of bytes written, or 0 if not even the 2-byte header fits.
uint16_t announce_serialize(const Announce& a, uint8_t* buf, uint16_t cap);

// Parse an announce from `buf`/`len` into `out`. Bounds-checked against the buffer
// and against the fixed array capacities (this is untrusted data off the radio).
// Returns false on any truncation or malformed count, leaving `out` cleared.
bool announce_deserialize(const uint8_t* buf, uint16_t len, Announce& out);

} // namespace mesh
