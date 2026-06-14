# LoRa Mesh Backbone — Project Plan

> **[DESIGN DOCUMENT — historical.]** This is the original project plan. Most of it is
> now built: Tier-0 (the full mesh data plane) and much of Tier-1 (the `agnctl` controller
> — signed control, autonomous power optimisation, web dashboard) are shipping. The
> planned MeshCore *fork* (§3) was superseded — the firmware is native (see
> `docs/meshcore-integration.md`). For what actually shipped and how to run it, see the
> top-level **README.md**. The requirements below (esp. Req 1–5) still govern the design.

---

## 1. Vision

Build a **LoRa backbone** — a dumb, efficient, app-agnostic transport that moves
addressed packets between nodes the way the internet moves IP. Applications
(a phone app talking to a hub, Reticulum, anything) ride on top as **opaque
payload**. The backbone is never programmed per-app.

A central service improves the network but is never required for it to run.

### Hard requirements (operator-set, non-negotiable)

1. **BLE + LoRa run at the same time.** A node serves a phone over BLE while the
   LoRa radio is actively TX/RX-ing, with no dropped links. Proven solvable on this
   exact hardware — stock Meshtastic, MeshCore, and Reticulum all hold a stable BLE
   link to the operator's **Samsung A42** *and* **Pixel 9 Pro XL** through live LoRa
   traffic. So the radio/BLE/host layer is **borrowed from working firmware, never
   hand-rolled.** Acceptance: BLE link to A42 *and* Pixel 9XL survives sustained
   LoRa traffic.
2. **Intelligent multi-hop routing.** A node with no direct link to the destination
   finds a working path through relays; nodes along the path forward intelligently
   toward it. Link-quality-aware — **not** flooding. (No existing firmware does this:
   Meshtastic floods; MeshCore floods + manual routes + flood fallback; Reticulum
   needs a host and can't run on bare MT/MC-class nodes.)
3. **Best path per direction, independently.** Forward and return each use their own
   best path — **no requirement they match.** One-way / asymmetric links get *used*,
   not discarded. (The core of the routing-sandbox premise; the part the controller's
   global per-direction link view exists to enable.)
4. **Minimize flash writes (hardware lifetime).** nRF52840 internal flash is rated
   ~10k erase cycles/page; ESP32 SPI flash comparable. A loose "write on every change"
   loop can brick a solar node in days. Rules: persist only what can't be rebuilt
   (identity, radio config — **not** routing tables / link metrics, which stay in RAM
   and rebuild from announces on boot); every write does a byte-equal compare against
   the last persisted snapshot first; debounce/batch frequently-changing values;
   schema bumps trigger a one-time rewrite, not per-boot. (Pattern already exists in
   `reticulum-loramesh` `ConfigStore::save_if_dirty` — reuse it.)

---

## 2. Design principles (the spine of every decision)

1. **The backbone stands alone.** Fixed nodes must keep routing with no internet
   and no controller. That is the floor and it never drops.
2. **The control plane rides the data plane.** The controller is one LoRa node
   (RPi Zero 2 W + radio); it can't hear everyone, so its telemetry and commands
   multi-hop through the same mesh. Therefore local routing must be solid and
   fully independent *first* — the controller is a passenger, never the engine.
3. **Graceful degradation in tiers.** Lose internet → fall back. Lose controller
   → fall back. Never a hard failure of the backbone.
4. **Airtime is the scarcest resource.** Every header byte and every beacon costs
   airtime under a duty/dwell budget. Small frames, piggybacked control, measured
   cadence.
5. **App-agnostic.** The backbone does not encrypt or inspect payload. Apps bring
   their own crypto. Only the **control plane is authenticated** (signed commands).
6. **The radio core never blocks.** All radio ops are interrupt-driven
   (RadioLib `startTransmit`/`startReceive` + DIO1) — never busy-wait through
   airtime. At SF11/BW250 a single TX/RX is hundreds of ms to >1 s; blocking the
   CPU that long starves the BLE stack and drops the phone link (observed in a
   prior LoRaMesher attempt). Short SPI bursts, SoftDevice-safe critical sections
   so nRF52 BLE timing is never violated. See §9.

---

## 3. Confirmed parameters

| Parameter        | Value |
|------------------|-------|
| Region / band    | US 902–928 ISM |
| Operating freq   | **904.375 MHz** (single fixed channel for v1) |
| Modem preset     | **Meshtastic LongFast PHY**: BW 250 kHz, SF11, CR 4/5, 16-sym preamble, CRC on, explicit header |
| Sync word        | **`0x4D`** — clear of MeshCore `0x12`, Meshtastic `0x2B`, LoRaWAN `0x34` (verified from MeshCore source) |
| Reference HW     | **RAK WisBlock RAK4631** — nRF52840 + SX1262 |
| Other HW (later) | LilyGo TLora (SX1276/1262), DIY 1 W nodes, misc |
| Base firmware    | **Fork of MeshCore** (meshcore-dev) — inherit radio/BLE/host, replace routing |
| RAK4631 BLE env  | `RAK_4631_companion_radio_ble` (MeshCore) |
| Radio HAL        | **RadioLib** (covers SX1262 + SX1276 from one API) |
| MCU platform     | `nordicnrf52` / Arduino (Adafruit nRF52 core), board `wiscore_rak4631` |
| Addressing       | Node ID = **16-byte** truncated hash of the node's own public key (self-certifying, collision-free; RNS-shaped format, *separate namespace* — a **locator**, not an app/RNS identity). 32-bit `FICR` fold is the Tier-0 placeholder. See [`docs/identity-vs-locator.md`](docs/identity-vs-locator.md). |
| Routing          | Per-direction next-hop tables → forward/reverse are independent (asymmetry is free) |
| Reliability      | Hop-by-hop ACK + small retry; optional end-to-end ACK per packet |
| Mobility         | Mostly-fixed, occasional movers (HLR tuned for slow updates) |
| Controller       | RPi Zero 2 W + LoRa node, on-mesh, optional internet backhaul |

---

## 4. Architecture — three tiers

### Tier 0 — Always on (no controller, no internet)
Fixed nodes beacon, discover neighbors, measure **bidirectional** link quality
(TX = how well I'm heard, RX = how well I hear, plus asymmetry — the model from
the routing sandbox), and route hop-by-hop from **local** tables. This tier alone
satisfies "at minimum keep fixed nodes working." If the RPi dies, nothing notices.

### Tier 1 — Controller reachable over LoRa
Nodes fold neighbor/link-quality into their beacons; the RPi collects them, builds
the **global topology**, and pushes back **authenticated** commands:
- set TX power (per node, per the FCC/airtime budget),
- block a bad/asymmetric link,
- optional route override.

Pure optimization layered on Tier 0 — never a dependency.

### Tier 2 — Internet reachable
RPi syncs to cloud for heavier compute, the **HLR / mobility directory**
(endpoint → serving node), remote management, and **gateway bridging** so a phone
app can reach a hub "via the internet." Lose internet → Tier 1. Lose RPi → Tier 0.

```
Tier 2  cloud / HLR / app bridge        (nice to have)
   │  (internet, optional)
Tier 1  RPi controller: optimize RF, signed commands   (improves the mesh)
   │  (LoRa, multi-hop, optional)
Tier 0  self-routing fixed-node mesh    (MUST always work — the floor)
```

---

## 5. Packet format (draft — finalized in Phase 0)

Two layers so hop-by-hop reliability is cleanly separated from end-to-end routing,
and asymmetric paths just fall out:

**Link header (per hop)**
- `prev_hop`, `next_hop` — 1-byte link-local neighbor aliases (negotiated per link)
- `link_seq` + flags — for hop-by-hop ARQ (ACK / retry)

**Network header (end to end)**
- `ver / type` — data | control | ack | beacon (1 byte)
- `flags` — ack-request, etc.
- `ttl` — hop limit
- `dst`, `src` — node-pubkey-hash IDs (**locators**). Target width **16 B** each
  (self-certifying, collision-free); **4 B** in the Tier-0 placeholder today.
- `pkt_id` (2 B) — dedup + end-to-end ACK

**Payload** — opaque (app's problem). An app's *own* identity (e.g. an RNS destination
hash) rides **inside** the payload and is never routed on by the mesh — the mesh routes
on the locator only. This is the app-agnostic boundary; see
[`docs/identity-vs-locator.md`](docs/identity-vs-locator.md). **Control packets**
additionally carry a signature so a node only obeys legitimate power/block/route commands.

> Goal: keep the combined header small. Per-hop addressing uses **1-byte link-local
> aliases**, so the 16-byte `src`/`dst` cost lands only on the end-to-end network header
> and announces — accepted as the price of self-certifying, collision-free identity.

---

## 6. Link metric & routing

- **Metric (from the sandbox):** derive a per-direction quality `q ∈ [0,1]` from
  RSSI/SNR. Learn RX locally; learn TX from the neighbor's report. Flag a link
  `asym` when `|q_tx − q_rx|` exceeds a threshold.
- **Tier-0 routing:** distance-vector with **per-direction** metrics
  (Babel-inspired — Babel is proven on exactly this kind of lossy/asymmetric link).
  Updates piggyback on beacons; cost ≈ `1 / q` in the relevant direction.
- **Tier-1 routing:** controller has the full graph, computes the global optimum,
  and can install route overrides that the DV layer honors.

---

## 7. Phased build plan (fork of MeshCore)

### Phase 0 — Fork & baseline (prove Req 1 on our base)
- Fork meshcore-dev/MeshCore; build `RAK_4631_companion_radio_ble` **unchanged** first.
- Apply our PHY config: 904.375 MHz, BW 250, SF11, CR 4/5, sync `0x4D`.
- Flash 2× RAK4631; **operator confirms BLE holds on A42 *and* Pixel 9XL under
  sustained LoRa TX/RX.**
- **Milestone:** stock-routing MeshCore on our PHY, BLE stable on both phones — the
  working baseline we fork from. *(Manual: operator flashes + pairs phones.)*

### Phase 1 — Own the routing seam (parity, no regression)
- Keep `Dispatcher` + radio/BLE/host untouched. Stand up our own mesh class that
  replaces `onRecvPacket`/`routeRecvPacket`/`send*`, initially reproducing flood (parity).
- **Milestone:** our routing carries packets end-to-end; BLE still stable; **nothing
  blocking added to `loop()`**.

### Phase 2 — Intelligent link-quality routing (Req 2)
- Neighbor table + bidirectional link metric (SNR/RSSI + asymmetry, from beacons);
  DV-style multi-hop path selection by link quality — not flood.
- **Milestone:** a node with no direct link reaches the destination via relays chosen
  by link quality; mesh re-routes when a relay drops.

### Phase 3 — Independent per-direction paths (Req 3)
- Per-direction next-hop tables; forward and reverse computed independently; one-way /
  asymmetric links **used, not discarded**.
- **Milestone:** a packet takes a one-way A→B hop outbound and a *different* return
  path home.

### Phase 4 — Tier-1 controller (RPi Zero 2 W)
- Node→controller telemetry; RPi builds the global per-direction topology and signs
  commands (power / block / route override). Reuse the existing HTML as the live dashboard.
- **Milestone:** controller lowers a node's power / blocks a bad asym link and the mesh
  adapts; kill the controller → mesh keeps running.

### Phase 5 — Other hardware + Tier-2
- TLora (SX1276) and 1 W DIY configs; per-class power caps.
- Tier-2: HLR / mobility directory, gateway bridge, phone-app hub.
- **Milestone:** heterogeneous nodes interoperate; a phone reaches an endpoint via a
  gateway; a mobile endpoint re-homes and stays findable.

**Cross-cutting:** preserve MeshCore's BLE tuning + pinned forked nRF52 BSP
byte-for-byte; **never block `loop()`**; flash dirty-compare (Req 4); signed control plane.

---

## 8. Open decisions (revisit, not blocking)

1. **~~Modem params (SF / BW / CR).~~** RESOLVED — use the Meshtastic **LongFast**
   PHY: BW 250 kHz, SF11, CR 4/5. Exposed as config but network-wide (all nodes
   must match to hear each other). ~8.2 ms symbol time → airtime stays the
   governing constraint.
2. **FCC compliance for the 1 W nodes** (still open). At 250 kHz a fixed channel
   does **not** meet §15.247's ≥500 kHz digital-modulation requirement, so the 1 W
   class can't legally run full power here. Options: throttle the 1 W nodes to
   legal power on this channel now, or add **frequency hopping** later (FHSS allows
   1 W at this bandwidth). Not a blocker for Phase 0–2 — the PHY choice is
   identical either way.
3. **Network size ceiling** before plain DV needs controller assistance.
4. **Payload MTU** the backbone advertises to apps.

---

## 9. Prior art & hard constraints (research + a prior attempt)

**Survey verdict — borrow, don't rebuild:**
- **Reticulum** — closest *philosophically* (key-derived self-certifying addresses,
  app-agnostic transport). But its routing requires a **symmetric reverse path**
  (discussion #1017); no real non-hub-and-spoke multi-node deployments found in the
  field. Asymmetric routing is **ours to build**, not inherited.
- **LoRaMesher** — closest *code* (RadioLib distance-vector; route table already has
  `rssi`/`snr`/`link_quality`/`is_network_manager`). But routes on **hop-count
  only**, is ESP32/FreeRTOS-bound, and its **blocking** radio model caused the BLE
  failure below. Borrow its data structures, **not** its execution model.
- **MeshCore** — smart flood-once-then-direct routing; source-routed + symmetric.
  Reference only.
- **deadmesh / IP2LoRa / TLoRa** — Internet-over-LoRa gateway/proxy patterns.
  Reuse for Tier 2, don't rebuild.
- **SDN-LoRa / ATPCA papers** — the only prior art for the Tier-1 controller; no
  open firmware does centralized power/link control → our genuine novelty.

**Hard constraint — BLE + LoRa coexistence (Req 1):**
Every hand-rolled attempt at BLE+LoRa on nRF52 has failed — including the prior
`reticulum-loramesh` repo, whose "it's Samsung's fault" postmortem was **wrong**:
stock Meshtastic, MeshCore, and Reticulum all hold stable BLE to the operator's
Samsung A42 *and* Pixel 9XL on the same hardware. Non-blocking radio (principle #6)
is necessary but was **not sufficient** alone. Conclusion: **never hand-roll the
radio/BLE/host layer — take it from working firmware.**

**Decision (resolved) — fork MeshCore (meshcore-dev).** Chosen over Meshtastic on
evidence from reading both:
- MeshCore's `Dispatcher`↔routing seam is a **single virtual**
  (`Mesh::onRecvPacket → DispatcherAction`); all radio scheduling, duty-cycle, CAD,
  dedup, and queueing sit *below* it, routing-agnostic. Payload container is opaque
  (`PAYLOAD_TYPE_RAW_CUSTOM`). Routing state is RAM-only (Req 4 free). ~2–4 wk effort.
- Meshtastic's seam is real but buried under protobuf / PortNum / MQTT / NodeDB, and
  its **phone protocol is protobuf too**, so "app-agnostic" means surgery into the
  host layer — ~6–10 wk, while carrying 30 MB of features.

**Keep byte-for-byte (Req 1):** the async RadioLib wrapper + DIO1 IRQ state machine
(`RadioLibWrappers`, `CustomSX1262*`), `nrf52/SerialBLEInterface` with its
conn-param / supervision-timeout / advertising-watchdog tuning, and the **pinned
forked nRF52 BSP** in `[nrf52_base]` (load-bearing — exists to stop BLE lockups).
**Replace:** `Mesh::onRecvPacket` / `routeRecvPacket` / `send*` with our link-quality
+ independent-per-direction routing. Borrow self-certifying addresses from Reticulum.

---

## 10. First code step (on your approval)
Phase 0: scaffold the PlatformIO project, the SX1262 radio HAL (interrupt-driven,
non-blocking from the start), the packet header, and a two-RAK-node link that
prints RSSI/SNR.
