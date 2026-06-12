// control.h — the Tier-1 signed control plane (write path), power-first slice.
//
// READS are open (telemetry.h — the air is already public); WRITES require an
// Ed25519 signature from the network controller. Node ids authenticate nothing
// (FICR-derived, collidable): the signature + a persisted monotonic counter are
// the entire trust model. No unsigned message may ever change node state.
//
// Wire layout (LE) inside a PKT_CONTROL payload:
//   COMMAND: u8 ver=1 | u8 cmd | u32 target | i8 arg | u32 counter | sig[64]
//     cmd 1 = POWER   (arg = TX dBm to apply)
//     cmd 2 = CONFIRM (arg = the dBm being confirmed — disarms the revert)
//   signature: Ed25519 over "AGN-CTRL-1" || the 11 unsigned bytes (domain-tagged
//   so a control signature can never be confused with any future signed thing).
//
//   ACK (UNSIGNED, success-only): u8 ver=1 | u8 cmd|0x80 | u32 origin | i8 applied
//                                 | u8 provisional | u32 counter
//   Nodes hold no keypair, so ACKs are informational: they can be forged, but a
//   forged "success" only lies to a UI — it cannot change state. Failures are
//   logged locally and NEVER acked (an auth-failure oracle helps only attackers).
//
// Replay protection: the controller signs a strictly-increasing counter; each
// node persists the highest accepted value and rejects anything not above it.
// The dead-man rail (power DECREASES revert in 60 s unless CONFIRMed) lives in
// the firmware, not here — this module is pure codec + crypto.
#pragma once

#include <stdint.h>
#include "mesh_types.h"

namespace mesh {

enum CtrlCmd : uint8_t { CTRL_POWER = 1, CTRL_CONFIRM = 2 };
enum CtrlVerdict : uint8_t { CTRL_OK = 0, CTRL_MALFORMED, CTRL_BAD_SIG, CTRL_REPLAY };

constexpr uint8_t  CTRL_VER       = 1;
constexpr uint16_t CTRL_MSG_BYTES = 11 + 64;   // unsigned part + signature
constexpr uint16_t CTRL_ACK_BYTES = 12;

struct CtrlMsg {
    uint8_t   cmd     = 0;
    node_id_t target  = 0;
    int8_t    arg     = 0;
    uint32_t  counter = 0;
};

struct CtrlAck {
    uint8_t   cmd         = 0;     // original cmd (high bit stripped)
    node_id_t origin      = 0;
    int8_t    applied     = 0;
    uint8_t   provisional = 0;     // 1 = will revert unless confirmed
    uint32_t  counter     = 0;
};

// Build a signed command. `seckey` is the 64-byte Ed25519 secret key
// (monocypher layout). Returns bytes written, or 0 on bad args/cap.
uint16_t ctrl_build(uint8_t cmd, node_id_t target, int8_t arg, uint32_t counter,
                    const uint8_t seckey[64], uint8_t* out, uint16_t cap);

// Parse + verify a command against `pubkey` and the replay floor `min_counter`
// (accepts only counter > min_counter). On CTRL_OK, `out` is filled.
CtrlVerdict ctrl_verify(const uint8_t* msg, uint16_t len, const uint8_t pubkey[32],
                        uint32_t min_counter, CtrlMsg* out);

// Is this payload a control ACK (vs a command)?
bool ctrl_is_ack(const uint8_t* msg, uint16_t len);

uint16_t ctrl_build_ack(uint8_t cmd, node_id_t origin, int8_t applied,
                        uint8_t provisional, uint32_t counter,
                        uint8_t* out, uint16_t cap);
bool     ctrl_parse_ack(const uint8_t* msg, uint16_t len, CtrlAck* out);

} // namespace mesh
