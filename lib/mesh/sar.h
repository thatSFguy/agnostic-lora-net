// sar.h — segmentation & reassembly for payloads larger than one LoRa frame.
//
// The backbone moves opaque packets of a few hundred bytes; anything bigger is an
// application's problem. This is that application layer: it chops a
// blob into fixed chunks, ships each as an ordinary DATA payload (carried by the
// mesh's routing + per-hop ARQ), and reassembles + integrity-checks the whole thing
// on the far side. Pure/portable — unit-tested on the host, runs on the nRF52.
//
// Fragment payload layout (inside a DATA packet's payload):
//   'S''A''R''1'  (4)   magic, so the receiver can tell fragments from plain data
//   u16 xfer_id   (2)   identifies one transfer (LE)
//   u16 frag_idx  (2)   0 .. frag_count-1
//   u16 frag_count(2)   total fragments
//   u32 total_len (4)   full blob length
//   u32 total_crc (4)   CRC-32 of the whole blob (same in every fragment)
//   chunk[]             up to SAR_CHUNK bytes (last fragment may be shorter)
#pragma once

#include <stdint.h>
#include <string.h>

namespace mesh {

constexpr uint16_t SAR_HDR_BYTES = 18;     // magic(4)+xfer(2)+idx(2)+cnt(2)+len(4)+crc(4)
constexpr uint16_t SAR_CHUNK     = 160;    // payload bytes per fragment (fits a DATA frame)
constexpr uint16_t SAR_MAX_FILE  = 8192;   // largest blob we buffer (RAM-bounded)
constexpr uint16_t SAR_MAX_FRAGS = (SAR_MAX_FILE + SAR_CHUNK - 1) / SAR_CHUNK + 1;

// CRC-32 (IEEE / zlib polynomial, reflected) — matches Python's zlib.crc32.
uint32_t sar_crc32(const uint8_t* data, uint32_t len);

// Number of fragments a blob of `len` bytes needs.
inline uint16_t sar_frag_count(uint32_t len) {
    return (uint16_t)((len + SAR_CHUNK - 1) / SAR_CHUNK);
}

// Build fragment `idx` of `blob` into `out` (header + chunk). Returns the byte
// length written (to use as the DATA payload), or 0 on bad args.
uint16_t sar_build_fragment(const uint8_t* blob, uint32_t len, uint16_t xfer_id,
                            uint32_t crc, uint16_t idx, uint8_t* out, uint16_t out_cap);

// True if `payload` begins with the SAR fragment magic ("SAR1").
bool sar_is_fragment(const uint8_t* payload, uint16_t len);

// --- missing-fragment request (NACK), for end-to-end reliability ---
// A receiver that has some but not all fragments asks the sender to resend the
// ones it's missing. Message layout (inside a DATA payload):
//   'S''A''R''N' (4) | u16 xfer_id | u16 n_missing | u16 missing[n_missing]
constexpr uint16_t SAR_NACK_HDR = 8;

bool     sar_is_nack(const uint8_t* payload, uint16_t len);
uint16_t sar_build_nack(uint16_t xfer_id, const uint16_t* missing, uint16_t n,
                        uint8_t* out, uint16_t out_cap);
// Parse a NACK; fills out[] with up to `cap` missing indices, sets *xfer_id,
// returns the count written.
uint16_t sar_parse_nack(const uint8_t* payload, uint16_t len, uint16_t* xfer_id,
                        uint16_t* out, uint16_t cap);

// --- transfer-complete ACK (DONE) ---
// The receiver confirms a verified (CRC-OK) reassembly so the sender can release
// its transfer slot immediately instead of sitting out the NACK window — that
// window dominates back-to-back transfer latency (~10 s per queued frame). A lost
// DONE costs nothing: the sender just falls back to the timed window.
// Message layout (inside a DATA payload): 'S''A''R''D' (4) | u16 xfer_id
constexpr uint16_t SAR_DONE_BYTES = 6;

bool     sar_is_done(const uint8_t* payload, uint16_t len);
uint16_t sar_build_done(uint16_t xfer_id, uint8_t* out, uint16_t out_cap);
// Returns the confirmed xfer_id, or 0xFFFF if `payload` is not a DONE.
uint16_t sar_parse_done(const uint8_t* payload, uint16_t len);

// Reassembles one transfer at a time into a fixed buffer.
class SarReassembler {
public:
    // Feed one received fragment payload. Starts a fresh transfer if xfer_id changes.
    // Returns false if it's not a valid fragment / overflows.
    bool add(const uint8_t* payload, uint16_t len);

    bool     active()      const { return active_; }
    bool     complete()    const;                 // all fragments present
    bool     verify()      const;                 // complete AND CRC matches
    uint32_t total_len()   const { return total_len_; }
    uint16_t got_count()   const { return got_count_; }
    uint16_t frag_count()  const { return frag_count_; }
    uint16_t xfer_id()     const { return xfer_id_; }
    const uint8_t* data()  const { return buf_; }

    // Fill `out` with the indices we're still missing (up to `cap`); returns count.
    uint16_t missing(uint16_t* out, uint16_t cap) const;

    void reset() { active_ = false; got_count_ = 0; }

private:
    bool     active_     = false;
    uint16_t xfer_id_    = 0;
    uint16_t frag_count_ = 0;
    uint32_t total_len_  = 0;
    uint32_t total_crc_  = 0;
    uint16_t got_count_  = 0;
    bool     got_[SAR_MAX_FRAGS] = {};
    uint8_t  buf_[SAR_MAX_FILE]  = {};
};

} // namespace mesh
