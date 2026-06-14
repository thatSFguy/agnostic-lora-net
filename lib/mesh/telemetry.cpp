#include "telemetry.h"

namespace mesh {

static void put_u16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static uint16_t get_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t* p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }

uint16_t telem_build_batt(uint16_t mv, uint8_t pct_plus1, uint8_t* out, uint16_t cap) {
    if (cap < 4) return 0;
    out[0] = TELEM_BATT; put_u16(out + 1, mv); out[3] = pct_plus1;
    return 4;
}

uint16_t telem_build_query(node_id_t target, uint8_t* out, uint16_t cap) {
    if (cap < 5 || target == 0) return 0;
    out[0] = TELEM_QUERY; put_u32(out + 1, target);
    return 5;
}

uint16_t telem_build_reply(uint16_t mv, uint8_t pct_plus1, uint16_t uptime_min,
                           int8_t power_dbm, uint8_t sf, uint8_t flags,
                           const char* fw, const TelemNbr* nbrs, uint8_t n_nbrs,
                           uint8_t* out, uint16_t cap) {
    uint8_t fw_len = 0;
    while (fw && fw[fw_len] && fw_len < TELEM_FW_MAX) fw_len++;
    if (n_nbrs > TELEM_NBR_MAX) n_nbrs = TELEM_NBR_MAX;
    uint16_t need = (uint16_t)(10 + fw_len + 1 + n_nbrs * 9);
    if (cap < need) return 0;
    uint8_t* p = out;
    *p++ = TELEM_REPLY;
    put_u16(p, mv); p += 2;
    *p++ = pct_plus1;
    put_u16(p, uptime_min); p += 2;
    *p++ = (uint8_t)power_dbm;
    *p++ = sf;
    *p++ = flags;
    *p++ = fw_len;
    memcpy(p, fw, fw_len); p += fw_len;
    *p++ = n_nbrs;
    for (uint8_t i = 0; i < n_nbrs; i++) {
        put_u32(p, nbrs[i].id); p += 4;
        *p++ = nbrs[i].q_rx;
        *p++ = nbrs[i].q_tx;
        *p++ = (uint8_t)nbrs[i].snr;
        put_u16(p, (uint16_t)nbrs[i].rssi); p += 2;
    }
    return need;
}

bool telem_parse(const uint8_t* msg, uint16_t len, TelemMsg* out) {
    if (!msg || !out || len < 1) return false;
    *out = TelemMsg{};
    out->kind = msg[0];
    switch (out->kind) {
        case TELEM_BATT:
            if (len < 4) return false;
            out->mv = get_u16(msg + 1); out->pct_plus1 = msg[3];
            return true;
        case TELEM_QUERY:
            if (len < 5) return false;
            out->target = (node_id_t)get_u32(msg + 1);
            return out->target != 0;
        case TELEM_REPLY: {
            if (len < 11) return false;
            const uint8_t* p = msg + 1;
            out->mv = get_u16(p); p += 2;
            out->pct_plus1 = *p++;
            out->uptime_min = get_u16(p); p += 2;
            out->power_dbm = (int8_t)*p++;
            out->sf = *p++;
            out->flags = *p++;
            uint8_t fw_len = *p++;
            if (fw_len > TELEM_FW_MAX || (uint16_t)(p - msg) + fw_len + 1 > len) return false;
            memcpy(out->fw, p, fw_len); out->fw[fw_len] = 0; p += fw_len;
            uint8_t n = *p++;
            if (n > TELEM_NBR_MAX || (uint16_t)(p - msg) + (uint16_t)n * 9 > len) return false;
            out->n_nbrs = n;
            for (uint8_t i = 0; i < n; i++) {
                out->nbrs[i].id   = (node_id_t)get_u32(p); p += 4;
                out->nbrs[i].q_rx = *p++;
                out->nbrs[i].q_tx = *p++;
                out->nbrs[i].snr  = (int8_t)*p++;
                out->nbrs[i].rssi = (int16_t)get_u16(p); p += 2;
            }
            return true;
        }
        default:
            return false;
    }
}

void TelemCache::upsert(node_id_t origin, uint16_t mv, uint8_t pct_plus1, uint32_t now_ms) {
    Entry* slot = nullptr;
    for (auto& e : e_) if (e.used && e.origin == origin) { slot = &e; break; }
    if (!slot) for (auto& e : e_) if (!e.used) { slot = &e; break; }
    if (!slot) {                                   // full: evict the stalest
        slot = &e_[0];
        for (auto& e : e_) if ((int32_t)(e.ms - slot->ms) < 0) slot = &e;
    }
    slot->used = true; slot->origin = origin; slot->mv = mv;
    slot->pct_plus1 = pct_plus1; slot->ms = now_ms;
}

uint16_t TelemCache::snapshot(View* out, uint16_t cap, uint32_t now_ms) const {
    uint16_t n = 0;
    for (auto& e : e_) {
        if (!e.used || n >= cap) continue;
        out[n].origin = e.origin; out[n].mv = e.mv; out[n].pct_plus1 = e.pct_plus1;
        out[n].age_ms = now_ms - e.ms;
        n++;
    }
    return n;
}

} // namespace mesh
