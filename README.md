# LoRa Mesh Backbone — Phase 0 + routing core

App-agnostic LoRa transport. See [`Agent.md`](Agent.md) for the full plan and the
non-negotiable requirements. This repo currently has **Phase 0** (the working-baseline
SX1262 link) plus the **portable routing core** that Phases 2–3 are built from —
link-quality, independent per-direction paths — developed and unit-tested host-side.

What's here, and what is deliberately *not* yet:

| Here now | Not yet (later phases) |
|---|---|
| Interrupt-driven SX1262 HAL (never blocks `loop()`, §2.6) | BLE host link — *borrowed* from the MeshCore fork, never hand-rolled (§9) |
| Draft two-layer packet header (§5) | The MeshCore fork **build** itself (designed in `docs/`, hardware-gated) |
| Beacon carries the routing announce (DV table + neighbour reports) over the air | End-to-end ACK (optional, per §3) |
| Multi-hop DATA forwarding: deliver / forward(`next_hop`) / drop(dup·TTL·no-route) | Controller, crypto, HLR (Phase 4–5) |
| **Directed link-layer unicast**: negotiated 1-byte neighbour aliases (§5) | Hardware validation on real RF (no boards this session) |
| **Hop-by-hop ACK + small retry** (link ARQ) over a non-blocking TX queue | |
| Routing + codec + relay + aliases + ARQ — all host-tested (25 cases) | |
| MeshCore Phase 1 integration design — grounded in the real seam (`docs/`) | |

## Layout

```
platformio.ini                  3 envs: wiscore_rak4631 (real) + compile_check + native (tests)
boards/wiscore_rak4631.json     project-local RAK4631 board definition
variants/wiscore_rak4631/       vendored RAK4631 pin-map variant (RAKwireless BSP)
include/board_config.h          SX1262 wiring + network-wide PHY (the only board-specific knobs)
include/packet.h                on-air frame format (link + network headers)
include/radio_hal.h  src/radio_hal.cpp   non-blocking SX1262 transport
lib/mesh/                       PORTABLE routing core (no Arduino) — builds for nRF52 + host:
    link_metric.*                 RSSI/SNR -> per-direction quality q ∈ [0,1]
    neighbor_table.*              neighbours with independent q_rx / q_tx + asymmetry flag
    routing_table.*               distance-vector, per-direction next hop (Babel-inspired)
    router.*                      ties it together: beacon in, next_hop() out
    announce_codec.*              compact, bounds-checked beacon (de)serialisation
    forwarder.*                   relay decision: deliver / forward / drop + dedup
    link_arq.*                    hop-by-hop ACK + small retry (link reliability)
docs/meshcore-integration.md    Phase 1 fork seam design (grounded in MeshCore source)
src/main.cpp                    prober + routing + forwarding + ARQ + outbound TX queue
test/test_mesh/                 host unit tests: routing logic (Unity)
test/test_codec/                host unit tests: wire codec (Unity)
test/test_forward/              host unit tests: relay decisions (Unity)
test/test_alias/                host unit tests: link-alias negotiation (Unity)
test/test_arq/                  host unit tests: hop-by-hop ACK/retry (Unity)
```

## Build & test

PlatformIO (verified against the `nordicnrf52` platform + Adafruit nRF52 core +
RadioLib 7.x, and host `g++` for the native tests):

```bash
pio run -e wiscore_rak4631    # RAK4631 firmware       -> .pio/build/wiscore_rak4631/firmware.{hex,zip}
pio run -e xiao_nrf52         # Seeed XIAO nRF52840 + Wio-SX1262
pio run -e promicro           # Pro Micro nRF52840 ("faketec") + SX1262
pio run -e compile_check      # host compile-verify on an Adafruit Feather nRF52840 (identical MCU/core)
pio test -e native            # run the routing/codec/relay/arq unit tests (no hardware)
```

All three boards carry an SX1262, so one firmware covers them; per-board pins, TCXO
voltage, RXEN RF-switch and power-enable live in `include/board_config.h` (values from
MeshCore's definitions). **Flashing + bring-up: see
[`docs/hardware-bringup.md`](docs/hardware-bringup.md)** — RAK pair first, then a 3rd
node for multi-hop + ARQ.

`compile_check` exists so the firmware logic builds on a stock nRF52840 toolchain
without the RAK board files. `native` runs `lib/mesh` — which is pure portable C++ —
as host unit tests, so the exact routing logic that ships on the nRF52 is verified
without a radio in hand.

## Routing core — what the tests prove

`pio test -e native` exercises the project's actual novelty (§9, "asymmetric routing
is ours to build"):

- **link metric** — RSSI/SNR maps monotonically into a clamped quality;
- **line topology** A–B–C — a node with no direct link reaches the destination via a relay;
- **asymmetric per-direction (Req 3)** — on a ring strong one way and weak the other,
  the forward path A→C (via B) and the return path C→A (via D) come out **different**,
  exactly as intended — one-way links are used, not discarded;
- **reroute** — when the preferred relay goes silent, traffic shifts to the backup;
- **wire codec** — announces round-trip within quantisation, and malformed/truncated
  buffers off the radio are rejected without overruns;
- **relay decisions** — deliver / forward(+TTL) / drop on dup·TTL·no-route·own;
- **link aliases** — each node assigns distinct 1-byte aliases; after negotiation the
  alias one node uses to address another lands in that node's own alias space, so
  forwarding is directed unicast rather than blind rebroadcast (§5);
- **link ARQ** — a tracked frame clears on ACK, retransmits on timeout, and is given
  up after the retry limit (never resent forever).

On hardware, every directed hop now requests a tiny ACK and retransmits if it's lost
(`[ACK]`/retry visible in the log); all TX paths share a non-blocking outbound queue so
beacons, forwards, ACKs and retransmits never collide mid-air.

The beacon carries each node's announce over the air (`+announce NB` in the TX log),
and DATA packets are delivered / forwarded along `next_hop()` / dropped by the relay
engine. Build one node with `-DAGN_DATA_DEST=0x...` to watch a 3-node line forward end
to end (`[FWD] …` / `[RX] DATA delivered …`).

**Phase 1 (MeshCore fork)** is designed in
[`docs/meshcore-integration.md`](docs/meshcore-integration.md) against MeshCore's real
seam (`onRecvPacket` / `allowPacketForward` / `sendDirect`); the fork build + BLE
coexistence validation are hardware-gated. The routing/codec/forwarding logic it needs
is the host-tested `lib/mesh` here — the fork work is marshalling, not new logic.

## Flash two RAK4631s

Give each node a distinct ID (until pubkey-derived IDs land, §3), then flash over
USB (double-tap reset for the bootloader if needed):

```bash
# Node A
pio run -e wiscore_rak4631 -t upload --upload-port /dev/ttyACM0
# Node B — override the node ID at build time
PLATFORMIO_BUILD_FLAGS="-DAGN_NODE_ID=0x0000000B" pio run -e wiscore_rak4631 -t upload --upload-port /dev/ttyACM1
```

Leaving `AGN_NODE_ID=0` auto-derives a stable per-chip ID from the nRF52 FICR, so
two boards already differ without overriding.

## Expected output

`pio device monitor -e wiscore_rak4631` on each node — every node beacons every
~10 s and prints what it hears:

```
=== LoRa Mesh Backbone — Phase 0 link prober ===
fw=0.0.1-phase0  node=1A2B3C4D
PHY: 904.375 MHz BW250 SF11 CR4/5 sync=0x4D
header=17 bytes (link=4 net=13)
radio up, listening...
[TX] beacon seq=0 from 1A2B3C4D
[RX] beacon  src=0000000B seq=3 up=41s  rssi=-87.0 dBm  snr=9.2 dB  q=0.78  neighbors=1
```

Seeing the other node's `src`, an incrementing `seq`, live `rssi`/`snr`, the derived
quality `q`, and a growing `neighbors` count is the milestone: a proven non-blocking
link feeding the routing core's neighbour table. The next step is to serialise the
announce (neighbour reports + DV table) into the beacon so the per-direction routing
proven in `pio test -e native` runs over the air.

## Hardware notes (verify on first flash)

The SX1262 wiring in `include/board_config.h` follows the RAK4631 reference (NSS 42,
DIO1 47, NRST 38, BUSY 46; dedicated LoRa SPI on SCK 43 / MOSI 44 / MISO 45; RF
switch on DIO2; 3.3 V TCXO on DIO3). The production firmware will inherit this radio
layer from the forked MeshCore BSP (§9) rather than carry a standalone variant — this
Phase 0 sketch vendors the variant so it builds and flashes on its own.
