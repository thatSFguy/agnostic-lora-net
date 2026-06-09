# Phase 1 — MeshCore fork integration design

> Status: **design, not yet built.** The actual fork, flash, and BLE-coexistence
> validation are hardware-gated (they need ≥2 RAK4631s and the operator's phones —
> Agent.md Req 1). This document fixes the integration contract so the build is
> mechanical when boards are in hand. API names below were read from
> `meshcore-dev/MeshCore@main` (`src/Mesh.h`, `src/Dispatcher.h`, `src/Packet.h`);
> re-check them against the pinned fork commit, as they may drift.

## Why a fork (recap, Agent.md §9)

MeshCore's routing↔transport boundary is a **single virtual**,
`Dispatcher::onRecvPacket(Packet*)`, with all radio scheduling, CAD, duty-cycle,
dedup and queueing *below* it and routing-agnostic. We inherit that machinery
(and the BLE host) untouched and replace only the routing. The routing brain is
already built and host-tested in this repo (`lib/mesh`): `Router` (per-direction
DV), `Forwarder` (relay decision), `announce_codec` (wire format). Phase 1 is the
**adapter** that marshals between MeshCore's `Packet` and our `lib/mesh` types.

## The layers, and which side of the seam they're on

```
            ┌─────────────────────────────────────────────┐
  REPLACE   │  AgnosticMesh : public Mesh                  │  ← our routing
            │   onRecvPacket() / allowPacketForward()      │
            └───────────────▲──────────────┬──────────────┘
                            │ Packet*       │ sendFlood/sendDirect
            ┌───────────────┴──────────────▼──────────────┐
  KEEP      │  Dispatcher  (scheduling, CAD, dedup, queue) │
  byte-for- │  Radio  (CustomSX1262 + RadioLib + DIO1 IRQ) │
  byte      │  PacketManager (alloc/queueOutbound/inbound) │
            │  nrf52/SerialBLEInterface  (BLE host)        │
            │  pinned forked nRF52 BSP  ([nrf52_base])     │
            └─────────────────────────────────────────────┘
```

**Keep byte-for-byte (load-bearing for Req 1 — BLE+LoRa coexistence):**
- `Radio` impl — `RadioLibWrappers` / `CustomSX1262*`, the async DIO1 state machine.
  Note this is the *same* non-blocking model we built standalone in `radio_hal.cpp`;
  in the fork we use MeshCore's, not ours.
- `Dispatcher` — `recvRaw`/`startSendRaw`/`isSendComplete`/`onSendFinished`,
  `getEstAirtimeFor`, `packetScore`, `queueOutbound`/`getNextOutbound` (priority).
- `nrf52/SerialBLEInterface` — conn-param / supervision-timeout / advertising-watchdog
  tuning, and the pinned forked nRF52 BSP. **Do not touch.**

**Replace (subclass `Mesh`):** the routing methods only.

## The seam, method by method

MeshCore `Mesh`/`Dispatcher` member → what our adapter does.

| MeshCore hook | Signature (from source) | Adapter behaviour |
|---|---|---|
| `onRecvPacket` | `DispatcherAction onRecvPacket(Packet* pkt)` | Entry point. If `pkt->getPayloadType() == PAYLOAD_TYPE_RAW_CUSTOM`, parse our network header + payload, run the data-plane (below). Return a `DispatcherAction`. Non-RAW_CUSTOM packets → `ACTION_RELEASE` (we don't speak MeshCore's app types). |
| `allowPacketForward` | `virtual bool allowPacketForward(const Packet*)` | Our forward gate. Return true only when `Forwarder::decide()` says `FORWARD` — replaces MeshCore's flood-forward policy. |
| `filterRecvFloodPacket` | `virtual bool filterRecvFloodPacket(Packet*)` | Drop foreign flood traffic we don't route. |
| `getLastRSSI/SNR` (Radio) | `float getLastRSSI()/getLastSNR()` | Sampled at RX → `mesh::quality_from_rf(snr,rssi)` → `Router::on_beacon` (q_rx). |
| `sendFlood` | `void sendFlood(Packet*, …)` | Phase-1 forwarding primitive (controlled rebroadcast + our TTL/dedup). |
| `sendDirect` | `void sendDirect(Packet*, const uint8_t* path, uint8_t path_len, …)` | Phase-2/3 primitive: source-route along the path our `Router` computes. |

### `DispatcherAction` return contract (from `Dispatcher.h`)

```
ACTION_RELEASE              (0)              // free the packet — deliver or drop
ACTION_MANUAL_HOLD         (1)              // keep, we'll manage it
ACTION_RETRANSMIT(pri)     ((1+pri)<<24)    // relay at priority `pri`
ACTION_RETRANSMIT_DELAYED(pri, delay)       // relay after `delay` ms
```

Map our `Forwarder::Decision` straight onto it:

| `Action` | `DispatcherAction` |
|---|---|
| `DELIVER`        | hand payload to app, then `ACTION_RELEASE` |
| `FORWARD`        | `ACTION_RETRANSMIT_DELAYED(pri, jitter)` (priority from link quality / airtime) |
| `DROP_DUP/TTL/NO_ROUTE/OWN` | `ACTION_RELEASE` |

MeshCore already has its own `hasSeen`/`clear` dedup table; we can rely on it and
drop our `Forwarder` seen-cache in the fork, or keep ours for parity — TBD at
integration (keeping ours costs ~600 B RAM and is transport-independent).

## Carrying our protocol inside MeshCore

Everything we send is a `PAYLOAD_TYPE_RAW_CUSTOM` (0x0F) packet — MeshCore treats
its bytes as opaque, which is exactly our "app-agnostic backbone" requirement
(§2.5). Inside that payload we put our own framing:

```
MeshCore Packet (RAW_CUSTOM)
  header / path[] / path_len      ← MeshCore's (used for ROUTE_TYPE_DIRECT later)
  payload[] = [ our NetHeader ][ our payload | announce ]   ← packet.h / announce_codec
```

- **Beacons/announces** → RAW_CUSTOM packets whose payload is our `BeaconPayload` +
  serialised `Announce` (`announce_codec`). Sent via `sendFlood` (single hop).
- **Data** → RAW_CUSTOM packets whose payload is `NetHeader` + opaque app bytes.
  Forwarded per `Forwarder::decide()`.

This keeps our wire format (`packet.h`) intact and independent of MeshCore's
encrypted app types (TXT_MSG, REQ, …), which we never use.

## Forwarding: parity first, then directed (matches Agent.md Phase 1→3)

- **Phase 1 (parity, no regression):** forward by controlled rebroadcast —
  `sendFlood` + our TTL + dedup. Equivalent reach to MeshCore flood, but already
  gated by `allowPacketForward` so we can tighten it next. Milestone: packets cross
  end-to-end, BLE stable, nothing blocking added to `loop()`.
- **Phase 2/3 (directed, per-direction):** once neighbour link aliases exist, build
  a `ROUTE_TYPE_DIRECT` path from `Router::next_hop()` and use `sendDirect`. Forward
  and return paths are computed independently — the asymmetric routing already proven
  in `test/test_mesh`.

## BLE coexistence — the non-negotiable (Req 1)

The reason we fork instead of hand-rolling. Our routing code must never threaten the
BLE link, so:
- **No blocking in the routing path.** `Router`, `Forwarder`, and the codecs are pure
  compute — no SPI, no flash, no delays. Verified by construction (host-tested, no I/O).
- **No new flash writes on the hot path** (Req 4): routing tables stay in RAM and
  rebuild from announces. Nothing in `lib/mesh` persists.
- **Keep the SoftDevice-safe critical sections and the pinned BSP** that make BLE
  timing hold. Untouched, below the seam.

**Acceptance (from §7 Phase 0 / Req 1):** with the adapter in, a RAK4631 holds a
stable BLE link to *both* the Samsung A42 and the Pixel 9 Pro XL through sustained
LoRa TX/RX. This is the gate that can only be checked on hardware.

## Concrete build steps (when boards are in hand)

1. Fork `meshcore-dev/MeshCore`; pin the commit + the forked nRF52 BSP in
   `[nrf52_base]`. Build `RAK_4631_companion_radio_ble` **unchanged**; confirm BLE
   holds on both phones under LoRa load (the Phase 0 milestone, our fork baseline).
2. Add `AgnosticMesh : public Mesh` overriding `onRecvPacket` + `allowPacketForward`,
   linking `lib/mesh` (it already cross-compiles for nRF52 — see the `wiscore_rak4631`
   env here). Drop in the `Packet ↔ PacketRef/Announce` marshalling shim.
3. Reproduce flood via our `Forwarder` (parity). Verify end-to-end + BLE stable +
   non-blocking `loop()`.
4. Enable link-quality DV (`Router`) and, once link aliases land, `sendDirect` along
   `next_hop()`. Verify multi-hop and independent per-direction paths on real RF.

## What this repo already provides for the adapter

| Need at the seam | Provided by |
|---|---|
| "What's the next hop to D?" | `mesh::Router::next_hop(dst)` |
| "Deliver / forward / drop this?" | `mesh::Forwarder::decide(PacketRef)` |
| Link quality from RSSI/SNR | `mesh::quality_from_rf` + `Router::on_beacon` |
| Announce on/off the air | `mesh::announce_serialize` / `announce_deserialize` |
| Our wire header | `packet.h` (`NetHeader`, `PKT_*`) |

All of the above is unit-tested host-side (`pio test -e native`, 16 cases). The fork
work is marshalling and the hardware-gated BLE/RF validation — no new routing logic.
