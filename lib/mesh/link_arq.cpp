#include "link_arq.h"
#include <string.h>

namespace mesh {

uint8_t LinkArq::next_seq() {
    do { seq_ctr_++; } while (seq_ctr_ == 0);  // 0 is reserved / "unset"
    return seq_ctr_;
}

bool LinkArq::track(uint8_t link_seq, node_id_t next_hop, const uint8_t* frame,
                    uint16_t len, uint32_t now_ms, uint32_t timeout_ms, uint8_t max_retries) {
    if (len > ARQ_FRAME_MAX) return false;     // too big to buffer for retransmit

    Pending* slot = nullptr;
    for (uint8_t i = 0; i < ARQ_MAX_PENDING; i++) {
        if (!slots_[i].used) { slot = &slots_[i]; break; }
    }
    if (!slot) return false;                   // table full

    slot->used         = true;
    slot->seq          = link_seq;
    slot->next_hop     = next_hop;
    slot->retries_left = max_retries;
    slot->timeout_ms   = timeout_ms;
    slot->next_tx_ms   = now_ms + timeout_ms;
    slot->len          = len;
    memcpy(slot->frame, frame, len);
    return true;
}

bool LinkArq::on_ack(uint8_t link_seq) {
    for (uint8_t i = 0; i < ARQ_MAX_PENDING; i++) {
        if (slots_[i].used && slots_[i].seq == link_seq) {
            slots_[i].used = false;
            acked_++;
            return true;
        }
    }
    return false;  // unknown/duplicate ACK
}

uint8_t LinkArq::tick(uint32_t now_ms, ArqResendFn fn, void* ctx) {
    uint8_t dropped_now = 0;
    for (uint8_t i = 0; i < ARQ_MAX_PENDING; i++) {
        Pending& p = slots_[i];
        if (!p.used) continue;
        if ((int32_t)(now_ms - p.next_tx_ms) < 0) continue;  // not due yet (wrap-safe)

        if (p.retries_left == 0) {
            p.used = false;       // out of retries — give up
            dropped_++;
            dropped_now++;
            continue;
        }
        p.retries_left--;
        p.next_tx_ms = now_ms + p.timeout_ms;
        if (fn) fn(ctx, p.frame, p.len);
    }
    return dropped_now;
}

uint8_t LinkArq::pending_count() const {
    uint8_t c = 0;
    for (uint8_t i = 0; i < ARQ_MAX_PENDING; i++) if (slots_[i].used) c++;
    return c;
}

} // namespace mesh
