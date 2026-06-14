# Implementing the 16-byte node id via normalization (NodeTable + `node_ref`)

Status (2026-06): **planned, not started.** The riskiest sub-task of
[`self-certifying-identity-plan.md`](self-certifying-identity-plan.md): moving node ids from a
4-byte scalar to a 16-byte self-certifying `hash(pubkey)`.

**The approach is normalization, not naïve widening.** Storing a 16-byte id redundantly across
the ~6 tables that hold ids today would cost +3–5 KB of RAM on parts where bss is already
~230 KB / 256 KB, and would 4× the cost of every hot-path `== id` comparison. Instead: intern
each distinct id **once** in a central `NodeTable`, and have every other table reference it by a
compact `node_ref` (1–2 bytes). Full `node_id_t` survives only in the directory and at the wire
codec boundary. This absorbs nearly the entire widening RAM cost and — by verifying each id once
at intern time — eliminates the per-frame signature-verify blocking.

It is a hard protocol break (the wire header width changes) touching **194 id sites across 22
files**, so it lands as one focused, compile-driven branch. Do this before
[`node-keygen-signed-announce-impl.md`](node-keygen-signed-announce-impl.md) and
[`controller-verify-acl-impl.md`](controller-verify-acl-impl.md).

---

## 0. Why normalize (the measured motivation)

Real baseline from the current builds: flash 237 KB / 1 MB (ample); but `data+bss` ≈ 230 KB of
the nRF52840's 256 KB. The bulk is the BLE stack + `sar_buf`/`sar_rx` (~16 KB of image buffers)
— **not** mesh tables — so those won't shrink here; the goal is to stop the identity feature from
*adding* pressure.

The same id is stored redundantly today across `Neighbor` (×32), `Route.dst` (×64),
`SeenEntry.src` (×64), `locdir.loc` (×32), telemetry `origin` (×16), and `ann_cache[8]`
announces. 3–6 live copies of each active node's id. Widening all of them to 16 B is the +3–5 KB
hit. Normalizing to one directory + refs:

| | Today (4-B, duplicated) | Widened, not normalized | Widened + normalized |
|---|---|---|---|
| id storage across the tables | ~1.1 KB | ~3.3 KB | refs ~0.4 KB + NodeTable(96×22 B) ~2.1 KB ≈ **2.5 KB** |

Net: 4→16-byte ids become roughly **RAM-neutral**, and per-packet comparison cost stays flat
(ref compare) instead of becoming a 16-byte `memcmp`.

---

## 1. The `NodeTable` + `node_ref`

```c
// mesh_types.h — the directory and the handle (portable core; deps: stdint, string).
struct NodeId { uint8_t b[16];                                   // full id = blake2b(pubkey)[0:16]
    bool operator==(const NodeId& o) const { return memcmp(b,o.b,16)==0; }
    bool operator!=(const NodeId& o) const { return !(*this==o); }
    bool is_zero()  const { static const uint8_t z[16]={0}; return memcmp(b,z,16)==0; } };
typedef NodeId node_id_t;                                        // name unchanged; full id type

struct node_ref { uint8_t idx; uint8_t gen; };                   // 2-byte handle into NodeTable
constexpr node_ref NODE_REF_NONE = {0xFF, 0};
```

```c
// node_table.h — interns ids; the ONLY place a full 16-byte id is stored in RAM.
class NodeTable {
public:
    node_ref intern(const NodeId& id);            // find-or-insert; updates LRU; returns handle
    bool     resolve(node_ref, NodeId& out) const;// false if stale (gen mismatch) / evicted
    bool     verified(node_ref) const;            // identity self-sig checked once
    void     mark_verified(node_ref);             // set after the one-time check
    void     pin(node_ref, bool);                 // protect live neighbours / active dsts from eviction
private:
    struct Slot { NodeId id; uint8_t flags; uint8_t gen; uint32_t last_ms; bool used; };
    Slot s_[NODE_TABLE_CAP];                       // CAP ~96; ~22 B/slot ≈ 2.1 KB
};
```

**`node_ref` width.** 2 bytes (`idx`+`gen`) is the safe default: the `gen` byte makes a stale
ref (slot evicted and reused) *detectable* — `resolve()` returns false and the caller treats it
as unknown, rather than silently aliasing to a different node. Drop to a 1-byte ref only if you
guarantee referenced slots are never evicted (see §3).

**Single definition.** Define `NodeId`/`node_ref`/NodeTable in the portable core
(`mesh_types.h` + `node_table.h`) and have `include/packet.h` `#include "mesh_types.h"` instead
of re-`typedef`ing `node_id_t` (today it's duplicated at `mesh_types.h:12` and `packet.h:24` — a
struct duplicated across two headers is an ODR hazard). `mesh_types.h` is dependency-free, so no
cycle, and the routing lib keeps building standalone for host tests.

---

## 2. The id=hash(pubkey) invariant — no stored pubkeys, verify once

Because the id **is** the hash of the pubkey, the id is already a commitment to the key:

- **Verify once, at `intern`.** When an id is first learned from a *signed* announce, check
  `nid_from_pubkey(pub)==id` and the Ed25519 signature, then `mark_verified` and **discard the
  pubkey**. The NodeTable never stores a 32-byte key — just the `VERIFIED` flag + the id.
- **A re-key is a new id** (different hash → different slot), so there is nothing to "update";
  the old binding simply ages out.
- **Steady-state verify cost → ~0.** Subsequent frames resolve a ref and read the flag (O(1), no
  crypto). Verification frequency drops from O(frames received) to O(distinct new nodes): a
  20-node mesh verifies ~20 times during convergence, then essentially never. This is what
  removes the ~20 ms-per-frame blocking call from the hot path.

Safe because routing data was never authenticated (best-effort DV); the signature establishes
*membership + identity*, which a one-time binding check satisfies. An unsigned announce claiming
a known id stays `unverified` (never ACL-eligible at the controller); a forged signature can't be
produced without the key.

---

## 3. Migration by table — what holds a `node_ref` vs a full id

| Table (file) | Field today | Becomes | Notes |
|---|---|---|---|
| `Neighbor` (`neighbor_table.h:30`) | `node_id_t id` | `node_ref` | already carries `my_alias`/`their_alias` (wire); `pin()` while it's a live neighbour |
| `Route` (`routing_table.h:26-27`) | `dst`, `next_hop` | `dst`→`node_ref`; `next_hop`→1-byte `link_addr_t` alias | next_hop is a neighbour → link-local alias, not a ref |
| `SeenEntry` (`forwarder.h:26`) | `node_id_t src` | **short hash** (e.g. `uint16_t`/`uint32_t` of src) + `pkt_id` | dedup needs *equality*, not identity — keep it OUT of NodeTable (see below) |
| `AnnCache` (`main.cpp:327`, ×8) | `src` + embedded `Announce` ids | `node_ref`s | the 3.3 KB consumer; refs shrink it most |
| `locdir` (`locator_dir.h:42`) | `node_id_t loc` | `node_ref` | the opaque app-identity `id[16]` key is unrelated, leave it |
| `TelemCache` (`telemetry.h`) | `origin` | `node_ref` | |
| `Router self_`, `Forwarder me_` | `node_id_t` | full id (it's *this* node) | the one place a long-lived full id is fine |

**The seen-cache is the deliberate exception.** It's high-churn and routinely sees transient,
one-shot flood sources you'll never route to; interning each would pollute the directory. Since
it only needs to answer "have I seen `(src,pkt_id)`?", key it on a compact hash of `src` (+
`pkt_id`) and never touch the NodeTable. A hash collision merely risks dropping one duplicate —
harmless. This also keeps `seen_or_insert` (a hot path) doing integer compares.

**Eviction policy.** `NodeTable` is bounded (CAP ~96). Evict only *cold, unpinned* slots
(LRU on `last_ms`, skipping `IS_NEIGHBOR` / active-route-dst). On eviction bump `gen` so any
lingering `node_ref` resolves to "unknown" rather than the new occupant. `log()` when the table
saturates (a saturated directory in a large mesh is a real signal, not to be swallowed silently).

---

## 4. Wire format is untouched by normalization

`node_ref` is an internal RAM optimization only. On air it stays exactly as
[`self-certifying-identity-plan.md`](self-certifying-identity-plan.md) §2 specifies: full 16-byte
end-to-end ids in `NetHeader`/announce reports/route dsts/command targets, and 1-byte link-local
aliases per hop. Codecs (`announce_codec`, `control`, `telemetry`, `locator_dir`) serialize/parse
full `NodeId` at the boundary (`nid_put`/`nid_get`, raw 16 bytes, no endianness) and `intern()`
on the way in. So the protocol break (below) and the representation refactor are independent and
could even ship in separate steps — but do them together to avoid touching the same sites twice.

Protocol-break specifics (the part that forces the coordinated reflash):
- **`NetHeader` (`packet.h:76`)** `dst`/`src` 4→16 B ⇒ `HEADER_BYTES` +24 B per data packet (the
  accepted §4 end-to-end cost; per-*hop* stays on the alias).
- **Control (`control.{h,cpp}`)** `target`/`aux` 4→16 B; `CTRL_MSG_BYTES` 75→87, `CTRL_BLK_BYTES`
  79→103; bump `CTRL_VER` 1→2. (Targets fit well under the 200 B cap.)
- **Telemetry REPLY** neighbour entries reference the **1-byte alias**, not a widened id → REPLY
  shrinks; no signature in REPLY (trust comes from the verified announce).
- **`NODE_ID_BROADCAST`** (`packet.h:26`, `forwarder.h:7` `BCAST_ID`) 0xFFFFFFFF → all-0xFF[16];
  every `== BCAST` check routes through the constant.
- **Mixed-version safety** via length checks (as `telemetry.h:29` already does) — old/new reject,
  don't mis-parse. Document the star-bench reflash.

---

## 5. Migration mechanics (what the compiler flags)

With most struct fields becoming `node_ref`, the churn splits:

| Category | Old | New |
|---|---|---|
| Equality (hot paths) | `slot.id == id` | `slot.ref == ref` (1–2 B compare, **cheaper than today's u32**) |
| Lookups | scan for `== id` | `intern(id)` once, then compare refs |
| Zero/sentinel | `id == 0`, `= 0` | `ref == NODE_REF_NONE`; full ids use `.is_zero()` / `= {}` |
| Serialize | `put_u32(p,id)` | resolve ref → `nid_put(p, full_id)` |
| Deserialize | `id = get_u32(p)` | `ref = table.intern(nid_get(p))` |
| Format | `"%08lX",(ulong)id` | resolve → `nid_hex(full_id, buf32)` (generalize `main.cpp:938`) |

Helper API in `mesh_types.h`: `nid_put/nid_get` (raw 16 B), `nid_hex` (32-hex), `nid_from_pubkey`
(`crypto_blake2b(out,16,pub,32)`), `NODE_ID_BROADCAST`, and a test-only `nid_from_u32`.

---

## 6. Resource budget (post-normalization)

- **Flash:** +2–6 KB. `crypto_ed25519_sign`/`_key_pair` and `blake2b` are currently
  GC'd-out (the binary links only `crypto_ed25519_check` + SHA-512); they re-link but reuse
  existing field arithmetic. Trivial against ~760 KB free.
- **RAM:** ≈ +2.5 KB net (NodeTable ~2.1 KB + refs ~0.4 KB), vs +3–5 KB without normalization —
  i.e. the widening is made roughly neutral. Keys add 96 B. **Confirm against the linker's
  reported `RAM:` % on the tightest target after the change**; if a target is short, lower
  `NODE_TABLE_CAP`, `SEEN_CACHE_SIZE` (64), or `MAX_ROUTES` (64). Watch the `send_beacon` stack
  frame (a transient widened `Announce` ~2 KB + monocypher scratch ~1–2 KB) — recheck the high-
  water of whatever task runs verify.
- **CPU / blocking:** keygen is one-time (first boot, ~6–12 ms, irrelevant). Signing own
  announces ~one op per signed beacon (≪1% duty). Per-frame verify is **gone** — verification is
  per-new-id at `intern` (§2), amortizing to ~zero in steady state. **Design rule:** the
  intern-time verify (~12–25 ms, blocking, single core) must run in the main loop/task context,
  never in the radio RX ISR, and a burst of new-node announces must not be verified synchronously
  in a way that starves RX or an active SAR transfer — queue them.

---

## 7. Tests & sequencing

The type change is atomic — host tests go red during the migration; first green checkpoint is
after the full sweep + test rewrite.

1. Land `NodeId` + `node_ref` + `NodeTable` (+ `nid_*` helpers, `nid_from_u32`) in the portable
   core; unify `packet.h` to include it.
2. Add a `test_node_table` suite: intern/resolve, dedup of equal ids, eviction bumps `gen` so old
   refs resolve false, `pin` protects live entries, saturation logs.
3. Sweep tables to `node_ref` (codecs first, then routing core, then `main.cpp`); rewrite test
   id literals via `nid_from_u32`. Host suite green again — the real checkpoint.
4. Wire-format + version bumps (`NetHeader`, `CTRL_VER`→2, telemetry alias REPLY) → reflash the
   RAK pair ([[hardware]]) and validate end-to-end before any controller-side work.

Keep this branch **identity-type-only**: no ACL, no keygen entropy work, no controller changes.
Those compose on top once the type + directory are solid.

---

## 8. Gotchas

- **`NodeId` must stay a bare `uint8_t[16]` POD** (no vtable/non-trivial members) so it's safe
  inside `AGN_PACKED NetHeader` with no padding, and header memcpy to/from the radio buffer stays
  valid. No endianness on id bytes — canonical order = the blake2b output.
- **Stale-ref discipline:** anything caching a `node_ref` across time must `resolve()` and handle
  the false (evicted) case; never assume a ref stays valid forever.
- **Seen-cache stays hash-keyed, not NodeTable-keyed** (§3) — the one table deliberately outside
  the directory.
- **Broadcast/flood checks** (`== BCAST_ID`) must all route through `NODE_ID_BROADCAST`; a missed
  one silently breaks flooding.
- **`AgnLoRa-%08lX` BLE name** (`main.cpp:527`) widens to 32 hex; the mobile app auto-fills its
  uplink locator from it and must read a longer/variable id ([[node-id-identity]]) — confirm with
  the app side.
- **Stray `uint32_t` ids:** any id-typed local not using the typedef becomes a hard compile error
  — good; grep `uint32_t.*id` after the sweep to be sure none survive.

Related: [[node-id-identity]], [`self-certifying-identity-plan.md`](self-certifying-identity-plan.md),
[`identity-vs-locator.md`](identity-vs-locator.md),
[`node-keygen-signed-announce-impl.md`](node-keygen-signed-announce-impl.md).
