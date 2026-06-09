#include "announce_codec.h"

namespace mesh {

// --- little-endian byte helpers --------------------------------------------
static inline void put_u8(uint8_t* p, uint8_t v)  { p[0] = v; }
static inline void put_u16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint8_t  get_u8(const uint8_t* p)  { return p[0]; }
static inline uint16_t get_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// --- quantisation ----------------------------------------------------------
static inline uint8_t q_to_byte(float q) {
    if (q < 0.0f) q = 0.0f;
    if (q > 1.0f) q = 1.0f;
    return (uint8_t)(q * 255.0f + 0.5f);
}
static inline float byte_to_q(uint8_t b) { return (float)b / 255.0f; }

static inline uint16_t cost_to_u16(float cost) {
    if (cost < 0.0f) cost = 0.0f;
    float v = cost * 16.0f + 0.5f;
    if (v > 65535.0f) v = 65535.0f;   // saturate
    return (uint16_t)v;
}
static inline float u16_to_cost(uint16_t v) { return (float)v / 16.0f; }

// ---------------------------------------------------------------------------
uint16_t announce_serialize(const Announce& a, uint8_t* buf, uint16_t cap) {
    if (cap < ANNOUNCE_HDR_BYTES) return 0;

    uint16_t off = ANNOUNCE_HDR_BYTES;  // reserve the two count bytes
    uint8_t  n_reports = 0, n_routes = 0;

    for (uint8_t i = 0; i < a.n_reports; i++) {
        if (off + ANNOUNCE_REPORT_BYTES > cap) break;
        put_u32(buf + off, a.reports[i].id);  off += 4;
        put_u8 (buf + off, q_to_byte(a.reports[i].q)); off += 1;
        put_u8 (buf + off, a.reports[i].alias); off += 1;
        n_reports++;
    }
    for (uint8_t i = 0; i < a.n_routes; i++) {
        if (off + ANNOUNCE_ROUTE_BYTES > cap) break;
        put_u32(buf + off, a.routes[i].dst);      off += 4;
        put_u32(buf + off, a.routes[i].next_hop); off += 4;
        put_u16(buf + off, cost_to_u16(a.routes[i].cost)); off += 2;
        put_u8 (buf + off, a.routes[i].hops);     off += 1;
        n_routes++;
    }

    put_u8(buf + 0, n_reports);
    put_u8(buf + 1, n_routes);
    return off;
}

bool announce_deserialize(const uint8_t* buf, uint16_t len, Announce& out) {
    out = Announce{};
    if (len < ANNOUNCE_HDR_BYTES) return false;

    uint8_t n_reports = get_u8(buf + 0);
    uint8_t n_routes  = get_u8(buf + 1);

    // Reject counts that can't fit our fixed arrays (untrusted radio input).
    if (n_reports > MAX_NEIGHBORS || n_routes > MAX_ROUTES) return false;

    // Reject buffers too short for the declared payload.
    uint32_t need = (uint32_t)ANNOUNCE_HDR_BYTES
                  + (uint32_t)n_reports * ANNOUNCE_REPORT_BYTES
                  + (uint32_t)n_routes  * ANNOUNCE_ROUTE_BYTES;
    if (len < need) return false;

    uint16_t off = ANNOUNCE_HDR_BYTES;
    for (uint8_t i = 0; i < n_reports; i++) {
        out.reports[i].id    = get_u32(buf + off); off += 4;
        out.reports[i].q     = byte_to_q(get_u8(buf + off)); off += 1;
        out.reports[i].alias = get_u8(buf + off); off += 1;
    }
    out.n_reports = n_reports;

    for (uint8_t i = 0; i < n_routes; i++) {
        out.routes[i].dst      = get_u32(buf + off); off += 4;
        out.routes[i].next_hop = get_u32(buf + off); off += 4;
        out.routes[i].cost     = u16_to_cost(get_u16(buf + off)); off += 2;
        out.routes[i].hops     = get_u8(buf + off); off += 1;
    }
    out.n_routes = n_routes;
    return true;
}

} // namespace mesh
