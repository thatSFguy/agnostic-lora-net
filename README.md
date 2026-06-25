# agnostic-LoRa-Net

[![build](https://github.com/thatSFguy/agnostic-lora-net/actions/workflows/build.yml/badge.svg)](https://github.com/thatSFguy/agnostic-lora-net/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/thatSFguy/agnostic-lora-net)](https://github.com/thatSFguy/agnostic-lora-net/releases/latest)
[![status: alpha](https://img.shields.io/badge/status-alpha-orange)](#project-status--alpha)
MIT licensed.

> ⚠️ **Alpha.** This is an early, single-maintainer project under active development.
> It works — everything below is measured on real hardware — but APIs, the wire
> format, and the console/controller interfaces can change without notice, and there
> is **no stability or support guarantee**. Treat it as a research/hacking platform,
> not a product. New features and new-board requests are **not guaranteed** to be
> picked up. **PRs are very welcome** — see [Contributing](#contributing).

An **app-agnostic LoRa mesh backbone** — a dumb, efficient transport that moves
addressed packets between nodes the way the internet moves IP. Applications ride on
top as **opaque payload** (a Reticulum app, a phone over BLE, anything); the backbone
itself is never programmed per-app.

The novelty (vs. Meshtastic/MeshCore/Reticulum): **link-quality-aware routing with
independent per-direction paths** — asymmetric/one-way links are *used*, not discarded.

## Why not just Meshtastic / MeshCore?

Same radios, same physics — different class of network. All numbers below are
**measured on real hardware**, not projected:

| | Meshtastic / MeshCore | **agnostic-LoRa-Net** |
|---|---|---|
| Encryption | AES channel keys; public-key DMs | **end-to-end Reticulum crypto** — opaque to the transport; the backbone never sees plaintext |
| Delivery confirmation | optional, non-cryptographic ACK | **cryptographic delivery proof** from the recipient's key |
| Large payloads | short messages; no file transfer | **images / files**: 14 KB delivered loss-free (SAR + 4-deep queue + transfer-complete ACK; ~210 B/s at SF7) |
| Routing | managed flooding — every node rebroadcasts (tolerates asymmetric links, but no link-quality routing) | **link-quality distance-vector**, independent per-direction paths (q_rx / q_tx) |
| Channel access | carrier-sense + randomized backoff | **CSMA via hardware CAD** (listen-before-talk) + jittered recovery timers — zero fragment loss across a 53-chunk transfer under load |
| Reliability stack | flood + optional ACK/retry | per-hop ARQ **+** end-to-end NACK repair **+** dedup |
| Addressing | device-derived node ids (+ public keys) | **self-certifying identity**: 16 B node id = `blake2b(node's Ed25519 pubkey)`, signed announces prove the id↔key binding; register/resolve directory, mobility-ready |
| PHY management | app/config setting | **runtime-retunable** (freq/BW/SF/CR/power, staged+persisted) with a documented network retune safety protocol |
| Overhead | configurable broadcast intervals | **beacon period auto-scales with SF** (~constant 0.3 % duty; configurable `net beacon`) |
| Telemetry | built-in device telemetry; no per-neighbour link metrics | battery %, **per-neighbour RSSI/SNR + SNR margin**, neighbour tables — free in beacons + on-demand status; mapped live |
| RF power | fixed / manual | **autonomous closed-loop optimiser** (Tier-1 controller): signed, mesh-wide, tunes each node against its weakest link; mobility-aware (raise fast / trim slow); self-heals via on-device auto-revert |
| Management UI | phone app | **one zero-install control plane** (`agnctl` dashboard): live map + decision feed + node config + in-browser firmware flashing, all over Web Serial / Web Bluetooth |

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
| **BLE + LoRa coexistence (Req 1)** + phone-app ⇄ BLE ⇄ mesh ⇄ BLE ⇄ phone-app | `-DAGN_BLE`, `web/chat-demo.html` |
| **Signed control plane** — Ed25519 POWER/CONFIRM/BLOCK/UNBLOCK, replay-countered, auto-revert rails | `lib/mesh/control.*`, `controller/internal/sign` |
| **Autonomous RF optimiser** — mesh-wide, weakest-link, mobility-aware (Tier-1 controller) | `controller/internal/policy` |
| **Consolidated web control plane** — live map · decision feed · node Configure · in-browser Flash | `controller/` (`agnctl`, served at `:8080`) |

The routing/codec/relay/alias/ARQ/SAR logic is **portable C++ in `lib/mesh`**, host
unit-tested (**77 cases, `pio test -e native`**) and cross-compiled unchanged onto the
nRF52. The Tier-1 controller is **Go** (`controller/`, stdlib-only), with its own host
tests (`go test ./...`). Current firmware: **v0.14.0**.

> **BLE note:** the original plan assumed BLE+LoRa coexistence required forking
> MeshCore (because every hand-rolled attempt had failed). On this firmware it **works
> without the fork** — the failures were from *blocking* radio code, and ours is
> rigorously non-blocking. The MeshCore fork is no longer on the critical path.

## Layout

```
platformio.ini            envs: wiscore_rak4631 · xiao_nrf52 · promicro · tracker_t1000_e · heltec_v4 · compile_check · native
boards/ · variants/       project-local board defs + vendored pin-map variants (RAK / XIAO / Pro Micro / T1000-E / Heltec V4)
include/board_config.h    per-board SX1262/LR1110 wiring + network-wide PHY (906.625 MHz, BW250, SF9, 22 dBm, sync 0x4D)
include/fs_compat.h       persistence shim: Adafruit LittleFS (nRF52) / core LittleFS (ESP32)
include/packet.h          on-air frame format (link + network headers)
radio_hal.*               non-blocking SX1262/LR1110 transport (default SPI, per-board TCXO/RXEN/FEM)
lib/mesh/                 PORTABLE core (no Arduino — builds for nRF52 + host):
    link_metric · neighbor_table · routing_table · router      link quality + per-direction DV
    announce_codec · forwarder · link_arq · sar                wire codec · relay · ARQ · file transfer
src/main.cpp              firmware: prober + routing + forwarding + ARQ + SAR + console + tunnel + BLE + signed control + mobile flag
lib/mesh/control.*        Ed25519 signed-control codec (POWER/CONFIRM/BLOCK/UNBLOCK), shared with the controller
lib/mesh/telemetry.*      battery + status query/reply codec (per-neighbour q/RSSI/SNR + mobile flag, fw 0.11.0)
controller/               Tier-1 controller (Go, stdlib-only): `agnctl` — console ingest → live topology,
                          airtime capture, signed control, autonomous power optimiser, served web dashboard
test/test_*/              host unit tests (Unity): mesh · codec · forward · alias · arq · sar · control · telemetry · …
docs/hardware-bringup.md  flashing + bring-up runbook
docs/tcp-bridge.md        app-integration guide: TCP bridge + tunnel protocol (distributable)
controller/README.md      Tier-1 controller: flags, margins, step, cadence, key custody
reticulum/interfaces/AgnosticLoraInterface.py   Reticulum custom interface (tunnels RNS over the mesh)
scripts/                  host harnesses: sar_test · sar_multihop · tunnel_test · rns_echo · rns_demo
web/chat-demo.html        Web Bluetooth clear-text chat demo: phone/laptop ⇄ BLE ⇄ mesh
web/manage.html           Web Serial/BT node manager: BLE PIN · radio config · battery calibration
web/map.html              live mesh map (Leaflet): nodes on real geography, per-direction link
                          quality/asymmetry/SNR margin, battery badges, gateway console
docs/INTEGRATING-AGNOSTIC-LORA-NET.md  third-party integration guide
docs/remote-config.md     remote node config + the network-wide retune safety protocol (Tier-1)
docs/identity-vs-locator.md  design boundary: mesh routes on node-id locators, apps address on identity hashes
```

## Build & test

PlatformIO (`nordicnrf52` + Adafruit nRF52 core + RadioLib 7.x; host `g++` for tests):

```bash
pio test -e native               # 77 host unit tests for lib/mesh (no hardware)
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

## Control plane — the `agnctl` Tier-1 controller

`controller/` is the **Tier-1 controller** (Go, stdlib-only — builds offline; `crypto/ed25519`
for signing). It tethers to one node over USB, reads the console stream into a **live global
topology**, and — with a controller key — signs and pushes control commands into the mesh. It
stays **optional**: kill it and the Tier-0 mesh keeps running, and every connectivity-reducing
command has an on-device auto-revert, so a controller crash self-heals.

```bash
cd controller
go run ./cmd/agnctl -port /dev/ttyACM0 -optimize -apply -http :8080   # live: optimise + serve dashboard
go run ./cmd/agnctl -file testdata/session.log -optimize -http :8080  # replay a log, no hardware
```

- **Autonomous RF power optimisation (mesh-wide).** A node transmits at one power, so the binding
  constraint is the **weakest outbound link it must keep** — the optimiser tunes each node against
  that, not just its link to the gateway. The tethered gateway's own links carry measured SNR;
  remote links arrive as routed **telemetry** (`status <id>`), which since **fw 0.11.0** carries
  per-neighbour SNR/RSSI so remote links are measured too (older/quality-only links can be raised
  but never trimmed — a quality estimate saturates). Every change is step-limited; a *decrease*
  applies provisionally and the controller only **CONFIRM**s it after re-observing the node, else
  the node's 60 s dead-man revert restores it.
- **Mobile vs fixed nodes.** A node self-reports mobility (`mobile on|off`, persisted, surfaced in
  telemetry). **Fixed** nodes get optimised straight down to a target margin; **mobile** nodes get
  a higher **reserve band** and an **asymmetric** loop — raise fast when the link weakens, trim
  slowly and only after the margin stays strong — a slow analogue of cellular closed-loop power
  control suited to the mesh's ~15 s feedback. (🚗 = mobile, 📍 = fixed in the UI.)
- **Signed control** (`lib/mesh/control` ↔ `controller/internal/sign`, byte-identical, gold-tested):
  POWER / CONFIRM / BLOCK / UNBLOCK, Ed25519-signed with a monotonic replay counter; flooded to the
  target and ACKed back through the mesh. ROUTE override and remote PHY retune are still TODO.
- **Resilient + self-contained.** The serial link auto-reconnects across node reboots/USB
  re-enumeration (the dashboard stays up through gateway blips). The controller also **serves the
  firmware** it flashes (`/fw/`, default `-fwdir ../web/fw`) — so it works fully offline, no
  external release fetch needed.

**The dashboard** (`-http :8080`, one consolidated single-page app — `localhost` is a secure
context, so Web Serial / Web Bluetooth work):

- **Dashboard** — per-node power/margin/battery/**firmware**, decision feed, gateway console, fixed/mobile.
- **Map** — per-direction link quality (colour + width + value labels), direction arrows, asymmetry
  badges, the optimiser's decision under each node, neighbour count, battery, 🚗/📍 — click a node to
  focus its links and see *why* the optimiser chose a power.
- **Configure** — connect a locally-attached node over Web Serial/BLE: radio PHY (with retune
  warning), battery calibration, BLE pairing/PIN, the mobile flag, raw console. (A port of
  `web/manage.html` into the control plane.)
- **Flash** — in-browser nRF52 serial DFU: pick a board, ① reboot to bootloader, ② select the
  re-enumerated bootloader port & flash, with a UF2 fallback. (A port of the standalone hub.)

See [`controller/README.md`](controller/README.md) for flags (margins, step, cadence, key custody) and scope.

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
(`web/chat-demo.html`, a Web Bluetooth chat). The BLE links stay up through LoRa traffic; that's
Req 1.

**Pairing is PIN-secured.** The UART requires `SECMODE_ENC_WITH_MITM`, BLE is off by
default, and a 6-digit pairing PIN is set per node. Management is **out-of-band over USB**
(you can't configure BLE security over BLE itself) via `web/manage.html` (Web Serial):
**Connect → Enable BLE → read the PIN → pair your phone with it.** Console equivalents:
`ble on|off`, `blepin [random|<6 digits>]`. The PIN is shown **on demand only**
(never in periodic output).

## Standalone web apps (zero install, no controller needed)

When a controller is running, flashing/config/map all live in the **`agnctl` dashboard**
(above) — that's the primary path. The standalone single-file web apps remain for
**offline / no-controller** use (e.g. bench-flashing before a controller exists). Web Serial /
Web Bluetooth need a secure context — `localhost` and HTTPS both qualify.

The hub is also published to **GitHub Pages** at
<https://thatsfguy.github.io/agnostic-lora-net/> (CI stages the latest firmware into `./fw/`
same-origin, so the in-browser flasher works straight from the hosted page). To run it
locally instead:

```bash
bash scripts/refresh_web_fw.sh     # build firmware for all boards into web/fw/
python3 -m http.server 8000        # then open http://localhost:8000/web/
```

- **`web/index.html`** — landing page: a simple menu that routes to the apps below (served at
  the GitHub Pages site root).
- **`web/flash.html`** — commissioning hub, two tabs: **Flash & Provision** (in-browser nRF52
  serial DFU via `web/nrf-dfu.js`, a byte-faithful port of adafruit-nrfutil, with UF2 fallback,
  plus BLE-PIN + controller-key provisioning) and **Manage** (the full node manager below,
  embedded). The firmware source defaults to the local `./fw/`; the GitHub release URL is an
  optional fallback.
- **`web/manage.html`** — node manager: radio PHY (with retune warning), battery calibration,
  BLE pairing/PIN, the mobile flag, raw console. Works standalone and is embedded as the
  flasher's **Manage** tab.
- **`web/map.html`** — gateway-centric mesh map (Leaflet, real geography): per-direction link
  quality/asymmetry/SNR-margin, battery badges, gateway console.
- **`web/chat-demo.html`** — Web Bluetooth **clear-text chat demo**: flash a couple of nodes,
  connect a browser tab to each over BLE, and message across the mesh to confirm it works
  (unencrypted — a bring-up/test tool, not a secure messenger).

(`nrf-dfu.js` is vendored into `controller/internal/httpd/` so the dashboard's Flash tab serves
it too — keep the two copies in sync.)

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

## Project status — Alpha

This is **alpha software**, built and maintained by one person in the open. What that
means in practice:

- **It runs, and the numbers are real** — the capabilities and measurements above are
  validated on actual hardware, not aspirational. But coverage is bench-scale (a handful
  of nodes), not field-hardened at fleet scale.
- **Nothing is frozen.** The on-air frame format, console grammar, controller protocol,
  and web/controller APIs can all change between commits. There are no compatibility
  promises across versions yet — expect to reflash the whole mesh together.
- **No roadmap commitments.** Feature requests and new-board requests are appreciated as
  signal, but may not be implemented — see [What's left](#whats-left) for where effort is
  actually going. If you need a board or feature now, the fastest path is a PR.
- **No support guarantee.** Best-effort only. File issues for bugs by all means, but
  there's no SLA.

## Contributing

**PRs are very welcome** — this is exactly the kind of project that gets better with more
hands and more radios.

- **Good first contributions:** new board support (add a variant under `variants/` + a
  `board_config.h` block + a `platformio.ini` env — the radio HAL is the only chip-specific
  seam), docs fixes, and host-test coverage in `test/`.
- **Keep the core portable.** The mesh logic in `lib/mesh/` builds for both the host and
  the MCU with **no Arduino dependencies** — please keep it that way so the unit tests stay
  meaningful.
- **Run the tests before you push:** `pio test -e native` (firmware core) and
  `go test ./...` in `controller/` (Go control plane). CI runs the build matrix in
  [`.github/workflows/build.yml`](.github/workflows/build.yml).
- **New boards you can't fully bench-test:** that's fine — say so in the PR. Several
  in-tree targets are compile-clean but flagged as not-yet-bench-validated (FEM/RF-switch
  details in particular); call out what you did and didn't verify.

No formal CLA or contribution process yet — open an issue to discuss anything substantial,
or just send the PR. Be kind; assume good faith.

## What's left

- **Tier-1 controller — shipped & ongoing.** Signed POWER/CONFIRM/**BLOCK/UNBLOCK**, the
  mesh-wide autonomous power optimiser, mobility-aware control, the consolidated dashboard,
  and resilient serial are all **live** (see the control-plane section above). Still TODO:
  signed **ROUTE override** and **remote PHY retune** over the same path; auto-block of
  pathological links; transfer boost; controller-key **rotation / re-key**. The
  network-wide retune safety protocol is in [`docs/remote-config.md`](docs/remote-config.md).
- **Energy** — nodes currently run continuous RX with the MCU spinning; the highest-value
  power win is to **light-sleep the nRF52 between radio interrupts** (DIO1 already wakes it),
  plus a deep-sleep role for leaf/tracker nodes. Not yet implemented.
- **Reticulum reliability/UX** — LXMF messaging through Sideband (via a TCP bridge, or
  an RNode-compatible BLE front-end backed by the mesh).
- Polish: FCC dwell-time handling for the 906.625 MHz fixed channel. (Flash wear is
  already a non-issue: writes are config-only — save-if-dirty — plus the signed-control
  replay counter on each accepted command, so ~0 writes/day with no controller and ~12 on
  a node the optimiser actively holds down, comfortably inside the nRF52840's endurance with
  LittleFS wear-levelling. A held node could drop near zero by skipping the no-op heartbeat
  re-assert persists, but it isn't worth the complexity yet.)
