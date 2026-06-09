// locator_dir.h — distributed identity→locator directory (Phase 1: portable core).
//
// Maps an opaque endpoint **identity** (e.g. an RNS destination hash) to the mesh
// **node it's currently served by** (its locator), so a sender can stamp the right
// destination and let the mesh route directly — no hub, no relaying through a central
// node. See docs/distributed-lookup-plan.md and docs/identity-vs-locator.md.
//
// Design invariants (app-agnostic):
//   * the key is an OPAQUE, length-prefixed id (≤16 B) — this code never interprets it;
//   * this sits BESIDE locator routing — it only answers "which node", it does not make
//     the mesh forward on identities;
//   * RAM-only (Agent.md Req 4) — bindings rebuild from re-registration, never persisted.
//
// Three wire messages (carried as opaque payload in a PKT_LOC packet):
//   REGISTER  an endpoint asserts "id is served at me" (locator = the packet's src node)
//   QUERY     "who serves id?"  (asker = the packet's src node)
//   REPLY     "id is at <locator>"  (carries the full binding so receivers cache it)
//
// Phase-1 scope: the cache + codec + a small resolver. Unsigned (trusted bench);
// `epoch`+`seq` ordering is in place so signed registration drops in later (review G/F).
#pragma once

#include <stdint.h>
#include <string.h>
#include "mesh_types.h"

namespace mesh {

constexpr uint8_t  LOC_ID_MAX      = 16;   // opaque id cap (fits an RNS hash)
constexpr uint16_t LOC_DIR_CAP     = 32;   // bindings cached per node (LRU + TTL)
constexpr uint16_t LOC_MAX_PENDING = 8;    // concurrent in-flight resolutions

enum LocKind : uint8_t { LOC_REGISTER = 1, LOC_QUERY = 2, LOC_REPLY = 3 };

// A parsed LOC message (only the fields relevant to its kind are populated).
struct LocMsg {
    uint8_t   kind     = 0;
    uint16_t  qid      = 0;   // QUERY / REPLY
    uint16_t  epoch    = 0;   // REGISTER / REPLY — per-boot nonce of the id owner
    uint16_t  seq      = 0;   // REGISTER / REPLY — monotonic within an epoch
    uint16_t  ttl_s    = 0;   // REGISTER / REPLY — binding lifetime, seconds
    node_id_t loc      = 0;   // REPLY — serving node (locator)
    uint8_t   id_len   = 0;
    uint8_t   id[LOC_ID_MAX] = {};
};

// --- wire codec --- (all multi-byte fields little-endian). Build fns return the byte
// length written, or 0 if `cap` is too small / id_len out of range.
uint16_t loc_build_register(uint16_t epoch, uint16_t seq, uint16_t ttl_s,
                            const uint8_t* id, uint8_t id_len, uint8_t* out, uint16_t cap);
uint16_t loc_build_query(uint16_t qid, const uint8_t* id, uint8_t id_len,
                         uint8_t* out, uint16_t cap);
uint16_t loc_build_reply(uint16_t qid, uint16_t epoch, uint16_t seq, uint16_t ttl_s,
                         node_id_t loc, const uint8_t* id, uint8_t id_len,
                         uint8_t* out, uint16_t cap);
// Parse any LOC message into `out`. Returns true only for a well-formed message.
bool loc_parse(const uint8_t* msg, uint16_t len, LocMsg* out);

// Strictly-newer comparison for the 16-bit seq, wraparound-aware.
inline bool loc_seq_newer(uint16_t a, uint16_t b) { return (int16_t)(a - b) > 0; }

// A full cached binding — what a node needs to answer a QUERY with a REPLY (the REPLY
// must carry the binding's epoch/seq so the asker can order it, plus the remaining TTL).
struct LocBinding { node_id_t loc; uint16_t epoch; uint16_t seq; uint16_t ttl_s; };

// The per-node binding cache: id → serving locator, bounded with TTL + LRU eviction.
class LocatorDir {
public:
    // Insert or refresh a binding. Acceptance order (review note F — answers the
    // RAM-only reboot trap): accept if the id is unknown, OR the cached binding has
    // expired, OR (same epoch AND strictly-newer seq), OR a different epoch (treated as
    // a newer boot of the endpoint). Returns true if stored, false if rejected as stale.
    // (The "different epoch supersedes" rule is a replay vector that signed registration
    // closes later; fine on the trusted bench.)
    bool upsert(const uint8_t* id, uint8_t id_len, node_id_t loc,
                uint16_t epoch, uint16_t seq, uint16_t ttl_s, uint32_t now_ms);

    // Resolve id → serving locator if cached and unexpired. Refreshes its LRU position.
    bool lookup(const uint8_t* id, uint8_t id_len, node_id_t* out_loc, uint32_t now_ms);

    // Like lookup(), but returns the full binding (epoch/seq + remaining TTL seconds, ≥1)
    // so a node can answer a QUERY with a well-ordered REPLY.
    bool lookup_full(const uint8_t* id, uint8_t id_len, LocBinding* out, uint32_t now_ms);

    // Drop a binding — e.g. on a NO_CONSUMER delivery failure (review note E).
    bool remove(const uint8_t* id, uint8_t id_len);

    uint16_t size(uint32_t now_ms) const;          // count of live (unexpired) bindings
    static constexpr uint16_t capacity() { return LOC_DIR_CAP; }

    // One live binding, for diagnostics (`dirdump`).
    struct View { uint8_t id[LOC_ID_MAX]; uint8_t id_len; node_id_t loc; uint16_t ttl_s; };
    // Copy up to `cap` live bindings into `out`; returns how many were written.
    uint16_t snapshot(View* out, uint16_t cap, uint32_t now_ms) const;

private:
    struct Entry {
        uint8_t   id[LOC_ID_MAX];
        uint8_t   id_len = 0;
        bool      used   = false;
        node_id_t loc    = 0;
        uint16_t  epoch  = 0;
        uint16_t  seq    = 0;
        uint32_t  expiry_ms = 0;
        uint32_t  touched   = 0;   // LRU stamp
    };
    Entry    e_[LOC_DIR_CAP];
    uint32_t clock_ = 0;           // LRU tick

    static bool expired(const Entry& e, uint32_t now) { return (int32_t)(now - e.expiry_ms) >= 0; }
    int  find(const uint8_t* id, uint8_t id_len) const;
    int  alloc_slot(uint32_t now_ms);   // free slot, else an expired one, else LRU-evict
};

// Tracks in-flight resolutions so a QUERY isn't re-sent while one is outstanding, and
// a slow/lost lookup times out instead of hanging. (Retry/backoff is firmware-level.)
class LocatorResolver {
public:
    // Start resolving `id`; returns a query id for the QUERY the caller will send. If a
    // resolution for this id is already pending, returns its existing qid (no new QUERY).
    uint16_t begin(const uint8_t* id, uint8_t id_len, uint32_t now_ms, uint16_t timeout_ms);
    // A REPLY arrived for `qid`: clears the matching pending. Returns true if it matched.
    bool on_reply(uint16_t qid);
    // Is there a still-pending (un-timed-out) resolution for `id`?
    bool pending(const uint8_t* id, uint8_t id_len, uint32_t now_ms) const;
    // Expire timed-out resolutions; call periodically. Returns how many expired.
    uint16_t tick(uint32_t now_ms);

private:
    struct P {
        bool     used = false;
        uint8_t  id[LOC_ID_MAX];
        uint8_t  id_len = 0;
        uint16_t qid = 0;
        uint32_t deadline_ms = 0;
    };
    P        p_[LOC_MAX_PENDING];
    uint16_t next_qid_ = 1;        // 0 reserved as "none"
};

} // namespace mesh
