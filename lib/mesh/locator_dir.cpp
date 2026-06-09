// locator_dir.cpp — see locator_dir.h.
#include "locator_dir.h"

namespace mesh {

// --- little-endian cursor helpers ------------------------------------------------
static inline void put8 (uint8_t*& p, uint8_t v)  { *p++ = v; }
static inline void put16(uint8_t*& p, uint16_t v) { *p++ = v & 0xFF; *p++ = (v >> 8) & 0xFF; }
static inline void put32(uint8_t*& p, uint32_t v) { for (int i = 0; i < 4; i++) { *p++ = v & 0xFF; v >>= 8; } }
static inline uint16_t get16(const uint8_t*& p) { uint16_t v = (uint16_t)(p[0] | (p[1] << 8)); p += 2; return v; }
static inline uint32_t get32(const uint8_t*& p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i); p += 4; return v; }

// --- codec -----------------------------------------------------------------------
uint16_t loc_build_register(uint16_t epoch, uint16_t seq, uint16_t ttl_s,
                            const uint8_t* id, uint8_t id_len, uint8_t* out, uint16_t cap) {
    if (id_len == 0 || id_len > LOC_ID_MAX) return 0;
    uint16_t need = (uint16_t)(1 + 2 + 2 + 2 + 1 + id_len);
    if (cap < need) return 0;
    uint8_t* p = out;
    put8(p, LOC_REGISTER); put16(p, epoch); put16(p, seq); put16(p, ttl_s);
    put8(p, id_len); memcpy(p, id, id_len); p += id_len;
    return (uint16_t)(p - out);
}

uint16_t loc_build_query(uint16_t qid, const uint8_t* id, uint8_t id_len,
                         uint8_t* out, uint16_t cap) {
    if (id_len == 0 || id_len > LOC_ID_MAX) return 0;
    uint16_t need = (uint16_t)(1 + 2 + 1 + id_len);
    if (cap < need) return 0;
    uint8_t* p = out;
    put8(p, LOC_QUERY); put16(p, qid);
    put8(p, id_len); memcpy(p, id, id_len); p += id_len;
    return (uint16_t)(p - out);
}

uint16_t loc_build_reply(uint16_t qid, uint16_t epoch, uint16_t seq, uint16_t ttl_s,
                         node_id_t loc, const uint8_t* id, uint8_t id_len,
                         uint8_t* out, uint16_t cap) {
    if (id_len == 0 || id_len > LOC_ID_MAX) return 0;
    uint16_t need = (uint16_t)(1 + 2 + 2 + 2 + 2 + 4 + 1 + id_len);
    if (cap < need) return 0;
    uint8_t* p = out;
    put8(p, LOC_REPLY); put16(p, qid); put16(p, epoch); put16(p, seq); put16(p, ttl_s);
    put32(p, (uint32_t)loc); put8(p, id_len); memcpy(p, id, id_len); p += id_len;
    return (uint16_t)(p - out);
}

bool loc_parse(const uint8_t* msg, uint16_t len, LocMsg* out) {
    if (!msg || len < 2 || !out) return false;
    *out = LocMsg{};
    const uint8_t* p = msg;
    out->kind = *p++;
    // helper to read the trailing [id_len][id] and confirm it consumes exactly the rest
    auto read_id = [&](void) -> bool {
        if ((uint16_t)(p - msg) + 1 > len) return false;
        uint8_t il = *p++;
        if (il == 0 || il > LOC_ID_MAX) return false;
        if ((uint16_t)((p - msg) + il) != len) return false;   // no trailing junk / short
        out->id_len = il; memcpy(out->id, p, il); p += il;
        return true;
    };
    switch (out->kind) {
        case LOC_REGISTER:
            if (len < 1 + 2 + 2 + 2 + 1 + 1) return false;
            out->epoch = get16(p); out->seq = get16(p); out->ttl_s = get16(p);
            return read_id();
        case LOC_QUERY:
            if (len < 1 + 2 + 1 + 1) return false;
            out->qid = get16(p);
            return read_id();
        case LOC_REPLY:
            if (len < 1 + 2 + 2 + 2 + 2 + 4 + 1 + 1) return false;
            out->qid = get16(p); out->epoch = get16(p); out->seq = get16(p); out->ttl_s = get16(p);
            out->loc = (node_id_t)get32(p);
            return read_id();
        default:
            return false;
    }
}

// --- LocatorDir ------------------------------------------------------------------
int LocatorDir::find(const uint8_t* id, uint8_t id_len) const {
    for (int i = 0; i < LOC_DIR_CAP; i++)
        if (e_[i].used && e_[i].id_len == id_len && memcmp(e_[i].id, id, id_len) == 0)
            return i;
    return -1;
}

int LocatorDir::alloc_slot(uint32_t now_ms) {
    int free_slot = -1, expired_slot = -1, lru = -1;
    uint32_t lru_touch = 0;
    for (int i = 0; i < LOC_DIR_CAP; i++) {
        if (!e_[i].used) { free_slot = i; break; }
        if (expired(e_[i], now_ms)) { expired_slot = i; }
        if (lru < 0 || e_[i].touched < lru_touch) { lru = i; lru_touch = e_[i].touched; }
    }
    if (free_slot >= 0)    return free_slot;
    if (expired_slot >= 0) return expired_slot;
    return lru;   // evict least-recently-used
}

bool LocatorDir::upsert(const uint8_t* id, uint8_t id_len, node_id_t loc,
                        uint16_t epoch, uint16_t seq, uint16_t ttl_s, uint32_t now_ms) {
    if (id_len == 0 || id_len > LOC_ID_MAX) return false;
    int idx = find(id, id_len);
    if (idx >= 0) {
        Entry& e = e_[idx];
        bool accept = expired(e, now_ms)
                   || (epoch == e.epoch && loc_seq_newer(seq, e.seq))
                   || (epoch != e.epoch);
        if (!accept) return false;
        e.loc = loc; e.epoch = epoch; e.seq = seq;
        e.expiry_ms = now_ms + (uint32_t)ttl_s * 1000u;
        e.touched = ++clock_;
        return true;
    }
    int slot = alloc_slot(now_ms);
    Entry& e = e_[slot];
    e.used = true; e.id_len = id_len; memcpy(e.id, id, id_len);
    e.loc = loc; e.epoch = epoch; e.seq = seq;
    e.expiry_ms = now_ms + (uint32_t)ttl_s * 1000u;
    e.touched = ++clock_;
    return true;
}

bool LocatorDir::lookup(const uint8_t* id, uint8_t id_len, node_id_t* out_loc, uint32_t now_ms) {
    int idx = find(id, id_len);
    if (idx < 0) return false;
    Entry& e = e_[idx];
    if (expired(e, now_ms)) { e.used = false; return false; }   // lazy expiry
    e.touched = ++clock_;
    if (out_loc) *out_loc = e.loc;
    return true;
}

bool LocatorDir::lookup_full(const uint8_t* id, uint8_t id_len, LocBinding* out, uint32_t now_ms) {
    int idx = find(id, id_len);
    if (idx < 0) return false;
    Entry& e = e_[idx];
    int32_t rem_ms = (int32_t)(e.expiry_ms - now_ms);
    if (rem_ms <= 0) { e.used = false; return false; }
    e.touched = ++clock_;
    if (out) {
        out->loc = e.loc; out->epoch = e.epoch; out->seq = e.seq;
        uint32_t s = (uint32_t)rem_ms / 1000u;
        out->ttl_s = (uint16_t)(s == 0 ? 1 : (s > 0xFFFF ? 0xFFFF : s));
    }
    return true;
}

bool LocatorDir::remove(const uint8_t* id, uint8_t id_len) {
    int idx = find(id, id_len);
    if (idx < 0) return false;
    e_[idx].used = false;
    return true;
}

uint16_t LocatorDir::size(uint32_t now_ms) const {
    uint16_t n = 0;
    for (int i = 0; i < LOC_DIR_CAP; i++)
        if (e_[i].used && !expired(e_[i], now_ms)) n++;
    return n;
}

uint16_t LocatorDir::snapshot(View* out, uint16_t cap, uint32_t now_ms) const {
    uint16_t n = 0;
    for (int i = 0; i < LOC_DIR_CAP && n < cap; i++) {
        const Entry& e = e_[i];
        if (!e.used || expired(e, now_ms)) continue;
        memcpy(out[n].id, e.id, e.id_len);
        out[n].id_len = e.id_len;
        out[n].loc    = e.loc;
        int32_t rem = (int32_t)(e.expiry_ms - now_ms);
        out[n].ttl_s = (uint16_t)(rem <= 0 ? 0 : (uint32_t)rem / 1000u);
        n++;
    }
    return n;
}

// --- LocatorResolver -------------------------------------------------------------
uint16_t LocatorResolver::begin(const uint8_t* id, uint8_t id_len, uint32_t now_ms, uint16_t timeout_ms) {
    if (id_len == 0 || id_len > LOC_ID_MAX) return 0;
    // Already pending (and not timed out)? Reuse its qid — don't fire a duplicate QUERY.
    for (int i = 0; i < LOC_MAX_PENDING; i++)
        if (p_[i].used && (int32_t)(now_ms - p_[i].deadline_ms) < 0
            && p_[i].id_len == id_len && memcmp(p_[i].id, id, id_len) == 0)
            return p_[i].qid;
    // Find a slot: free, else a timed-out one.
    int slot = -1;
    for (int i = 0; i < LOC_MAX_PENDING; i++) {
        if (!p_[i].used) { slot = i; break; }
        if ((int32_t)(now_ms - p_[i].deadline_ms) >= 0) slot = i;   // reuse expired
    }
    if (slot < 0) return 0;   // table full of live resolutions
    uint16_t qid = next_qid_++;
    if (next_qid_ == 0) next_qid_ = 1;   // keep 0 reserved
    P& p = p_[slot];
    p.used = true; p.qid = qid; p.id_len = id_len; memcpy(p.id, id, id_len);
    p.deadline_ms = now_ms + timeout_ms;
    return qid;
}

bool LocatorResolver::on_reply(uint16_t qid) {
    for (int i = 0; i < LOC_MAX_PENDING; i++)
        if (p_[i].used && p_[i].qid == qid) { p_[i].used = false; return true; }
    return false;
}

bool LocatorResolver::pending(const uint8_t* id, uint8_t id_len, uint32_t now_ms) const {
    for (int i = 0; i < LOC_MAX_PENDING; i++)
        if (p_[i].used && (int32_t)(now_ms - p_[i].deadline_ms) < 0
            && p_[i].id_len == id_len && memcmp(p_[i].id, id, id_len) == 0)
            return true;
    return false;
}

uint16_t LocatorResolver::tick(uint32_t now_ms) {
    uint16_t n = 0;
    for (int i = 0; i < LOC_MAX_PENDING; i++)
        if (p_[i].used && (int32_t)(now_ms - p_[i].deadline_ms) >= 0) { p_[i].used = false; n++; }
    return n;
}

} // namespace mesh
