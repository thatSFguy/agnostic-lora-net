#include "control.h"
#include <string.h>
#include "monocypher.h"
#include "monocypher-ed25519.h"

namespace mesh {

// Domain tag: a control signature can never be mistaken for any other signed
// message this network may grow (telemetry signing, firmware blobs, ...).
static const uint8_t DOMAIN[10] = {'A','G','N','-','C','T','R','L','-','1'};
static const uint16_t UNSIGNED_BYTES = 11;     // ver|cmd|target|arg|counter

static void put_u32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static uint32_t get_u32(const uint8_t* p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }

// The exact byte string that is signed: DOMAIN || unsigned-part.
static uint16_t signed_view(const uint8_t* unsigned_part, uint8_t* out) {
    memcpy(out, DOMAIN, sizeof(DOMAIN));
    memcpy(out + sizeof(DOMAIN), unsigned_part, UNSIGNED_BYTES);
    return (uint16_t)(sizeof(DOMAIN) + UNSIGNED_BYTES);
}

uint16_t ctrl_build(uint8_t cmd, node_id_t target, int8_t arg, uint32_t counter,
                    const uint8_t seckey[64], uint8_t* out, uint16_t cap) {
    if (cap < CTRL_MSG_BYTES || target == 0) return 0;
    if (cmd != CTRL_POWER && cmd != CTRL_CONFIRM) return 0;
    out[0] = CTRL_VER;
    out[1] = cmd;
    put_u32(out + 2, (uint32_t)target);
    out[6] = (uint8_t)arg;
    put_u32(out + 7, counter);
    uint8_t view[sizeof(DOMAIN) + UNSIGNED_BYTES];
    uint16_t vn = signed_view(out, view);
    crypto_ed25519_sign(out + UNSIGNED_BYTES, seckey, view, vn);
    return CTRL_MSG_BYTES;
}

CtrlVerdict ctrl_verify(const uint8_t* msg, uint16_t len, const uint8_t pubkey[32],
                        uint32_t min_counter, CtrlMsg* out) {
    if (!msg || len < CTRL_MSG_BYTES || msg[0] != CTRL_VER) return CTRL_MALFORMED;
    uint8_t cmd = msg[1];
    if (cmd != CTRL_POWER && cmd != CTRL_CONFIRM) return CTRL_MALFORMED;

    uint8_t view[sizeof(DOMAIN) + UNSIGNED_BYTES];
    uint16_t vn = signed_view(msg, view);
    if (crypto_ed25519_check(msg + UNSIGNED_BYTES, pubkey, view, vn) != 0)
        return CTRL_BAD_SIG;

    uint32_t counter = get_u32(msg + 7);
    // Replay floor AFTER the signature check: a forged counter must never be
    // able to probe the floor, and a replayed-but-valid message is the case
    // this rejects.
    if (counter <= min_counter) return CTRL_REPLAY;

    if (out) {
        out->cmd = cmd;
        out->target = (node_id_t)get_u32(msg + 2);
        out->arg = (int8_t)msg[6];
        out->counter = counter;
    }
    return CTRL_OK;
}

bool ctrl_is_ack(const uint8_t* msg, uint16_t len) {
    return msg && len >= CTRL_ACK_BYTES && msg[0] == CTRL_VER && (msg[1] & 0x80) != 0;
}

uint16_t ctrl_build_ack(uint8_t cmd, node_id_t origin, int8_t applied,
                        uint8_t provisional, uint32_t counter,
                        uint8_t* out, uint16_t cap) {
    if (cap < CTRL_ACK_BYTES) return 0;
    out[0] = CTRL_VER;
    out[1] = (uint8_t)(cmd | 0x80);
    put_u32(out + 2, (uint32_t)origin);
    out[6] = (uint8_t)applied;
    out[7] = provisional;
    put_u32(out + 8, counter);
    return CTRL_ACK_BYTES;
}

bool ctrl_parse_ack(const uint8_t* msg, uint16_t len, CtrlAck* out) {
    if (!ctrl_is_ack(msg, len)) return false;
    if (out) {
        out->cmd = (uint8_t)(msg[1] & 0x7F);
        out->origin = (node_id_t)get_u32(msg + 2);
        out->applied = (int8_t)msg[6];
        out->provisional = msg[7];
        out->counter = get_u32(msg + 8);
    }
    return true;
}

} // namespace mesh
