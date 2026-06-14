# Plan: `web/map.html` — mesh map & node management console

> Note (2026-06): the map + node-management console described here also ship **inside the
> `agnctl` control-plane dashboard** (Map / Configure tabs at `localhost:8080`), now the
> primary path when a controller is running. The standalone `web/map.html` / `web/manage.html`
> remain for offline / no-controller use. The plan below is still the reference for the
> gateway data model and map UI.

Real geography from day one — OSM tiles, with a configurable default map center/zoom (the
map fits to placed nodes once you position them). Target deployments are
**kilometre-scale** rural/suburban spreads, which is exactly the
regime where link asymmetry and TX-power differences start to matter — the map
is the tool for seeing it.

**Deployed nodes have NO USB.** The app reaches the mesh through exactly one
**gateway node** — over Web Bluetooth (NUS) from wherever the operator stands,
or Web Serial on the bench. Everything beyond the gateway is learned over the
air. That makes remote telemetry the core data plane, in three tiers:

1. **Gateway's own state** — `info` over the attached transport. Free today.
2. **One hop out, passively** — every node's ~10 s beacon carries its announce:
   `Report{id, q, alias}` for each of ITS neighbours. The gateway therefore
   already hears its neighbours' link tables. New console verb `nbrdump`
   (tiny firmware addition) exposes that cached view to the app. For small
   clusters where everyone hears everyone, this alone paints the full map.
3. **Multi-hop, on demand** — new mesh message pair `STATUS_QUERY <id>` /
   `STATUS_REPLY` reusing the locator directory's QUERY/REPLY flood plumbing:
   the gateway floods a query, the target unicasts back a compact status blob
   (fw, uptime, vitals, neighbours with q both ways, routes). Read-only, so it
   ships before Tier-1 signing; **remote control commands stay gated on
   signed control packets** (node ids are non-authenticating — never a trust
   anchor for writes).

So unlike the original draft, this plan has a small firmware component
(`nbrdump` + STATUS query/reply, est. one version bump) — the gateway's own
`info` already covers tier 1.

---

## 1. Architecture

- One **self-contained HTML file** (`web/map.html`), same pattern as `ble.html`
  / `manage.html`: no server, no build step. Leaflet + OSM tiles from CDN
  (graceful plain-canvas fallback when offline).
- **Transports:** Web Bluetooth to the gateway node (field), Web Serial (bench).
  Same text console + HDLC demux either way — the `manage.html` connect code is
  the starting point. NOTE: a node has ONE BLE client slot; while the map holds
  it, a phone can't use that node — prefer a dedicated/spare gateway node, and
  surface "you are occupying this node's BLE slot" in the UI.
- Polling: gateway `info` every ~5 s; `nbrdump` every ~15 s; STATUS_QUERY each
  placed remote node round-robin (configurable, default ~60 s each — airtime
  is the budget, and queries ride the same channel as user traffic).
- Parsed console lines:
  - `node <id8>  neighbors=N routes=N blocked=N`
  - `  nbr <id8>  q_rx=NN q_tx=NN (x100)  myAlias=N theirAlias=N`
  - `  route dst=<id8> via=<id8> cost=N hops=N`
  - `fw X.Y.Z built …`, `cad on|off busy=N forced=N`, heartbeat (`stk=`, `txq=`)
  - new: `nbrdump` / STATUS_REPLY rendered in the same `nbr`/`route` line format
    so the app has ONE parser.
- The mesh picture is the union of gateway + announce-derived + query-derived
  tables, each datum tagged with **source + age**. Nodes known only secondhand
  render as **ghost nodes** until a STATUS_REPLY firms them up.

## 2. Map & node placement

- Default view: a generic center/zoom until nodes are placed (then the map fits to them).
- New nodes land in a sidebar "unplaced" tray → click node, then click the map
  to place (or drag an existing marker).
- Positions persist in `localStorage` keyed by node id, with **JSON
  export/import** so a layout can be committed to the repo / moved between
  machines (`web/node-positions.json` convention).
- Marker: short node id + freshness ring (green = heartbeat fresh, amber =
  stale, faded = lost) + badges: fw version, BLE client attached, TX power
  (dBm), CAD busy rate. Ghost nodes render hollow/grey.

## 3. Link visualization (symmetric vs asymmetric)

A LoRa link is two links; draw it that way.

- Each adjacency = **two parallel offset arrows**, one per direction
  (A→B uses A's `q_tx` toward B / B's `q_rx` of A — cross-checked when both
  ends are attached).
- **Color by quality**: green ≥ 0.90, amber 0.60–0.90, red < 0.60.
  **Width ∝ quality.**
- Direction with unknown/zero q renders **grey dashed** → a one-way link is
  one solid arrow out, one dashed ghost back. Unmissable.
- **⚠ midpoint badge** when |q_rx − q_tx| exceeds the firmware's
  `ASYM_THRESHOLD` (the same test `Neighbor::asymmetric()` uses).
- Click a link → popup: q both directions *from both ends* when available,
  aliases, blocked state. If the two ends disagree (A's q_tx ≉ B's q_rx),
  outline the link **dotted purple** — stale data or a genuinely weird link.
- Blocked links: black with ✕.
- Per-link sparkline (last ~50 polls) in the popup — asymmetry that comes and
  goes is a different problem from asymmetry that is structural.

## 4. Node focus mode

Click a node → its links bolden, everything else dims, and its **routes**
overlay as curved blue arrows per destination through the via-node (chained
multi-hop when the via node's table is also known), labeled cost/hops.
This makes "the route disagrees with the link quality" — the interesting
routing bugs — visually obvious. Esc / click-away exits.

## 5. Management panel (right sidebar, per selected node)

Until Phase C, **actions apply only to the gateway node** (the one the browser
is attached to); remote nodes show telemetry with action buttons disabled and
a "requires signed control plane" tooltip. Walking up to a node with the
laptop and connecting BLE makes *it* the gateway — field servicing works today.

- Vitals: fw, uptime, `stk=`, `txq=`, CAD busy/forced, BLE state + PIN.
- Actions (existing console verbs, no new firmware):
  - `rf` get/stage/apply — with the retune-safety warning from
    `docs/remote-config.md` surfaced in the UI
  - `ble on|off`, `blepin`
  - `block <id>` / `unblock <id>` — pick the target by clicking its marker
  - `dirdump`, `cad on|off`
  - raw command box (manage.html-style) per node
- **Mesh health tab**: all links sorted worst-first; current asymmetries;
  per-node CAD busy rate (channel-contention proxy).

## 6. Battery telemetry (solar nodes)

Goal: know when a solar node's battery needs a swap — without spending
airtime on it. Reporting is deliberately sparse: **4 floods/day/node**.

**Measurement + empirical calibration (no hardcoded dividers).** Each board
reads VBAT through a different divider on a different pin (RAK4631 base-board
AIN; XIAO `PIN_VBAT` P0.31 with the `VBAT_ENABLE` P0.14 pull-low quirk), and
resistor tolerance makes datasheet ratios approximate anyway. Instead the node
stores one **scale factor calibrated against a real meter**:

- `batt` — print raw ADC, scaled mV, %, and the current scale factor.
- `batt cal <measured_mV>` — operator measures the battery with a multimeter,
  types the reading into the webapp's battery panel; the node samples the ADC
  at that moment, computes `scale = measured / raw`, persists it in LittleFS
  (`/agn_batt.cfg`, alongside the rf/ble configs). Single-point is sufficient:
  a resistive divider is linear through zero.
- Calibration happens at install time with the laptop physically at the node
  (it's the BLE gateway at that moment) — matches the deployment workflow.

**% curve**: firmware maps mV → % via a 1S Li-ion discharge table (3.30 V = 0,
4.20 V = 100, piecewise between). Caveat surfaced in the UI: while the panel
is charging, voltage reads high — the % is most truthful at night. The raw mV
is always reported alongside %, and the webapp trends it; **the swap decision
is really "mV trend at night drifting down"**, which the sparkline shows.

**Distribution — three paths, none chatty:**
1. Heartbeat + `info` gain a `batt=` field (local console, zero airtime).
2. STATUS_REPLY includes battery (on-demand, Phase B).
3. **`PKT_TELEM` flood every ~6 h (jittered)** — ~10 B payload (id, mV, %).
   Every node caches every node's last report with age (a battery directory,
   like the locator directory): connect to any gateway and instantly see
   last-known battery for the whole mesh — `battdump` console verb — without
   querying anyone. 4 tiny floods/node/day is negligible airtime even at SF11.

**Webapp**: battery badge on each marker (% + age), orange below 30 %, red
below 15 % or when the last report is older than ~18 h (two missed reports =
maybe the node died of exactly the thing we're monitoring). Battery panel per
node: live mV/%, calibration box, mV sparkline from cached/polled history.

**Phase C hook**: low battery is the first trigger for automatic management —
a node could self-shed load (longer beacon period, lower TX power) below a
threshold. Out of scope here; noted so the telemetry format carries what
that will need.

## 7. Phasing

| Phase | Scope | Gate |
|---|---|---|
| **A** | Map, placement/persistence, gateway polling (`info`), announce-derived 1-hop topology (`nbrdump`), directional links, asymmetry view; gateway battery panel + calibration (`batt`, `batt cal`) | fw: `nbrdump` + `batt`/`batt cal` verbs |
| **B** | STATUS_QUERY/REPLY for multi-hop telemetry; PKT_TELEM 6-hourly battery flood + `battdump` cache; management actions on the GATEWAY node; route overlay; mesh-health table | fw: STATUS query/reply (read-only) + PKT_TELEM |
| **C** | **Remote control** of non-gateway nodes ("automatic node management"): rf staging, block/unblock, ble settings over the air | Tier-1 signed control packets (Agent.md §4/§6) — writes need authentication; ids alone are non-authenticating |

The UI treats "gateway" vs "via mesh" as a per-node transport tag from day
one, so each phase is a transport/verb addition, not a redesign.

## 8. Firmware work implied by this plan

- **Phase A:** `nbrdump` — print the announce-derived neighbour-of-neighbour
  cache in the standard `nbr` line format (the router already holds the data;
  this is a print loop). `batt` / `batt cal <mV>` — per-board ADC read
  (RAK4631 AIN / XIAO PIN_VBAT+VBAT_ENABLE / promicro), single-point scale
  persisted in `/agn_batt.cfg`, `batt=` field added to heartbeat + `info`.
- **Phase B:** `PKT_STATUS` query/reply on the locator QUERY/REPLY pattern —
  flooded query, unicast compact reply (incl. battery); rendered to the
  console in the same `nbr`/`route` format. Read-only by construction.
  `PKT_TELEM` battery flood every ~6 h jittered + per-node cache + `battdump`.

### Security posture of unauthenticated STATUS (deliberate, bounded)

- **No secrecy is lost**: beacons already broadcast every node's neighbour
  report in the clear ~every 10 s; a passive listener on the right PHY can
  assemble the whole map without transmitting. Authenticating the read path
  would protect already-public information.
- **Amplification is the real exposure** (a query is a remote-triggered
  transmit). Bound: a node answers at most ONE STATUS_REPLY per cooldown
  (~30 s) **regardless of asker** — rate-limit by target, not source, because
  source ids are spoofable. Queries are TTL-bounded floods like REGISTER/
  locator-QUERY, which carry the same exposure today (no new attack class).
- **Spoofed replies** could paint a false map — but forged beacons can poison
  the real routing tables today, which is strictly worse; both are fixed by
  the same future mechanism.
- **End state**: when Tier-1 signing lands, STATUS replies are signed alongside
  control packets. Until then the auth boundary is exactly: *reads are open,
  writes are impossible* — no unauthenticated message may change node state.
- Nice-to-haves, not blockers: `info json` (structured parsing), last-beacon
  RSSI/SNR per neighbour, link-event counters (dups, NACK rounds) for the
  health tab.
