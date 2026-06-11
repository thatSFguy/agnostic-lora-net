#include "sar.h"

namespace mesh {

static const uint8_t MAGIC[4] = {'S', 'A', 'R', '1'};

uint32_t sar_crc32(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

// --- little-endian helpers ---
static inline void put_u16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t get_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool sar_is_fragment(const uint8_t* payload, uint16_t len) {
    return len >= SAR_HDR_BYTES && memcmp(payload, MAGIC, 4) == 0;
}

uint16_t sar_build_fragment(const uint8_t* blob, uint32_t len, uint16_t xfer_id,
                            uint32_t crc, uint16_t idx, uint8_t* out, uint16_t out_cap) {
    uint16_t count = sar_frag_count(len);
    if (idx >= count) return 0;

    uint32_t off = (uint32_t)idx * SAR_CHUNK;
    uint16_t chunk = (uint16_t)((len - off) < SAR_CHUNK ? (len - off) : SAR_CHUNK);
    if (out_cap < (uint16_t)(SAR_HDR_BYTES + chunk)) return 0;

    memcpy(out, MAGIC, 4);
    put_u16(out + 4, xfer_id);
    put_u16(out + 6, idx);
    put_u16(out + 8, count);
    put_u32(out + 10, len);
    put_u32(out + 14, crc);
    memcpy(out + SAR_HDR_BYTES, blob + off, chunk);
    return (uint16_t)(SAR_HDR_BYTES + chunk);
}

bool SarReassembler::add(const uint8_t* payload, uint16_t len) {
    if (!sar_is_fragment(payload, len)) return false;

    uint16_t xfer  = get_u16(payload + 4);
    uint16_t idx   = get_u16(payload + 6);
    uint16_t count = get_u16(payload + 8);
    uint32_t tlen  = get_u32(payload + 10);
    uint32_t tcrc  = get_u32(payload + 14);

    if (count == 0 || count > SAR_MAX_FRAGS || tlen > SAR_MAX_FILE || idx >= count) return false;

    // New transfer? (different id, or first fragment seen) — reset.
    if (!active_ || xfer != xfer_id_) {
        active_     = true;
        xfer_id_    = xfer;
        frag_count_ = count;
        total_len_  = tlen;
        total_crc_  = tcrc;
        got_count_  = 0;
        for (uint16_t i = 0; i < SAR_MAX_FRAGS; i++) got_[i] = false;
    }

    uint32_t off   = (uint32_t)idx * SAR_CHUNK;
    uint16_t chunk = (uint16_t)(len - SAR_HDR_BYTES);
    if (off + chunk > SAR_MAX_FILE) return false;

    memcpy(buf_ + off, payload + SAR_HDR_BYTES, chunk);
    if (!got_[idx]) { got_[idx] = true; got_count_++; }
    return true;
}

uint16_t SarReassembler::missing(uint16_t* out, uint16_t cap) const {
    uint16_t n = 0;
    if (!active_) return 0;
    for (uint16_t i = 0; i < frag_count_ && n < cap; i++) {
        if (!got_[i]) out[n++] = i;
    }
    return n;
}

bool SarReassembler::complete() const {
    return active_ && got_count_ == frag_count_;
}

// --- NACK message ---
static const uint8_t NMAGIC[4] = {'S', 'A', 'R', 'N'};

bool sar_is_nack(const uint8_t* payload, uint16_t len) {
    return len >= SAR_NACK_HDR && memcmp(payload, NMAGIC, 4) == 0;
}

uint16_t sar_build_nack(uint16_t xfer_id, const uint16_t* missing, uint16_t n,
                        uint8_t* out, uint16_t out_cap) {
    if (out_cap < SAR_NACK_HDR) return 0;
    uint16_t fit = (uint16_t)((out_cap - SAR_NACK_HDR) / 2);
    if (n > fit) n = fit;
    memcpy(out, NMAGIC, 4);
    put_u16(out + 4, xfer_id);
    put_u16(out + 6, n);
    for (uint16_t i = 0; i < n; i++) put_u16(out + SAR_NACK_HDR + i * 2, missing[i]);
    return (uint16_t)(SAR_NACK_HDR + n * 2);
}

uint16_t sar_parse_nack(const uint8_t* payload, uint16_t len, uint16_t* xfer_id,
                        uint16_t* out, uint16_t cap) {
    if (!sar_is_nack(payload, len)) return 0;
    if (xfer_id) *xfer_id = get_u16(payload + 4);
    uint16_t n = get_u16(payload + 6);
    if ((uint32_t)SAR_NACK_HDR + (uint32_t)n * 2 > len) n = (uint16_t)((len - SAR_NACK_HDR) / 2);
    if (n > cap) n = cap;
    for (uint16_t i = 0; i < n; i++) out[i] = get_u16(payload + SAR_NACK_HDR + i * 2);
    return n;
}

// --- DONE message ---
static const uint8_t DMAGIC[4] = {'S', 'A', 'R', 'D'};

bool sar_is_done(const uint8_t* payload, uint16_t len) {
    return len >= SAR_DONE_BYTES && memcmp(payload, DMAGIC, 4) == 0;
}

uint16_t sar_build_done(uint16_t xfer_id, uint8_t* out, uint16_t out_cap) {
    if (out_cap < SAR_DONE_BYTES) return 0;
    memcpy(out, DMAGIC, 4);
    put_u16(out + 4, xfer_id);
    return SAR_DONE_BYTES;
}

uint16_t sar_parse_done(const uint8_t* payload, uint16_t len) {
    if (!sar_is_done(payload, len)) return 0xFFFF;
    return get_u16(payload + 4);
}

bool SarReassembler::verify() const {
    return complete() && sar_crc32(buf_, total_len_) == total_crc_;
}

} // namespace mesh
