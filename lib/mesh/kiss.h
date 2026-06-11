// kiss.h — KISS TNC framing (header-only, portable, host-tested).
//
// KISS is the packet-radio ecosystem's lingua franca: FEND-delimited frames, a
// 1-byte command, byte-stuffing for the two special values. Speaking it on the
// node's USB console makes the node a plain TNC to stock software (Reticulum's
// KISSInterface, APRS tooling) — zero custom code on the host.
//
//   frame    := FEND cmd escaped-payload FEND
//   FEND     = 0xC0   FESC = 0xDB
//   escaping : 0xC0 -> 0xDB 0xDC      0xDB -> 0xDB 0xDD
//   cmd      : low nibble 0 = data (high nibble = port); 0xFF = exit KISS mode
//
// KISS carries NO addressing — the firmware pins data frames to a configured
// destination (`kiss <node-id>`); see docs/tcp-bridge.md.
#pragma once

#include <stdint.h>

namespace mesh {

constexpr uint8_t  KISS_FEND  = 0xC0;
constexpr uint8_t  KISS_FESC  = 0xDB;
constexpr uint8_t  KISS_TFEND = 0xDC;
constexpr uint8_t  KISS_TFESC = 0xDD;
constexpr uint8_t  KISS_CMD_DATA   = 0x00;
constexpr uint8_t  KISS_CMD_RETURN = 0xFF;
constexpr uint16_t KISS_MAX_FRAME  = 1 + 800;   // cmd + payload (≥ TUN_HOST_MAX)

// Encode one frame (FEND + cmd + escaped payload + FEND) into `out`.
// Returns bytes written, or 0 if `cap` can't hold the worst case.
inline uint16_t kiss_encode(uint8_t cmd, const uint8_t* in, uint16_t len,
                            uint8_t* out, uint16_t cap) {
    uint16_t n = 0;
    if (cap < 4) return 0;
    out[n++] = KISS_FEND;
    out[n++] = cmd;                      // cmd 0x00/0xFF never needs escaping
    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = in[i];
        if (b == KISS_FEND)      { if (n + 3 > cap) return 0; out[n++] = KISS_FESC; out[n++] = KISS_TFEND; }
        else if (b == KISS_FESC) { if (n + 3 > cap) return 0; out[n++] = KISS_FESC; out[n++] = KISS_TFESC; }
        else                     { if (n + 2 > cap) return 0; out[n++] = b; }
    }
    out[n++] = KISS_FEND;
    return n;
}

// Streaming decoder: feed bytes; returns true when a complete frame (cmd +
// payload) is ready in frame()/len(). Bytes outside FEND framing are ignored
// (console text can interleave without confusing it). Oversize frames are
// discarded whole.
class KissDecoder {
public:
    bool feed(uint8_t b) {
        if (b == KISS_FEND) {
            if (in_ && n_ > 0) { ready_ = n_; in_ = false; n_ = 0; esc_ = false; return true; }
            in_ = true; n_ = 0; esc_ = false;     // open (or empty re-sync) frame
            return false;
        }
        if (!in_) return false;
        uint8_t v = b;
        if (esc_) {
            if (b == KISS_TFEND)      v = KISS_FEND;
            else if (b == KISS_TFESC) v = KISS_FESC;
            // anything else: protocol error — keep the byte as-is (lenient)
            esc_ = false;
        } else if (b == KISS_FESC) {
            esc_ = true;
            return false;
        }
        if (n_ < KISS_MAX_FRAME) buf_[n_++] = v;
        else { in_ = false; n_ = 0; }            // oversize: drop the frame
        return false;
    }
    const uint8_t* frame() const { return buf_; }   // [0] = cmd, payload follows
    uint16_t       len()   const { return ready_; } // cmd + payload bytes

private:
    uint8_t  buf_[KISS_MAX_FRAME];
    uint16_t n_ = 0, ready_ = 0;
    bool     in_ = false, esc_ = false;
};

} // namespace mesh
