// mesh_types.h — minimal shared types for the routing core.
//
// The routing library is deliberately self-contained: it does NOT depend on the
// firmware's wire-format header (packet.h), so it builds standalone for host unit
// tests and drops into any transport. packet.h includes THIS header for node_id_t
// (single source of truth).
//
// Identity model:
//   * node_id_t is a 16-byte self-certifying id = blake2b(node pubkey)[0:16].
//   * It is stored ONCE per distinct node in a NodeTable (node_table.h); the
//     routing tables reference it by a compact node_ref to keep RAM flat and to
//     move signature verification to once-per-id. Full ids live only in the
//     directory and at the wire-codec boundary.
#pragma once

#include <stdint.h>
#include <string.h>

// 16-byte node id (self-certifying locator). A bare POD so it is safe inside a
// packed wire header (no padding) and trivially memcpy'd to/from the radio buffer.
// No endianness: the bytes are the canonical blake2b output order.
struct NodeId {
    uint8_t b[16];
    bool operator==(const NodeId& o) const { return memcmp(b, o.b, 16) == 0; }
    bool operator!=(const NodeId& o) const { return memcmp(b, o.b, 16) != 0; }
    bool is_zero() const {
        static const uint8_t z[16] = {0};
        return memcmp(b, z, 16) == 0;
    }
};
typedef NodeId node_id_t;

// All-0xFF broadcast/flood destination (was 0xFFFFFFFF in the 4-byte era).
constexpr NodeId NODE_ID_BROADCAST = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

// 1-byte link-local neighbour alias. Identical typedef to packet.h's,
// so both headers can be included together.
typedef uint8_t link_addr_t;

// --- NodeId helpers (pure; the crypto-dependent nid_from_pubkey lives in
// node_table.cpp so this header stays dependency-free). -----------------------

// Copy the 16 raw id bytes to/from a wire buffer (no endianness conversion).
inline void   nid_write(uint8_t* p, const NodeId& id) { memcpy(p, id.b, 16); }
inline NodeId nid_read(const uint8_t* p) { NodeId id; memcpy(id.b, p, 16); return id; }

// Format as 32 UPPERCASE hex chars + NUL. Uppercase to match loc_id_hex (identity ids),
// the controller's id normalization, and the project convention "uppercase everywhere"
// (docs/INTEGRATING-AGNOSTIC-LORA-NET.md). V1 used %08lX (also uppercase); v2's earlier
// lowercase was an oversight that produced mixed-case `loc <ID> <node>` lines.
inline void nid_hex(const NodeId& id, char out[33]) {
    static const char* hx = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) { out[2*i] = hx[id.b[i] >> 4]; out[2*i+1] = hx[id.b[i] & 0xF]; }
    out[32] = '\0';
}

// Fold a NodeId to a 32-bit value (FNV-1a). For id-derived scalars that don't
// need the full width: the link-alias seed, the seen-cache dedup key, etc.
inline uint32_t nid_fold(const NodeId& id) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; i++) { h ^= id.b[i]; h *= 16777619u; }
    return h;
}

// TEST / back-compat helper: build a NodeId from a 32-bit value (LE in the first
// 4 bytes, rest zero). NOT an implicit constructor — keeps scalar misuse out of
// production while letting tests keep their integer id literals readable.
inline NodeId nid_from_u32(uint32_t v) {
    NodeId id{};
    id.b[0] = (uint8_t)v; id.b[1] = (uint8_t)(v >> 8);
    id.b[2] = (uint8_t)(v >> 16); id.b[3] = (uint8_t)(v >> 24);
    return id;
}

// One hex digit -> 0..15, or -1 if not a hex char. Case-insensitive.
inline int nid_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse up to 32 hex chars (case-insensitive) into a NodeId in NATURAL order —
// byte[0] is the first hex pair, the exact inverse of nid_hex (NOT little-endian:
// the v2 id is a raw 16-byte array, so display order == wire order). Parsing stops
// at the first non-hex char or after 16 bytes; unfilled bytes stay zero. Returns the
// number of bytes filled. Replaces the old 32-bit strtoul parse, which OVERFLOWED a
// 32-hex id to 0xFFFFFFFF and addressed a non-existent node.
inline uint8_t nid_from_hex(const char* s, NodeId& out) {
    for (int i = 0; i < 16; i++) out.b[i] = 0;
    uint8_t nb = 0;
    for (int i = 0; i < 16; i++) {
        int hi = nid_hexval(s[2*i]);   if (hi < 0) break;
        int lo = nid_hexval(s[2*i+1]); if (lo < 0) break;
        out.b[i] = (uint8_t)((hi << 4) | lo);
        nb++;
    }
    return nb;
}

// --- node_ref: a compact handle into the NodeTable -------------------------
// 2 bytes: a slot index plus a generation tag. The gen makes a stale ref (slot
// evicted and reused) DETECTABLE — NodeTable::resolve() returns false rather than
// silently aliasing to a different node. Use NODE_REF_NONE for "unset".
struct node_ref {
    uint8_t idx;
    uint8_t gen;
    bool operator==(const node_ref& o) const { return idx == o.idx && gen == o.gen; }
    bool operator!=(const node_ref& o) const { return idx != o.idx || gen != o.gen; }
};
constexpr node_ref NODE_REF_NONE = {0xFF, 0};
inline bool ref_is_none(node_ref r) { return r.idx == NODE_REF_NONE.idx; }
