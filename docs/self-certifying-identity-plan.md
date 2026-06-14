# Self-certifying node identity + membership ACL — wire-level & controller plan

Status (2026-06): **planned, not built.** This note operationalizes
[`identity-vs-locator.md`](identity-vs-locator.md) §2/§4/§5 into a concrete wire format and
controller-plane plan: each node mints its own Ed25519 keypair, derives its node id from
that pubkey (self-certifying), signs its announces, and the controller enforces a
membership allowlist over verified pubkeys. It supersedes the placeholder 32-bit FICR-fold
id ([[node-id-identity]]) and lands as a single coordinated firmware+controller reflash.

The whole point: make "membership" a real, spoof-proof boundary **without** turning the
controller into a single point of failure and **without** blowing the 200 B LoRa payload.

---

## 0. What this buys, and what it costs

| | |
|---|---|
| **Buys** | Node ids you can't spoof (id = `hash(node pubkey)`, self-certifying); a controller-side allowlist that gates *which* nodes it manages; identity verification on every announce. |
| **Costs** | One coordinated reflash (the `node_id_t` width changes — a protocol bump). Per-node keypair (+96 B flash). Wider announces (the accepted §4 tension). |
| **Does NOT cost** | A controller dependency in the addressing path. Nothing here assigns addresses. Kill the controller and the Tier-0 mesh keeps routing, exactly as today. |

---

## 1. The load-bearing idea: addresses are self-derived, never assigned

`identity-vs-locator.md` already splits **end-to-end locator** (the 16-byte node id) from
**per-hop locator** (the 1-byte link-local alias, §4). Two facts make the controller
*structurally* unable to become a SPOF for addressing:

1. **The 16-byte node id is `hash(node's own pubkey)`** — self-certifying *and*
   collision-free by construction (2¹²⁸). A node derives its own id from its own key. There
   is no assignment authority and nothing to arbitrate globally.
2. **The 1-byte alias is link-local** — negotiated locally between adjacent neighbors. The
   only thing that can ever *clash* is an alias within one node's neighbor set (256 values),
   and that is resolved between those two neighbors, never by the controller.

So the controller is a pure **verify-and-command** layer. It learns identities, gates
membership, and issues signed commands — but it is never in the path that lets a node be
addressed. This is the Tier-0 "kill it and the mesh keeps running" invariant
(Agent.md §4/§7), preserved by design rather than by best effort.

> A controller-*assigned* short locator was considered and rejected: it would buy nothing
> (the self-derived id is already collision-free) while making new-node admission depend on
> a live controller. Self-derived ids + link-local aliases dominate it on every axis.

---

## 2. The folded addressing rule

Carry the full 16-byte id **only** where the reference is end-to-end. Use the existing
1-byte alias everywhere the reference is per-hop / per-neighbor. Carry identity *proof*
(pubkey + signature) **only** in the announce — the one rare, signed, identity-publishing
frame.

| Reference is… | Carry | Width |
|---|---|---|
| End-to-end: announce report id, route `dst`, net-header src/dst, command `target`/`victim` | full self-derived id | 16 B |
| Per-hop / per-neighbor: route `next_hop`, telemetry REPLY neighbor entries | link-local alias | 1 B |
| Identity proof: node pubkey + self-signature | **announce only** | 32 + 64 B |

The announce report tuple `{id, q, alias}` (`announce_codec.h:16`) already binds each
neighbor's full id to *this* node's 1-byte alias for it. That binding is what lets the dense
frames stay on aliases: the controller ingests announces, builds a per-node `alias → id`
table, and resolves REPLY neighbor lists against it.

---

## 3. MTU budget (everything inside the 200 B `MAX_PAYLOAD`)

| Frame | Today | This plan | Fits? |
|---|---|---|---|
| Control POWER/CONFIRM | 75 B | `target` 4→16 ⇒ **87 B** | ✓ |
| Control BLOCK/UNBLOCK | 79 B | `target`+`victim` 4→16 ⇒ **103 B** | ✓ |
| Telemetry REPLY, 16 nbrs | ~152 B | nbr id → **1-byte alias**; **no pubkey/sig in REPLY** ⇒ ~108 B | ✓ |
| Announce report entry | 6 B | full id + alias ⇒ **18 B** | width cost lands here |
| Announce route entry | 11 B | `dst` 16 B + `next_hop` as **alias** ⇒ **20 B** | per-hop stays cheap |

- **Commands** are infrequent unicast; full-width ids fit with room to spare.
- **REPLY** is fixed by referencing neighbors via the alias and **dropping pubkey/sig
  entirely** — a reply's trust derives from the sender's already-verified *announce*
  identity, not from re-signing every reply. (Signing REPLY + full-id neighbors would hit
  ~336 B and overflow — explicitly out.)
- **Announce density** is the one genuine residual cost (18 B per full-id report; the §4
  line-90 "accepted tension"). Mitigation: rotate *which* `id↔alias` bindings each beacon
  publishes — routing between hops only needs the cached 1-byte alias, so a node need not
  re-publish every binding every beacon.

---

## 4. Firmware changes

| File / site | Change |
|---|---|
| `mesh_types.h:12` | `node_id_t` → 16-byte POD struct, but stored once in a central `NodeTable`; the ~6 id-bearing tables reference it by a 2-byte `node_ref` (**normalization**, so the widening is ~RAM-neutral and per-frame verify is eliminated). **The protocol bump** touches 194 sites across 22 files. The riskiest piece; planned in detail in [`node-id-widening-impl.md`](node-id-widening-impl.md). |
| `src/main.cpp` `NodeStore` (~:304) | Extend to persist the node's Ed25519 keypair (`seckey[64]` + `pubkey[32]`, +96 B in `/agn_node.cfg`). |
| `src/main.cpp` `setup()` first boot | If no keypair persisted: fill 32 B from hardware TRNG (`NRF_RNG` on nRF52, `esp_random()` on ESP32), `crypto_eddsa_key_pair()` (monocypher, already linked), persist. |
| `src/main.cpp` `derive_node_id()` (~:684) | Return `hash(pubkey)` (blake2b, truncated to 16 B) instead of the FICR/MAC fold. |
| `announce_codec.{h,cpp}` | Append optional `pubkey[32]` + self-signature (domain tag `"AGN-ANN-1"`, mirroring control's `"AGN-CTRL-1"`). Reports already carry `alias`, so the `id↔alias` binding is free. Backward-tolerant: trailing bytes ignored by old parsers. |
| `telemetry.h:46` `TelemNbr.id` | `node_id_t` → `uint8_t alias` (this node's link-local handle for the neighbor). REPLY shrinks to alias-width; **no signature added** → MTU solved. |
| `control.h` `CtrlMsg.target`/`aux` | → 16-byte. `ctrl_build*` / `ctrl_verify` offsets follow. `CTRL_MSG_BYTES` 75→87, `CTRL_BLK_BYTES` 79→103. Bump `CTRL_VER` → 2. |
| control *verify* path | Unchanged in spirit: a node still verifies the *controller's* signature with the controller's pubkey. The node's own key signs only what the node emits (announces). |
| **removed from earlier scope** | No locator-assignment handshake; no locator table on the node. Self-derived id + existing link-local alias do the job. |

---

## 5. Controller-plane changes

| File / site | Change |
|---|---|
| `controller/internal/keystore/keystore.go` | Add a pubkey **allowlist** (`map[pubkeyHex]→status`) + `ApproveNode/RevokeNode/IsAllowed`; persist in `controller.json`; include in `Export()` / backup. The global replay counter model is unchanged (counter is per-controller, not per-node). |
| `controller/internal/sign/control.go` | `target`/`victim` 4→16 byte; `unsignedBytes` 11→23, block 15→39. **No truncation logic** — full ids are carried. |
| ingest / topo | On each announce: verify the self-signature, check `id == hash(pubkey)`, check ACL; tag each node `verified` + `allowed/pending/blocked`. Build the per-node `alias→id` table from announce reports to resolve REPLY neighbor lists. "Managed nodes" filters to verified-and-allowed. |
| `controller/internal/httpd/{server,uistate}.go`, `index.html` | Trust badges (✓ verified / ⚠ unverified); a Security/approval surface (approve/revoke, pending queue); 32-hex id display; gate command `issue()` on `IsAllowed`. Persist ACL choices through backup/restore. |
| `controller/cmd/agnctl/main.go` | `acl list|pending|approve|revoke`; widen id parsing to 32-hex. Gate power/block/etc. on `IsAllowed`. |
| **removed from earlier scope** | No locator authority; no 4-byte truncation/collision-detection branch. |

---

## 6. Membership semantics (and the one soft dependency)

- **Membership = an approved pubkey on the controller's allowlist**, *and* a node whose
  announce self-signature verifies and whose id equals `hash(pubkey)`. Both must hold — an
  ACL entry without a verified signature is just a name; a valid signature without an ACL
  entry is `pending`.
- **Existing members are unaffected by a controller outage** — they keep their keys, ids,
  aliases, routes, and delivery. The control plane's dead-man rails (power auto-revert, the
  6 h heartbeat) already handle a silent controller.
- **New-node *admission* is controller-gated by design** — if the gatekeeper is offline, no
  *new* node gets approved. That is the desired security property, not a SPOF: it bounds
  *who the controller manages*, never *whether the mesh runs*. A self-derived id lets an
  unapproved node still route at the Tier-0 layer; it simply isn't *managed* until approved.

---

## 7. Sequencing

One coordinated reflash (the `node_id_t` width is a hard protocol break — mixed versions
reject each other via length checks, as telemetry already does, `telemetry.h:29`). Land
firmware (keygen + id derivation + signed announces + alias-based REPLY + 16-byte control)
and the controller changes (verify + ACL + 16-byte targeting) together. The ACL UI/CLI can
follow the reflash without a second flash, since it is controller-only.

The work is broken into three implementation notes, in dependency order:

1. [`node-id-widening-impl.md`](node-id-widening-impl.md) — the `node_id_t` type change via
   normalization (NodeTable + `node_ref`; do this first; everything else assumes 16-byte ids).
   This is also what keeps the widening RAM-neutral and moves signature verification to
   once-per-id instead of per-frame.
2. [`node-keygen-signed-announce-impl.md`](node-keygen-signed-announce-impl.md) — firmware
   keygen, id derivation, and signed announces (incl. the entropy gap that must be closed).
3. [`controller-verify-acl-impl.md`](controller-verify-acl-impl.md) — controller verify +
   membership ACL + 16-byte command targeting. Note the gateway↔controller `[ann] … pub= … sig=`
   console contract defined there — it's the seam between the firmware and controller halves.

Related: [[node-id-identity]] (the placeholder this retires), [[controller-power-mesh]]
(the optimizer that must keep working controller-less), [`phase4-controller-plan.md`](phase4-controller-plan.md)
§4e (key re-key, still deferred), [`remote-config.md`](remote-config.md) (signed PHY retune).
