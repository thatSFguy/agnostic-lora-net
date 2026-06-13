# agnostic-LoRa-Net

[![build](https://github.com/thatSFguy/agnostic-lora-net/actions/workflows/build.yml/badge.svg)](https://github.com/thatSFguy/agnostic-lora-net/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/thatSFguy/agnostic-lora-net)](https://github.com/thatSFguy/agnostic-lora-net/releases/latest)
MIT licensed.

An **app-agnostic LoRa mesh backbone** — a dumb, efficient transport that moves
addressed packets between nodes the way the internet moves IP. Applications ride on
top as **opaque payload** (a Reticulum app, a phone over BLE, anything); the backbone
itself is never programmed per-app. See [`Agent.md`](Agent.md) for the full design and
the non-negotiable requirements.

The novelty (vs. Meshtastic/MeshCore/Reticulum): **link-quality-aware routing with
independent per-direction paths** — asymmetric/one-way links are *used*, not discarded.

## Why not just Meshtastic / MeshCore?

Same radios, same physics — different class of network. All numbers below are
**measured on real hardware**, not projected:

| | Meshtastic / MeshCore | **agnostic-LoRa-Net** |
|---|---|---|
| Encryption | channel PSK (shared key) | **end-to-end Reticulum crypto** — nodes never see plaintext, no shared network key to leak |
| Delivery confirmation | optional hop-level ACK | **cryptographic delivery proof** from the recipient's key |
| Large payloads | short texts only | **images / files**: 14 KB delivered loss-free (SAR + 4-deep queue + transfer-complete ACK; ~210 B/s at SF7) |
| Asymmetric links | discarded / break routing | **routed per-direction** (independent q_rx/q_tx paths) |
| Channel access | random / fixed slots | **CSMA via hardware CAD** (listen-before-talk) + jittered recovery timers — zero fragment loss across a 53-chunk transfer under load |
| Reliability stack | single-shot packets | per-hop ARQ **+** end-to-end NACK repair **+** dedup |
| Addressing | fixed node ids | **identity directory**: register/resolve opaque ≤16 B ids (RNS hashes), push-on-change, mobility-ready |
| PHY management | reflash / app setting | **runtime-retunable** (freq/BW/SF/CR/power, staged+persisted) with a documented network retune safety protocol |
| Overhead | fixed beacon cadence | **beacon period auto-scales with SF** (~constant 0.3 % duty; configurable `net beacon`) |
| Telemetry | app-dependent | battery %, link RSSI/SNR + **SNR margin**, neighbour tables — free in beacons, mapped live |
| Management UI | phone app | **zero-install web apps** (Web Serial / Web Bluetooth): node manager + live mesh map |

The honest trade: our short texts are ~4× bigger on air than Meshtastic's
(~200 B vs ~50 B) — that's the cost of end-to-end encryption and proofs, not of
the mesh. At equal SF, texts deliver in ~1 s and prove in ~2 s.

## Status — proven on real hardware

Validated end-to-end on 2× RAK4631 + a Seeed XIAO nRF52840 (Wio-SX1262), all SX1262:

| Capability | Where |
|---|---|
| Non-blocking SX1262 transport (never blocks `loop()`) | `radio_hal.*` |
| Neighbour discovery + **per-direction** link quality (q_rx / q_tx) | `lib/mesh` |
| Distance-vector routing, announces piggybacked on beacons | `router.*`, `announce_codec.*` |
| Directed link-layer unicast (negotiated 1-byte aliases) | `neighbor_table.*` |
| **Multi-hop forwarding** (deliver / forward / drop) | `forwarder.*` |
| Hop-by-hop **ARQ** (ACK + retry) over a non-blocking TX queue | `link_arq.*` |
| **Reliable file transfer** (SAR fragment/reassembly + CRC + NACK) | `sar.*` |
| Runtime **link blocking** (the Tier-1 "block a bad link" control hook) | `router.block()` |
| **Reticulum** running over the mesh (announce + proven echo) | `reticulum/`, `scripts/rns_*` |
| **BLE + LoRa coexistence (Req 1)** + phone-app ⇄ BLE ⇄ mesh ⇄ BLE ⇄ phone-app | `-DAGN_BLE`, `web/ble.html` |

The routing/codec/relay/alias/ARQ/SAR logic is **portable C++ in `lib/mesh`**, host
unit-tested (**46 cases, `pio test -e native`**) and cross-compiled unchanged onto the
nRF52.

> **BLE note:** the original plan assumed BLE+LoRa coexistence required forking
> MeshCore (because every hand-rolled attempt had failed). On this firmware it **works
> without the fork** — the failures were from *blocking* radio code, and ours is
> rigorously non-blocking. The MeshCore fork is no longer on the critical path.

## Layout

```
platformio.ini            envs: wiscore_rak4631 · xiao_nrf52 · promicro · tracker_t1000_e · heltec_v4 · compile_check · native
boards/ · variants/       project-local board defs + vendored pin-map variants (RAK / XIAO / Pro Micro / T1000-E / Heltec V4)
include/board_config.h    per-board SX1262/LR1110 wiring + network-wide PHY (904.375 MHz, BW250, SF9, 22 dBm, sync 0x4D)
include/fs_compat.h       persistence shim: Adafruit LittleFS (nRF52) / core LittleFS (ESP32)
include/packet.h          on-air frame format (link + network headers)
radio_hal.*               non-blocking SX1262/LR1110 transport (default SPI, per-board TCXO/RXEN/FEM)
lib/mesh/                 PORTABLE core (no Arduino — builds for nRF52 + host):
    link_metric · neighbor_table · routing_table · router      link quality + per-direction DV
    announce_codec · forwarder · link_arq · sar                wire codec · relay · ARQ · file transfer
src/main.cpp              firmware: prober + routing + forwarding + ARQ + SAR + console + tunnel + BLE
test/test_*/              host unit tests (Unity): mesh · codec · forward · alias · arq · sar
docs/hardware-bringup.md  flashing + bring-up runbook
docs/tcp-bridge.md        app-integration guide: TCP bridge + tunnel protocol (distributable)
docs/meshcore-integration.md   Phase-1 fork seam design (now optional — BLE works without it)
reticulum/interfaces/AgnosticLoraInterface.py   Reticulum custom interface (tunnels RNS over the mesh)
scripts/                  host harnesses: sar_test · sar_multihop · tunnel_test · rns_echo · rns_demo
web/ble.html              Web Bluetooth client: phone-app ⇄ BLE ⇄ mesh chat
web/manage.html           Web Serial/BT node manager: BLE PIN · radio config · battery calibration
web/map.html              live mesh map (Leaflet): nodes on real geography, per-direction link
                          quality/asymmetry/SNR margin, battery badges, gateway console
docs/INTEGRATING-AGNOSTIC-LORA-NET.md  third-party integration guide (authored by the mobile-app agent)
docs/node-map-webapp-plan.md           map app + telemetry architecture (gateway model, phases)
docs/remote-config.md     remote node config + the network-wide retune safety protocol (Tier-1)
docs/identity-vs-locator.md  design boundary: mesh routes on node-id locators, apps address on identity hashes
```

## Build & test

PlatformIO (`nordicnrf52` + Adafruit nRF52 core + RadioLib 7.x; host `g++` for tests):

```bash
pio test -e native               # 46 host unit tests for lib/mesh (no hardware)
pio test -e native               # 61 host unit tests for lib/mesh (no hardware)
pio run  -e wiscore_rak4631      # RAK4631 mesh firmware (BLE compiled in, off by default)
pio run  -e xiao_nrf52           # Seeed XIAO nRF52840 + Wio-SX1262 (SoftDevice s140 v7)
pio run  -e promicro             # Pro Micro nRF52840 + SX1262
pio run  -e tracker_t1000_e      # Seeed SenseCAP T1000-E (nRF52840 + Semtech LR1110)
pio run  -e heltec_v4 -t upload  # Heltec WiFi LoRa 32 V4 (ESP32-S3 + SX1262, flash over USB serial)
pio run  -e compile_check        # host compile-verify on a stock Feather nRF52840
```

Most boards carry an SX1262, so one firmware covers them; the **T1000-E** carries a
Semtech **LR1110** instead (the radio HAL selects RadioLib's LR1110 class via
`AGN_RADIO_LR1110`, the only chip-specific seam — RF-switch setup). Per-board pins, TCXO
voltage, RXEN/FEM RF-switch and power-enable live in `include/board_config.h` (values
from MeshCore / Meshtastic). The radio sits on the default `SPI` remapped to the LoRa
pins (`SPI.setPins` on nRF52, `SPI.begin(sck,miso,mosi,ss)` on ESP32) with a
crystal-mode fallback — matching MeshCore's working RAK4631 init.

The **Heltec V4** is the first ESP32 target: no SoftDevice (BLE is left out for now —
its console is over USB serial), persistence on the ESP32 LittleFS
(`include/fs_compat.h`), and the node ID folded from the eFuse MAC. Its SX1262 sits
behind a GC1109/KCT8103L front-end module driven in lockstep with TX/RX; **that FEM/VEXT
control is board-revision-specific — verify on first flash** (see the note in
`include/board_config.h`). It flashes over USB serial (esptool), not the nRF52 UF2/DFU
path used by the hub.

**Flashing + bring-up: [`docs/hardware-bringup.md`](docs/hardware-bringup.md).** Node
IDs auto-derive from each chip's FICR (nRF52) or eFuse MAC (ESP32), so boards differ
without configuration.

## The stack

- **Routing** — each node measures `q ∈ [0,1]` per direction from RSSI/SNR, runs
  Babel-style distance-vector, and forwards toward `next_hop()`. Forward and return
  paths are computed independently, so asymmetric links are first-class.
- **Reliability** — every directed hop requests a 1-byte-sequenced ACK and retransmits
  (`link_arq`); all TX paths share a non-blocking outbound queue so beacons, forwards,
  ACKs and retransmits never collide mid-air.
- **App transport (SAR)** — payloads larger than one frame are fragmented, reassembled
  and CRC-verified; a missing-fragment NACK recovers end-to-end loss. Proven by
  transferring a real image byte-perfect over 1 and 2 hops (`scripts/sar_*`).
- **Runtime console** (USB serial / BLE): `send`/`block`/`unblock`/`info`/`sbegin`+`xfer`+
  `dump`/`tunnel`/`rf`/`ble`. `block` and `rf` (live radio retune: freq/BW/SF/CR/power/
  sync, staged + `rf apply`, persisted) are the local stand-ins for the Tier-1
  controller's signed control commands — see [`docs/remote-config.md`](docs/remote-config.md).

## Reticulum over the mesh

The backbone is a transparent **Reticulum interface** — RNS packets ride as opaque
payload; *our* routing carries them. (Not RNode emulation, which would bypass our
routing.) The node's `tunnel` mode turns USB serial into a binary HDLC pipe carrying
`[node-id][payload]`; `reticulum/interfaces/AgnosticLoraInterface.py` plugs that into
RNS. `scripts/rns_demo.py` runs two isolated RNS instances (no LAN path) bound to two
RAKs and round-trips a cryptographically-proven echo **over the mesh**.

**Building your own app (phone or otherwise)?** See
[`docs/tcp-bridge.md`](docs/tcp-bridge.md) — a distributable guide to the TCP bridge and
the tunnel wire protocol, for both Reticulum apps and custom apps in any language.

## BLE (Req 1)

Every board build includes a SoftDevice BLE Nordic UART Service alongside the mesh
(`-DAGN_BLE` is on for all envs). It's **off by default** and the SoftDevice is enabled
*lazily* on the first `ble on`, so a node that never uses BLE pays no runtime/power cost.
BLE frames are tunnelled into the mesh and deliveries come back out over BLE, so the full
path is **app → BLE → node → LoRa → node → BLE → app** — proven on hardware with two RAKs
(`web/ble.html`, a Web Bluetooth chat). The BLE links stay up through LoRa traffic; that's
Req 1.

**Pairing is PIN-secured.** The UART requires `SECMODE_ENC_WITH_MITM`, BLE is off by
default, and a 6-digit pairing PIN is set per node. Management is **out-of-band over USB**
(you can't configure BLE security over BLE itself) via `web/manage.html` (Web Serial):
**Connect → Enable BLE → read the PIN → pair your phone with it.** Console equivalents:
`ble on|off`, `blepin [random|<6 digits>]`. The PIN is shown **on demand only**
(never in periodic output).

## Commissioning hub & web apps (zero install)

A single-page **commissioning hub** ([`web/index.html`](web/index.html)) flashes,
provisions, and hands off nodes for remote administration. Run it locally
(`localhost` is a secure context, so Web Serial / Web Bluetooth work):

```bash
bash scripts/refresh_web_fw.sh     # build firmware into web/fw/
python3 -m http.server 8000        # then open http://localhost:8000/web/
```
Set the hub's firmware source to `./fw/`. The flow:

1. **Flash** — in-browser nRF52 serial DFU (`web/nrf-dfu.js`, a faithful port of
   adafruit-nrfutil's protocol, verified byte-for-byte) with **UF2 drag-drop fallback**.
2. **Provision** — set the BLE PIN and bind the node to this browser's Ed25519
   **controller key**, so it's ready for signed remote management.
3. **Manage** — the live mesh map ([`web/map.html`](web/map.html)).

Firmware comes from `web/fw/` (built by `refresh_web_fw.sh`) or, for a public deploy,
the latest GitHub release (UF2 + `.dfu.json` are published per board). Web Serial /
Web Bluetooth need a secure context — `localhost` qualifies.

## Web apps (zero install)

Both are single files served from the repo — Web Serial / Web Bluetooth need a
secure context, and `localhost` counts:

```bash
python3 -m http.server 8000
# http://localhost:8000/web/manage.html   — node manager
# http://localhost:8000/web/map.html      — mesh map
```

**manage.html** — connect a node over USB (Web Serial) or BLE (Web Bluetooth):
enable BLE + read the pairing PIN, stage/apply radio settings with the retune
warning, calibrate the battery (enter a multimeter reading — volts or mV — and
the node derives + persists its ADC scale), or drive the raw console.

**map.html** — connect ONE node as the *gateway* (deployed nodes have no USB;
BLE from wherever you stand works). The app polls the gateway's tables plus the
announce-derived view of every neighbour's links — the whole cluster appears
with **zero extra packets on the air**. Place nodes on the map (positions
persist; JSON export/import), then read it: paired directional arrows colored
by link quality, grey-dashed = direction never heard (a one-way link), ⚠ =
asymmetric, click a link for both directions' q/RSSI/SNR **and the SNR margin
at the current SF** — the "how close is this link to the cliff" number. Click a
node for vitals, battery, who it hears / who hears it, and its routes overlaid.

## Bridges: putting other stacks on the mesh

The node's `tunnel` mode turns the console (USB or BLE) into a framed binary
pipe carrying `[typed address][opaque payload]` — that one contract is the
integration surface for everything:

- **Serial (today, proven)** — `reticulum/interfaces/AgnosticLoraInterface.py`
  binds a full RNS stack to a USB node; the mobile app does the same over BLE.
  Wire contract: [`docs/tcp-bridge.md`](docs/tcp-bridge.md), worked examples:
  [`docs/INTEGRATING-AGNOSTIC-LORA-NET.md`](docs/INTEGRATING-AGNOSTIC-LORA-NET.md).
- **TCP bridge (shipped: [`scripts/tcp_bridge.py`](scripts/tcp_bridge.py))** —
  expose a USB node on the LAN: multi-client, serial-reconnect, atomic frame
  writes. Custom apps speak the §4.2 framing over the socket; Reticulum users
  can also run Path A from the doc (an RNS instance bridging
  `TCPServerInterface` ⇄ `AgnosticLoraInterface`).
- **KISS TNC mode (shipped, fw ≥ 0.7.1)** — `kiss <node-id>` (persisted) turns
  the USB console into a standard KISS TNC: RNS's stock `KISSInterface` and the
  wider packet-radio ecosystem work unmodified; the node stays manageable over
  BLE. Honest caveat: KISS carries no destination, so a KISS node pins traffic
  to its configured peer — point-to-point by construction, not a replacement
  for the typed envelope.

## What's left

- **Tier-1 controller** — signed remote control is **live for TX power** (Ed25519, fw
  ≥ 0.8.0): the controller key lives in the map app, gets provisioned per node
  (`ctrlkey`), and commands carry a replay counter; power *decreases* apply
  provisionally and auto-revert in 60 s unless confirmed, so a remote command can never
  strand a node. `block`/route/full-rf-retune over the same signed path are next. The
  network-wide retune safety protocol is in [`docs/remote-config.md`](docs/remote-config.md).
- **Reticulum reliability/UX** — LXMF messaging through Sideband (via a TCP bridge, or
  an RNode-compatible BLE front-end backed by the mesh).
- Polish: pub-key-derived node IDs + signed control plane (§3/§5), FCC handling for the
  1 W class (§8), flash-write minimization for solar nodes (§4 Req 4).
