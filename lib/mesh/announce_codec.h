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

// --- signed-announce identity tail (docs/node-keygen-signed-announce-impl.md §5/6) ---
// A beacon's serialized announce body is followed by [pubkey:32][sig:64]; the Ed25519
// signature covers "AGN-ANN-1" || pubkey || announce-body (domain-tagged, so it can never
// be confused with a control signature). This proves the sender owns the key whose hash is
// its node id. The id == blake2b(pubkey) check is the CALLER's (it needs node_table).
constexpr uint16_t ANNOUNCE_SIG_TAIL = 32 + 64;   // pubkey + signature

// Number of body bytes a parsed announce occupies on the wire (header + entries).
uint16_t announce_body_len(const Announce& a);

// Write the identity tail (pubkey + signature over the domain-tagged view) for the
// serialized announce `body`/`body_len`. `tail` must have room for ANNOUNCE_SIG_TAIL.
void announce_sign(const uint8_t* body, uint16_t body_len,
                   const uint8_t pubkey[32], const uint8_t seckey[64], uint8_t* tail);

// Verify an identity `tail` (pubkey+sig) against the announce `body`/`body_len`. Returns
// true iff the signature checks out; on success `out_pubkey` (32 B) is the sender's key.
bool announce_verify(const uint8_t* body, uint16_t body_len,
                     const uint8_t* tail, uint8_t out_pubkey[32]);

} // namespace mesh
