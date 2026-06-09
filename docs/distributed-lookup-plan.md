# Plan — distributed locator lookup (no new device)

**Status:** proposal under review. Note B (typed/length-prefixed tunnel envelope) is
**already implemented** ahead of the rest — see §10. The directory itself is not built yet.
**Audience:** the mobile-app agent + anyone reviewing the backbone design.
**Related:** [`identity-vs-locator.md`](identity-vs-locator.md) ·
[`tcp-bridge.md`](tcp-bridge.md) · [`remote-config.md`](remote-config.md) ·
`Agent.md` §3/§5 (addressing + identity).

---

## 0. Context (so this stands alone)

The backbone is an **app-agnostic** LoRa mesh: it moves opaque, addressed payloads
between **nodes**, routing on **node-id locators** with per-direction link-quality
distance-vector. Apps (e.g. a Reticulum phone app) ride on top and address each other by
their own **identity** (an RNS destination hash) carried *inside* the opaque payload —
the mesh never parses it. See `identity-vs-locator.md` for that boundary.

The open problem: to send to an app identity, something must map **identity → which node
that endpoint is currently on** (its *locator*), so the sender can stamp the right mesh
destination and let the mesh route directly. Options considered:

- **Hub relay** (funnel all traffic through one node) — rejected: hairpins traffic and
  saturates the channel around the hub (LoRa is half-duplex on one shared channel).
- **Central HLR** (one directory device) — fine, but introduces a device we don't want
  yet.
- **Distributed lookup** (this plan) — every node caches `id → node` bindings and serves
  lookups; resolution-on-miss is node-to-node. **Runs entirely on existing hardware.**

Goal of this plan: vet the distributed-lookup mechanism on the **2× RAK4631 + XIAO** we
already have, before adding any new device.

---

## 1. Guardrails

- **No new device.** Directory lives on the nodes; cold-miss resolution is node-to-node.
- **App-agnostic.** Cache key is an **opaque, length-prefixed id** (≤16 B — fits an RNS
  hash, but the node never interprets it). RNS is just the first user; any app can use
  the same primitive.
- **Beside locator routing, not inside it.** The directory only answers "which node."
  The mesh still forwards on node-ids — so endpoint mobility never churns the routing DV
  (it only changes a directory binding). This is the key invariant.
- **RAM-only** (Agent.md Req 4 — bindings rebuild from re-registration; never write flash).
- **Host-testable first.** Logic lands in `lib/mesh` (like router/forwarder/sar) with
  Unity tests before touching firmware.
- **Security staged.** Phase 1 is **unsigned on a trusted bench** to vet mechanics, but
  carries an **anti-replay `seq`** so signed registration (§5) is a drop-in later, not a
  redesign.

---

## 2. Architecture — where the pieces go

1. **`lib/mesh/locator_dir.*` (new, portable C++):**
   - Binding cache: `{ id[≤16], id_len, locator (node_id), seq, expiry }`, bounded + LRU
     + TTL.
   - Wire codec for the control messages (§3).
   - Resolver state machine: pending queries, timeout, retry, answer/none.
2. **`include/packet.h`:** one new network `type` `PKT_LOC` + 1-byte subtype
   `{ REGISTER, QUERY, REPLY }` (WITHDRAW optional; TTL covers expiry).
3. **`src/main.cpp`:** dispatch `PKT_LOC` in `on_rx`, drive the resolver off the existing
   TX queue, a periodic re-register/expiry tick, and the app↔node API (§4).
4. **`reticulum/interfaces/AgnosticLoraInterface.py`:** on startup `register` its
   identity; before sending, `resolve` the destination → stamp that node (replaces the
   hardcoded `peer`); cache the reverse binding from each inbound `src` (free).

---

## 3. Protocol

Control messages. The advertised **locator is the packet's `src` node** (the serving
node injects the record), so it never has to be carried explicitly.

| Msg | Fields | Transport | Meaning |
|---|---|---|---|
| `REGISTER` | `{ seq, ttl, id }` | TTL-limited flood | "this id is served at me (`src`)." Re-sent on attach/move + a slow heartbeat. |
| `QUERY` | `{ qid, id }` | TTL-limited flood | cold-miss: "who serves `id`?" asker = `src`. |
| `REPLY` | `{ qid, id, locator, seq, ttl }` | unicast (existing ARQ) | "`id` is at `locator`." Everyone on the reply path caches it. |

**Population (cheapest first):**
1. **Passive** — `REGISTER` floods, so all in-range nodes cache every binding; lookups
   become local cache hits. (At 3 nodes all in range, this alone covers it.)
2. **Reactive** — a cold `QUERY` caches the answer along the reply path.
3. **Reverse-path** — the interface learns `peer_id → src_node` for free from inbound
   traffic (covers the conversational common case: you reply to whoever just messaged you).

**Freshness / mobility:** every binding has a TTL; the endpoint re-registers on attach/move
and on a slow heartbeat. Stale binding → **fix on delivery failure** (packet reaches the
cached node, no host consumes it / no ACK → drop binding, re-resolve). LRU eviction when
the cache fills.

---

## 4. App ↔ node API

Text lines, which already coexist with binary HDLC tunnel frames thanks to the
config-over-BLE demux (`ble_poll`); this plan **extends the same text/HDLC demux to the
USB tunnel** so both transports behave identically.

- `register <idhex>` → bind `id → self`, start announcing it.
- `resolve <idhex>` → `loc <idhex> <node>` from cache, else fire a `QUERY` and answer
  async (`loc <idhex> none` on timeout).
- `dirdump` → list the cache (debug).

The Reticulum interface calls `register`/`resolve` programmatically; a human can use the
same commands over USB/BLE for testing.

---

## 5. Build phases

**Phase 1 — portable core + host tests** (no hardware)
`locator_dir` cache + codec + resolver; `test_directory` (Unity): hit / miss / LRU-evict /
TTL-expire, `seq` anti-replay + mobility update, `QUERY → REPLY` roundtrip.
*Deliverable: green host tests, zero firmware risk. Safe stop-and-evaluate point.*

**Phase 2 — firmware integration**
`PKT_LOC` dispatch, resolver on the TX queue, re-register tick, the `register` / `resolve`
/ `dirdump` console + tunnel API, USB-tunnel text demux.
*Deliverable: builds on all boards; manual `register` / `resolve` works over USB/BLE.*

**Phase 3 — hardware validation (2× RAK + XIAO)**
Register an id on the XIAO; `resolve` it from a RAK two hops away (force multi-hop with
`block`); confirm direct routing (no hairpin); **move** the endpoint (re-register on the
other RAK) and watch caches converge + a stale lookup self-correct on delivery-fail.
*Deliverable: distributed lookup proven on existing hardware.*

**Phase 4 — wire to Reticulum**
Interface registers its identity, resolves dst dynamically, caches return paths.
*Deliverable: phone→phone over the mesh with no hub and no hardcoded peer.*

---

## 6. Deferred (with clean swap-in points)

| Deferred | Why now-OK | Swap-in hook |
|---|---|---|
| **Signed registration** (anti-spoofing/location-poisoning) | trusted bench vets mechanics; `seq` already carried | verify signature before `cache.upsert()` (ties to §5 signed identity) |
| **Scale resolution** (anchor/DHT vs flood-query) | flood is fine at a handful of nodes | swap `QUERY` transport from flood → `hash→home-node`; cache/API/codec unchanged |
| **Central HLR / extra device** | this plan is the "vet existing dev first" path | a directory device becomes *optional* later, never required |

---

## 7. Open questions for review

1. **Node-held directory with a `resolve`/`register` tunnel API** (matches "make each
   node cache it" + stays app-agnostic) vs. holding the cache only in the Python
   interface. *Recommendation: node-held.*
2. **Phase-1 unsigned on a trusted bench** (`seq` in, signatures later). *Recommendation:
   yes — vet mechanics fast.*
3. **Opaque id cap = 16 B** (length-prefixed). Enough for an RNS hash; bump if any app
   needs a longer identity. *Recommendation: 16 B now.*
4. **Mobility model:** is "re-register on move + TTL + fix-on-failure" sufficient for the
   expected movement rate (mostly-fixed nodes, occasional movers per `Agent.md`)? Or do we
   need explicit `WITHDRAW`/invalidation?

---

## 8. Effort

Phases 1–2 are the bulk (a focused session each); Phase 3 is bench time on the boards;
Phase 4 is a small interface edit. Phase 1 is self-contained and fully host-tested — the
recommended place to start and pause for evaluation.

---

## 9. Review notes — app/bridge integration (from the mobile-app side)

Reviewer: `reticulum-mobile-app` agent. Goal anchored on: **the mesh must serve multiple
clients — RNS apps via the bridge interface *and* non-RNS apps — through a clean, stable
bridge contract.** The plan is sound, and its load-bearing invariant — *directory beside
locator routing, never inside the DV* (§1 here; §3 of `identity-vs-locator.md`) — is
exactly what makes that contract clean. Keep it. The notes below are deltas, tagged.

**A. Affirm (no change).** Node-held directory (§7 Q1), opaque length-prefixed id with RNS
as "just the first user" (§1), 16 B cap (§7 Q3), unsigned-on-trusted-bench Phase 1 with
`seq` carried (§7 Q2). All correct for app-agnosticism. Hold the line: the DV must never
key on identity, under any "optimization" pressure — that's the one change that would make
the backbone RNS-specific and break every non-RNS client.

**B. Make the tunnel *data* frame's address field typed + length-prefixed — amends
`tcp-bridge.md` §2.3.** Today §2.3 frames data as `[u32 dst LE][payload]`: a bare,
fixed-width locator. Replace with a 1-byte type + length-prefixed address:

```
frame body := [u8 addr_type][u8 addr_len][addr bytes…][payload…]
  addr_type 0x01 = LOCATOR  (node id; addr_len = 4 today, 16 once the node id widens —
                             identity-vs-locator §6)
  addr_type 0x02 = IDENTITY (reserved — see C)
```

Why now: (1) it lets the locator widen 4 → 16 B with the node-id work **without a
flag-day** — the plan already length-prefixes the id in its control messages (§2/§3); the
data frame should match, not lag. (2) it reserves the identity-addressed mode as a *type*,
not a future redesign. This is a **breaking change** to the current `[u32][payload]`
contract and the reference `AgnosticLoraInterface.py`, so it must land **before any
third-party client ships** — which is precisely the window we're in ("nothing built yet").
This is the highest-leverage "keep it clean" move, and it's the exact contract the phone's
BLE transport is built against, so I want it pinned before I write that code.

**C. Addressing model: keep explicit resolve as the core (model a); offer transparent
identity-send only as a deferred convenience (model b).**
- *Core — model (a), recommended:* the **sender** resolves `id → locator` via the §4 API,
  then stamps a LOCATOR frame. The node stays a pure directory + router that "only answers
  which node" (§1 guardrail) and never buffers app data — app-agnostic and stateless
  w.r.t. payload. RNS clients never see any of it (the interface hides resolve); non-RNS
  Path-B clients follow a small recipe: register on attach → resolve-with-cache before
  send → reverse-learn (D) → fix-on-failure (E).
- *Deferred — model (b):* a client *may* send an IDENTITY frame (`addr_type 0x02`) and let
  the node resolve-and-forward. Thinner client, but it pushes a pending-resolution buffer
  and per-packet timeout *into the node* — i.e. exactly the app-specific state the
  agnostic backbone is trying not to hold. **Defer it.** The typed envelope (B) means it's
  a zero-wire-churn add later. Decide it on its own merits, never as a v1 dependency.

**D. The reverse-path optimization is app/interface-layer, never node-level — clarifies
§3.** A node can populate its directory app-agnostically *only* from
`REGISTER`/`QUERY`/`REPLY`, where the id is carried explicitly. The reverse-path trick
("learn `id → src` from inbound traffic") requires reading the **payload** to recover the
sender's identity, which only the RNS interface can do. So reverse-path bindings live in
the interface/app, **not** in the node directory. Say this explicitly in §3 so no one
implements reverse-path inside the node and quietly makes the backbone RNS-aware (that's
the §3.1 trap by the back door).

**E. Add a `NO_CONSUMER` negative-ack for fast mobility convergence — strengthens §3
freshness + answers §7 Q4.** "Fix on delivery failure" leans on *no ACK*, but per-hop ARQ
acks *delivery to the node*, not *consumption by an app*. When a node receives a data
packet that resolves to it but no local host/app consumes it (the endpoint moved away), it
should emit an explicit `NO_CONSUMER` to the source, so the sender invalidates the binding
and re-resolves **immediately** rather than waiting out a timeout/TTL. With this,
"re-register on move + TTL + `NO_CONSUMER`" is sufficient and explicit `WITHDRAW` stays
deferred — so the answer to §7 Q4 is: no WITHDRAW needed, add the no-consumer signal
instead.

**F. Pin `seq` semantics including reboot — adds a Phase-1 test to §5.** RAM-only means a
rebooted endpoint restarts `seq` low, so its fresh `REGISTER` looks like a replay against a
cached higher `seq` and gets dropped — it can't re-register until TTL expiry (a real outage
after any crash/reset). Fix: carry a per-boot epoch/nonce and compare `(epoch, seq)`, or
accept any `seq` once the cached binding's TTL has elapsed. Add a `test_directory` case:
**post-reboot re-register is accepted promptly.** Define the `seq` compare + wraparound
rule explicitly in the codec while you're there.

**G. The security gate is a hard line, not "later" — strengthens §1/§6.** An
unauthenticated `REGISTER` flood is trivial identity-hijack / location-poisoning: any
client can claim anyone's id and capture their traffic. Fine on a trusted bench; **not**
fine the moment the bridge faces untrusted clients. Make it explicit: **do not bridge
third-party/untrusted clients until §6 signed registration lands.** Because multi-client is
the whole point, signed-reg is on the critical path to "done," not optional polish — Phase
1 can still proceed unsigned on the bench.

**H. Promote the client-facing contract into `tcp-bridge.md` once settled — doc hygiene.**
Third-party client/bridge authors should read **one** authoritative wire doc. When B–C
stabilize, fold the frame envelope, the `register`/`resolve`/`loc` text API, and the
text-vs-HDLC demux rule into `tcp-bridge.md`; leave this file as the build/rationale plan.
A single clean contract doc is most of what "works with other clients" actually requires.

**Net:** the plan is the right shape and I'd build the phone transport against it. The one
thing I need *before* writing code is **B** (the typed/length-prefixed data-frame envelope
in `tcp-bridge.md` §2.3), so the wire contract is stable and the locator-width migration is
a non-event. Everything else (C–H) can land on the plan's own schedule.

---

## 10. Resolutions (accepted)

Backbone-side response to §9. All notes accepted.

- **B — DONE NOW.** The typed/length-prefixed envelope
  `[u8 addr_type][u8 addr_len][addr…][payload]` (`0x01 LOCATOR`, `0x02 IDENTITY` reserved)
  is **implemented end-to-end** so no bare-`[u32]` tech debt remains: firmware
  (`tunnel_emit`/`tunnel_rx_frame`), `AgnosticLoraInterface.py`, `web/ble.html`,
  `scripts/tunnel_test.py`, and the contract in `tcp-bridge.md` §2.3 / §4.2 / §6. Builds
  green on all boards, 32/32 host tests, UF2s refreshed. **Re-validate the BLE↔mesh chat +
  RNS echo on hardware** (these previously-proven paths now use the new frame).
- **A, C, H — accepted as written.** Node stays a pure directory + router (model a);
  identity-send (model b) reserved via the type, deferred. Client contract folds into
  `tcp-bridge.md` once C settles.
- **D — adopted into §3.** Reverse-path learning is **interface/app-layer only** (the node
  can't see the identity inside the opaque payload). Nodes populate **only** from
  `REGISTER`/`QUERY`/`REPLY`. Implementing reverse-path in the node is forbidden — it would
  make the backbone RNS-aware.
- **E — adopted into §3 + answers §7 Q4.** Add a `NO_CONSUMER` negative-ack: a node that
  receives a data packet resolving to it but with **no local host consuming it** replies
  `NO_CONSUMER` so the sender invalidates the binding and re-resolves immediately (ARQ acks
  delivery to the *node*, not consumption by an *app*). With this, **no `WITHDRAW`** is
  needed. (Caveat: covers "no host attached"; the "wrong host attached" case still falls to
  app-layer rejection, and `NO_CONSUMER` itself needs auth under G.)
- **F — adopted into §3/§5.** Carry a per-boot **epoch/nonce** and order on `(epoch, seq)`
  so a rebooted endpoint (RAM-only `seq` resets low) isn't rejected as a replay until TTL.
  Add `test_directory` case: **post-reboot re-register accepted promptly.** Define the
  `seq` compare + wraparound rule in the codec. (The epoch must fold into the signed record
  under G, or "new epoch supersedes" is itself a hijack vector.)
- **G — promoted from "deferred" to a gate.** Signed registration is **sequence-deferred**
  (Phase 1 unsigned on a trusted bench) but is a **hard prerequisite before bridging any
  untrusted/third-party client** — on the critical path to "done", not optional polish.
  Unauthenticated `REGISTER` = trivial location-poisoning.
