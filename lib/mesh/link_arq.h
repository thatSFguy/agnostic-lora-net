// link_arq.h — hop-by-hop acknowledgement + small retry (Agent.md §3 "Reliability").
//
// Each directed unicast (a frame addressed to a specific next hop's alias) can ask
// that hop for a tiny link-layer ACK. This engine remembers a sent frame by its
// 1-byte link sequence number; if the ACK doesn't come back within a timeout it
// retransmits, up to a small retry limit, then gives up. This is per-hop — every
// relay runs its own ARQ for its own onward hop — so reliability is local and
// independent of the end-to-end path (which may be asymmetric).
//
// Pure logic, no radio: time is injected, and retransmissions are handed back to
// the caller through a callback. Unit-tested in test/test_arq.
#pragma once

#include <stdint.h>
#include "mesh_types.h"

namespace mesh {

constexpr uint8_t  ARQ_MAX_PENDING = 4;     // frames awaiting ACK at once
constexpr uint8_t  ARQ_MAX_RETRIES = 5;     // retransmits before giving up
// Wait per try. Must exceed a full frame's airtime + the ACK round-trip: at
// SF11/BW250 a max (~200 B) frame is ~2 s on air, so a sub-second timeout would
// retransmit before the first TX even finished. 5 s leaves margin.
constexpr uint32_t ARQ_TIMEOUT_MS  = 5000;
constexpr uint16_t ARQ_FRAME_MAX   = 200;   // cover a full SAR fragment (~178 B)

// Called by tick() for each frame that needs (re)transmission now.
typedef void (*ArqResendFn)(void* ctx, const uint8_t* frame, uint16_t len);

class LinkArq {
public:
    // Next link sequence number to stamp (node-wide, skips 0).
    uint8_t next_seq();

    // Remember a just-sent unicast frame so it can be retried until ACKed. The
    // frame must already carry `link_seq` and the ACK-request flag. Returns false
    // (frame still sent, just untracked) if the frame is too big or the table full.
    bool track(uint8_t link_seq, node_id_t next_hop, const uint8_t* frame, uint16_t len,
               uint32_t now_ms, uint32_t timeout_ms = ARQ_TIMEOUT_MS,
               uint8_t max_retries = ARQ_MAX_RETRIES);

    // An ACK for `link_seq` arrived — clear the pending entry. True if it matched.
    bool on_ack(uint8_t link_seq);

    // Advance time: retransmit any frame whose timeout elapsed (via `fn`), or give
    // up on those out of retries. Returns how many were given up this call.
    uint8_t tick(uint32_t now_ms, ArqResendFn fn, void* ctx);

    uint8_t  pending_count() const;
    uint32_t acked_count()   const { return acked_; }
    uint32_t dropped_count() const { return dropped_; }

private:
    struct Pending {
        bool      used         = false;
        uint8_t   seq          = 0;
        node_id_t next_hop     = 0;
        uint8_t   retries_left = 0;
        uint32_t  next_tx_ms   = 0;
        uint32_t  timeout_ms   = 0;
        uint16_t  len          = 0;
        uint8_t   frame[ARQ_FRAME_MAX];
    };

    Pending  slots_[ARQ_MAX_PENDING];
    uint8_t  seq_ctr_ = 0;
    uint32_t acked_   = 0;
    uint32_t dropped_ = 0;
};

} // namespace mesh
