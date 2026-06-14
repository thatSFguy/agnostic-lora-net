# Phase 4 — Tier-1 controller (RPi Zero 2 W)

Status (2026-06): **largely built.** The Go controller (`controller/`, `agnctl`) ships
4a (console ingest + global topology), §8 (passive airtime capture), 4b (signed
POWER/CONFIRM/BLOCK/UNBLOCK), and 4d (autonomous power-optimisation loop — now
**mesh-wide**: tunes each node against its weakest outbound link, with a mobility-aware
reserve/asymmetric strategy for nodes that self-report `mobile`). It serves a consolidated
web dashboard (Dashboard | Map | Configure | Flash) and the firmware itself, and survives
gateway drops (auto-reconnecting serial). Deferred: ROUTE override, auto-block of
pathological links, transfer boost, key re-key (§4e). See the top-level README for shipped
capabilities; the notes below are the original plan.

Phase 4 turns the manual "browser-as-controller"
setup into a real, field-resident **RPi controller** that collects global telemetry,
optimizes RF autonomously under safety rails, and stays optional — kill it and the
Tier-0 mesh keeps running (Agent.md §4, §7 Phase 4).

---

## 0. Why — the field problems this solves

From the first field test:

| Observation | Phase 4 capability that fixes it |
|---|---|
| All nodes turned way down → image (SAR) transfer was a poor/marginal link | **Per-node power optimization** (4d): the controller sets each node *just high enough* to hold the SNR margin on the links actually in use, instead of a blind global low. |
| "Probably not a good link — I don't know" | **Field dashboard** (4a): the RPi serves the live global topology (per-direction q, RSSI/SNR, SNR-margin) over its own WiFi — no laptop, no internet. |
| "I didn't have access to the web" | RPi is an **always-present field device** with a WiFi AP / captive portal to the dashboard; works fully offline. The controller also runs policy with no operator present. |
| Wanted to push a picture through a weak path | **Transfer boost** (4d): temporarily raise power along the active path for the duration of a transfer, auto-reverting after — driven by an operator tap or by auto-detecting an active SAR session. |

Phase 4 is **pure optimization layered on Tier 0 — never a dependency** (Agent.md §4).
Every connectivity-reducing action (power-down, block) is provisional with an
auto-revert rail, so neither a bad command nor the controller dying can partition the
mesh.

---

## 1. What already exists (the POWER slice)

| Piece | Where | State |
|---|---|---|
| Signed control codec — Ed25519 over domain-tagged bytes, replay counter | `lib/mesh/control.{h,cpp}` | ✅ POWER (1) + CONFIRM (2) |
| Node verify/apply + persisted pubkey & replay floor | `src/main.cpp` (`ctrl_*`, `/agn_ctrl.cfg`) | ✅ |
| Dead-man rail — power *decreases* auto-revert in 60 s unless CONFIRMed | `src/main.cpp` (`CTRL_REVERT_WINDOW_MS`) | ✅ |
| Unsigned success-only ACK (informational; can't change state) | `control.h` | ✅ |
| Open telemetry — neighbor reports fold into beacons/announces | `lib/mesh/telemetry.{h,cpp}` | ✅ |
| Controller key + signed POWER UI (**browser is the controller today**) | `web/map.html` (`ctrlSend`, `ctrlkey`) | ✅ |
| Controller key export/import (in the map backup) | `web/map.html` | ✅ — lets the RPi inherit the existing key |
| Remote PHY retune protocol + `apply_config()` seam | `docs/remote-config.md`, `RadioHal::apply_config` | spec'd; signed transport pending |

**Trust model (unchanged, already correct):** node IDs authenticate nothing; the
**signature + persisted monotonic counter** are the entire trust model. No unsigned
message may change node state. This carries straight into the RPi controller.

---

## 2. The gap

1. **No controller device.** A browser + a tethered gateway stand in manually. There is
   no autonomous collector/optimizer/signer, and no field-resident dashboard host.
2. **Topology is gateway-centric.** The map builds the graph from what the *connected*
   node hears + its announce cache. Phase 4 needs the **global per-direction** graph.
3. **Only POWER is signed.** BLOCK is local-console (Tier-0) only — it can't act on a
   node you're not physically connected to (exactly the limitation that blocks the
   operator's use case). ROUTE override isn't implemented at all.
4. **No RF policy.** Nothing decides power/block automatically from the topology.

---

## 3. Architecture

```
        phone / laptop browser  (unprivileged viewer)
                  │  HTTP + WebSocket over the RPi's WiFi AP (offline-capable)
                  ▼
        ┌─────────────────────────────┐
        │  RPi Zero 2 W — CONTROLLER   │   holds the Ed25519 key (single signer)
        │  • telemetry ingest          │   persists the monotonic replay counter
        │  • global per-dir topology   │   runs the RF policy engine
        │  • signs POWER/BLOCK/ROUTE   │   serves the dashboard (reuse web/ HTML)
        └─────────────┬───────────────┘
                  │  USB serial (the existing console/tunnel stream the map already parses)
                  ▼
        ┌─────────────────────────────┐
        │  controller node (a flashed  │   ordinary Tier-0 node; just the RPi's
        │  RAK/XIAO acting as gateway) │   ear+mouth into the mesh
        └─────────────┬───────────────┘
                  │  LoRa, multi-hop
                  ▼
              … the rest of the mesh (Tier-0, self-routing) …
```

Key decisions:

- **The RPi is the sole signer.** Move the controller key off browsers onto the RPi
  (import the existing key from the map backup so already-provisioned nodes keep
  working). Browsers become viewers that *ask* the RPi to sign via its API — the secret
  never leaves the managed device.
- **Reuse the gateway stream.** The RPi ingests the same console/tunnel text the map
  parses (`info`, `nbrdump`, announces). No new node firmware is needed just to collect.
- **Reuse the dashboard.** `web/map.html` becomes the controller-served UI; its
  client-side signing is replaced by calls to the controller's sign endpoint.

### Language & dev workflow — Go, laptop-first

The controller is written in **Go**, so it is developed and tested **on the laptop
first**, then dropped onto the RPi unchanged:

- **One static binary, cross-compiled.** Develop natively on the laptop; deploy with
  `GOOS=linux GOARCH=arm64 go build` for 64-bit Pi OS on the Zero 2 W (Cortex-A53), or
  `GOARCH=arm GOARM=7` for a 32-bit image. No runtime/deps to install on the Pi.
- **Stdlib does the hard part.** `crypto/ed25519` (RFC 8032) produces the exact
  signatures the firmware already verifies; `net/http` + WebSocket serve the dashboard;
  a serial lib (`go.bug.st/serial`) reads the tethered node's console/tunnel stream.
- **Laptop = a real controller.** Tether a flashed node to the laptop over USB and the
  Go binary is fully functional — collect telemetry, sign commands, serve the dashboard
  — before any RPi exists. The "final destination" is just a different deploy target.
- **First gate — signature parity. ✅ DONE.** Proven: for a fixed seed the Go signer
  (`controller/internal/sign`, stdlib `crypto/ed25519`) produces a control message
  **byte-identical** to the firmware's `ctrl_build`, with a matching pubkey, and it
  verifies `CTRL_OK` on-device — asserted in `test/test_ctrl_interop` (runs under
  `pio test -e native`). Confirms Go's `ed25519.PrivateKey` (seed‖pub) is a drop-in for
  the firmware's 64-byte monocypher secret and both are RFC 8032. Still open (ops, not
  crypto): **reuse the exported browser key** (import its seed) vs **mint fresh on the
  controller + re-`ctrlkey` every node**.

Suggested layout: `controller/` (Go module) — `ingest/` (serial + parsers ported from
`web/map.html`), `topo/` (global graph), `sign/` (the control codec, mirroring
`lib/mesh/control.*`), `policy/` (4d), `httpd/` (API + dashboard), `capture/` (§8).

---

## 4. Workstreams (each independently shippable)

### 4a — Controller foundation: collector + field dashboard host
- **Go daemon** (port the map's line parsers into `ingest/`) tethered to a controller
  node over USB serial. Maintains the live model: nodes, per-direction links
  (q_rx/q_tx, RSSI/SNR), battery, fw, power. Runs on the laptop first.
- Move the Ed25519 key to the RPi; expose a **sign+send** API and a read API (WebSocket
  push of the topology).
- RPi WiFi **AP / captive portal** serving the dashboard offline.
- **Milestone:** in the field, join the RPi hotspot on a phone → see the live global
  topology and link margins → manually issue a signed POWER command. *(Directly fixes
  "I didn't have access to the web.")*

### 4b — Extend the signed command set: BLOCK / UNBLOCK / ROUTE
- ✅ **BLOCK/UNBLOCK done.** `CTRL_BLOCK` (3) / `CTRL_UNBLOCK` (4) added to
  `lib/mesh/control.{h,cpp}` with a 15-byte layout (recipient `target` + victim `aux`),
  branching on cmd so POWER's wire format is untouched. Mirrored in the Go signer
  (`controller/internal/sign` `BuildBlock`) and proven **byte-identical** in
  `test/test_ctrl_interop`. Firmware handler routes to the existing
  `Router::block()/unblock()` with a **TTL dead-man rail** (`ctrl_blk_*` + a per-loop
  sweep): a signed block auto-expires unless renewed, so a stale block can never
  permanently partition the mesh. (`arg` = TTL minutes, default 30, max 120.)
- ✅ **Send path done.** The Go controller signs and pushes commands through the tethered
  node: `internal/keystore` (the Ed25519 key + persisted monotonic replay counter, seeded
  from wall-clock so it always clears prior floors), `internal/commander` (POWER/CONFIRM/
  BLOCK/UNBLOCK → the node's `ctrlsend <hex>` bridge), and an `agnctl` command REPL. Key
  custody per the operator: **reuse the browser key** now (`-import-backup` reads the map
  backup JSON), **mint fresh for production** (`-mint`). Also fixed the firmware `ctrlsend`
  bridge, which previously hard-rejected the 79-byte BLOCK message. Covered by
  `keystore`/`commander` tests (round-trip through the same verifier the firmware runs).
- ⬜ **ROUTE override** (`CTRL_ROUTE` 5) — **deferred (optional).** Needs a new Router
  route-override API and careful interaction with the DV routing/TTL; Agent.md marks it
  optional, and POWER + BLOCK already cover the operator's needs (RF optimization + remote
  blocking). Clean future addition via the same codec/interop pattern.
- ⬜ **Web:** the map's block button still uses the *local* console path; rewiring it to
  "block *any* node" through the controller waits on 4a's `httpd` (controller API).

### 4c — Telemetry uplink hardening (global, per-direction, fresh)
- Confirm every node's two-way neighbor table reaches the controller via announce
  propagation; where coverage is thin, add a **signed "report now"** poll (a node dumps
  its full table on request). Define per-link freshness/aging on the RPi.
- Build the global per-direction graph with asymmetry detection (reuse the map's
  `ASYM_THRESHOLD` + `SNR_LIMIT` margin model, applied network-wide).

### 4d — Autonomous RF optimization (the brain) + transfer boost
- ✅ **Power-optimisation loop done (v1).** `controller/internal/policy`: each cycle it
  reads the topology, computes each managed node's SNR margin (observed SNR − the SF floor,
  same `SNR_LIMIT` table as the map), and step-limits its TX power toward a target band
  (`-margin-low`/`-margin-high`, `-max-step`). **Dry-run by default** (`-optimize`); `-apply`
  actually signs+sends. The controller owns each node's power (absolute POWER), and a
  **decrease is only CONFIRMed once the node is re-observed reachable** — so a bad call or a
  controller death self-heals via the on-device 60 s revert. Every observation→decision→
  command→ack is written to a **JSONL audit trail + console narration** (`-policy-log`) for
  troubleshooting. Pure `Decide()` + the apply/confirm dance are unit-tested; runs on a
  replay for a no-hardware preview. *Scope:* optimises against the **tethered gateway's**
  measured SNR (link node→gateway) — exact for star/bench, a sound first approximation
  elsewhere; global per-direction optimisation is §4c.
- ⬜ **Auto-block** pathological links — same engine, not yet wired.
- ⬜ **Transfer boost** — raise power along an active path for a transfer, then revert.
- (original design notes below)
- Policy engine on the RPi computes, from the global graph:
  - **Power targets:** minimize each node's TX power subject to every *in-use* link
    keeping SNR margin ≥ threshold (the `SNR_LIMIT` table). Replaces "turn everything
    down and hope." Emits signed POWER commands; continuously re-CONFIRMs so the rails
    stay armed.
  - **Auto-block** pathological asymmetric/dead links with hysteresis + TTL.
- **Transfer boost:** operator tap (or auto-detect an active SAR session) raises power
  along the active path for the transfer's duration, then reverts. *(The failed picture
  send becomes: boost path → send → auto-revert.)*
- All commands provisional + auto-revert ⇒ killing the RPi returns the mesh to safe
  defaults on its own.

### 4e — Resilience, counter continuity, Tier-0 proof
- **Replay-counter continuity:** the RPi must persist its monotonic counter across
  restarts, or nodes reject post-restart commands. Define the store + a documented
  **re-key / re-provision** procedure (`ctrlkey` rotation).
- **Milestone (Agent.md):** controller lowers a node's power / blocks a bad asym link →
  mesh adapts; **kill the controller → revert rails fire, mesh keeps running.** Add a
  field/bench test script for exactly this.
- Tier-2 seam noted only: the RPi may later sync to cloud for the HLR/app bridge
  (Agent.md §4 Tier 2) — out of scope here.

---

## 5. Cross-cutting requirements

- **No connectivity-reducing command without a rail.** Power-down and block are always
  provisional (revert/TTL). The controller's job is to keep re-arming them; its absence
  is automatically safe.
- **Airtime budget.** Power boosts, blocks, and "report now" polls cost airtime — honor
  the duty-cycle/CSMA budget; batch and rate-limit on the RPi.
- **Key custody.** One signer (RPi). Browsers are unprivileged. Document key backup
  (already exportable) and rotation.
- **Offline-first field UX.** Everything an operator needs in the field works on the
  RPi's local AP with zero internet.

---

## 6. Suggested order & milestones

1. **4a** — RPi collector + offline dashboard + manual signed POWER  → *field visibility & control restored.*
2. **4b** — signed BLOCK/UNBLOCK (+TTL)  → *remote blocking, the operator's blocker.*
3. **4c** — global per-direction topology + freshness  → *the policy engine's input.*
4. **4d** — power-optimization policy + transfer boost  → *the picture send works.*
5. **4e** — counter continuity + the "kill the controller, mesh survives" proof.

Each step is usable on its own; 4a alone already changes the field experience.

---

## 7. STUB — Joining a private mesh & relinquishing control

> Status: **stub / design sketch.** Captures the intended onboarding flow so 4a/4b are
> built with it in mind; not yet specified to the wire.

**Scenario:** someone else has a node and wants to join *your* private mesh, ceding
admin authority to *your* controller.

**What "private" and "control" mean here (today's trust model):**
- The mesh is defined by **shared PHY** (freq/BW/SF/CR + the `sync word` "network
  colour", `board_config.h`). Same PHY = you can hear each other. **Reads are public by
  design** — the sync word is a filter, not a secret.
- "Control" is the **authenticated write path**: a node obeys power/block/route commands
  *only* from the controller key it has provisioned (`ctrlkey`). **Relinquishing control
  = installing the controller's pubkey and not running a competing controller.**

**Onboarding flow (out-of-band trust at join time):**
1. **Align PHY.** The joining node is flashed with the network's PHY, or the controller
   pushes a signed retune once it's provisioned (`docs/remote-config.md`).
2. **Relinquish control.** The joining node provisions the controller's pubkey —
   `ctrlkey <controller-pubkey>` over USB/BLE at onboarding. This is the human's
   deliberate act of trust: the node now obeys that controller. (The controller never
   needs the node's secrets — telemetry is open.)
3. **Controller picks it up.** The node's announces reach the controller; it appears on
   the dashboard and is now a managed member.

**Reclaim / leave:** `ctrlkey clear` drops the controller key → the node falls back to
local-only (Tier-0) admin. The owner regains full control instantly.

**Open questions to resolve when this graduates from stub:**
- **Membership ACL?** Today the controller manages *whoever* provisioned its key; there
  is no controller-side allowlist, and node IDs authenticate nothing (FICR-derived,
  spoofable). A real "approved members" gate needs the **pubkey-derived node IDs**
  (Agent.md §3/§5) — until then, membership = "I installed your key."
- **One controller per node.** `ctrl_pubkey[32]` holds a single key; handover = overwrite.
  Multi-controller / delegated control is future work.
- **Replay-counter reset on re-provision.** Switching controllers must reset/seed the
  node's replay floor so the new controller's counter is accepted. Define the procedure.
- **Actual confidentiality** (closed/encrypted membership, secret sync word, payload
  privacy) is a separate effort — routing metadata is public by design here.

---

## 8. STUB — RF utilization & chattiness capture

> Status: **stub / design sketch.** The operator's read is "the network may be too
> chatty." Before tuning anything, **measure** — this section plans the capture; the 4d
> policy engine consumes it.

**What spends airtime:** beacons (periodic), announces (neighbor reports), data,
forwards, ARQ acks/retries, SAR fragments + NACKs, CAD scans, control packets. We have
*counts* today (`radio_hal`: tx/rx/rx_err, `cad_busy_count`, `cad_forced_count`) but no
**time-on-air (ToA) accounting by class** and no channel-occupancy estimate.

**Capture, two layers:**
1. **Node-side instrumentation (firmware).** Accumulate **ToA per packet class** using
   RadioLib's `getTimeOnAir(len)` at send/receive, bucketed by class
   (beacon/announce/data/forward/arq/sar/control). Add a periodic **airtime report**
   (an open telemetry read, no signature) carrying the per-class ToA counters + the
   CAD-busy ratio. This yields a per-node **duty-cycle / channel-occupancy** estimate.
2. **Controller-side passive capture (Go, laptop-first).** The `capture/` package logs
   every frame the tethered node hears — timestamp, length, class, src, RSSI/SNR — to a
   local time-series (CSV/SQLite now on the laptop, identical on the RPi). From that:
   network-wide airtime %, packets/sec **by class**, busiest nodes, ARQ retransmit
   ratio, SAR fragment efficiency, beacon-overhead vs data-payload ratio.

**Outputs → optimization levers (feed 4d):**
- **Beacon overhead** dominating? Lengthen/adapt the beacon period (`net beacon`,
  already configurable; consider an adaptive/`auto` mode driven by neighbor stability).
- **High ARQ retransmits / low SAR efficiency** on a path? That's the weak-link signal —
  raise power along it (transfer boost, 4d) or block a pathological asym link.
- **High CAD-busy ratio?** Genuine congestion → back off non-essential chatter
  (suppress redundant announces, widen jitter) and rate-limit control traffic.

**Why laptop-first fits:** the Go controller + a tethered node is already a passive
**network airtime recorder** with zero extra hardware — start logging during the next
bench/field run, analyze offline, *then* decide what to trim. Capture before cut.

**Open questions:** ToA-by-class counter budget (RAM/flash on the node); report cadence
vs. the very airtime we're trying to save (the meter mustn't be a big talker); whether
duty-cycle enforcement becomes a hard cap or stays advisory.
