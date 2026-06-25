# Mobile-app test procedure (Reticulum over BLE → agnostic-LoRa-Net)

How to bring up and validate a phone app that rides the mesh over **BLE**. The phone runs
a full RNS stack and connects to a node's Nordic UART Service (NUS); the node tunnels its
opaque RNS packets into the LoRa mesh. This is the BLE equivalent of the proven desktop
path (`scripts/rns_demo.py`, validated on hardware) — same wire contract, BLE instead of
USB.

Read alongside: [`tcp-bridge.md`](tcp-bridge.md) (wire contract), [`remote-config.md`]
(remote-config.md) (BLE PIN), and [`identity-vs-locator.md`](identity-vs-locator.md) (addressing).

---

## 0. What the phone transport must implement (the contract)

The phone's BLE transport is a port of `reticulum/interfaces/AgnosticLoraInterface.py`.
These are non-negotiable — each was learned the hard way validating the desktop interface:

1. **Typed, length-prefixed envelope** inside each HDLC frame:
   `[u8 addr_type=0x01 LOCATOR][u8 addr_len=4][u32 node_id LE][payload]`. Read `addr_len`,
   don't assume 4 (it becomes 16 when node ids widen). (`tcp-bridge.md` §2.3.)
2. **HDLC framing** over the NUS: `FLAG=0x7E ESC=0x7D mask 0x20`. **Chunk BLE writes to
   ≤20 bytes** (ATT MTU); the node reassembles.
3. **Demux text vs frames** on the NUS receive side: bytes inside `0x7E` frames are tunnel
   payload; plain text lines (`loc …`, `registered …`, heartbeat) are the node's console.
   The node demuxes the same way, so you can send text commands and HDLC frames on one link.
4. **Buffer-and-flush, never drop, while the peer is unresolved.** ← THE bug that cost us
   a day. RNS emits its destination announce immediately on startup; if you drop it while
   still resolving the peer locator, the other side never learns a path (`no-path`). Queue
   outgoing (bounded) and flush on resolve.
5. **Addressing** — pick one:
   - *static*: stamp a fixed uplink node id (simplest; fine for one gateway). ⚠ The uplink is
     the node that **terminates your RNS path** — *not* necessarily the node you're BLE-attached
     to. Auto-filling it from the attached node's `AgnLoRa-<id>` name is correct only when that
     node is your gateway; if the RNS peer lives behind a *different* node, stamp **that** node's
     id. A wrong uplink looks like success: healthy BLE link, outbound frames sent, but **zero
     return traffic** ever arrives. (This is the exact shape of a real mobile-app bring-up bug.)
   - *identity*: send `register <my-id-hex>` and `resolve <peer-id-hex>` as text lines;
     parse the `loc <id> <node>` reply to learn the peer's node id (tracks mobility).
     Since fw 0.4.3 the node also **pushes** `loc <id> <node>` lines *unsolicited* when it
     learns a new/moved binding from the mesh (a peer registering anywhere in the network)
     — same line format, so one parser handles resolve replies AND peer discovery. No
     dirdump polling needed: register on connect, treat every `loc` line whose id isn't
     yours as a discovered peer, resolve nothing extra, unicast your RNS announce to that
     node. (Ids are opaque to the firmware, ≤16 bytes — use your 16-byte RNS destination
     hash as 32 hex chars. Registrations are RAM-only: re-`register` on every connect;
     the node re-floods on its own every ~240 s, TTL 600 s.)
6. **One writer lock** — the directory/resolve loop and RNS both write the BLE characteristic;
   serialize writes so a text command never interleaves mid-HDLC-frame.
7. **Android 13+ (API 33): override the 3-arg notification callback.** On API 33+ the framework
   delivers GATT notifications via `onCharacteristicChanged(gatt, char, value)`; the deprecated
   2-arg overload receives a **null** `characteristic.value`. A client that overrides only the
   old form therefore gets **zero inbound** on a perfectly healthy link — connects, MTU and CCCD
   succeed, sends fine, receives nothing. Override the 3-arg form and read `value` directly (keep
   the 2-arg for API < 33). Confirmed on a Galaxy A42 (Android 13).
8. **Request `CONNECTION_PRIORITY_HIGH` right after connect.** BLE's default connection interval
   lets some centrals — notably Samsung (e.g. the A42 acceptance device) — let the link lapse a
   few seconds after each LoRa TX: you'll see GATT `status=133`, CCCD-write failures, and a
   reconnect storm ("connection not solid"). HIGH priority keeps the interval tight enough to
   ride through mesh traffic; on the A42 it turned a constantly-dropping link into a stable one.

Reference for all of the above: the desktop `AgnosticLoraInterface.py` (swap serial for a
BLE-NUS client; the framing/buffering/resolve logic is identical).

---

## 1. Hardware prep (once)

1. Flash the node(s) the phone(s) will attach to: `agn-rak.uf2` / `agn-xiao.uf2`.
2. Enable BLE + set a PIN — `web/manage.html` over USB (Connect → Enable BLE → note PIN),
   or console `ble on` + `blepin <6 digits>`.
3. Pair the phone with the node in the OS Bluetooth settings using that PIN. The node
   advertises as `AgnLoRa-<nodeid>`.
4. Confirm the mesh is up: each node's heartbeat shows `nbrs>=1 routes>=…`.

> Tip: a fresh **power-cycle** clears leftover directory registrations (they re-flood every
> ~240 s and add channel contention). Start RNS tests from a clean boot.

---

## 2. Tests (run in order — each builds on the last)

### Test 1 — BLE link survives LoRa traffic (Req 1)
Connect the phone over BLE. Drive mesh traffic (another node sending, or `send`). Watch the
node heartbeat: `[ble] … connected=1` stays up *through* `[TX]` lines.
**Pass:** BLE stays connected during sustained LoRa TX/RX. (Acceptance: A42 *and* Pixel 9XL.)

### Test 2 — Tunnel framing round-trip
Use `web/ble.html` (the reference Web-Bluetooth chat) from a laptop as a known-good peer,
or the app's own loopback: send a typed-envelope frame addressed to a reachable node; the
mesh delivers it and a frame comes back out over BLE.
**Pass:** bytes round-trip byte-for-byte; the `← from <node>` line shows the right source.
*(This is the path we proved on hardware end to end with two RAKs.)*

### Test 3 — Directory register/resolve over BLE (if using identity addressing)
From the app (or by typing in a BLE terminal), send `register AABBCCDD\n`, then
`resolve AABBCCDD\n`.
**Pass:** node replies `registered 4-byte id at <node>` then `loc AABBCCDD <node>`. This
validates the phone's text/HDLC demux and the resolve parse. (Cache miss → the node floods
a QUERY; an answer may take a few seconds.)

### Test 4 — RNS over BLE: phone ↔ desktop peer (bootstrap)
Easiest first RNS test — one phone against the proven desktop setup:
1. On a USB-attached node, run an RNS echo **server** (adapt `scripts/rns_echo.py` /
   `rns_demo.py`; for identity addressing give it `identity`/`peer_identity`).
2. Configure the phone's RNS with the BLE transport: register the phone's identity, resolve
   the server's (or static-pin the server's node id), buffer-until-resolved.
3. From the phone, probe/echo the server's RNS destination.
**Pass:** `DELIVERED` — the echo round-trips with cryptographic proof.
**Expect:** RNS path setup is **slow (~117 s on this PHY)** — show progress, be patient; give
it ≥300 s before calling failure.

### Test 5 — Phone ↔ phone over the mesh
Two phones, each paired to its own node. Each registers its identity, resolves the other's.
Send an LXMF message / packet phone-A → phone-B.
**Pass:** the message arrives at phone B; a reply round-trips.

---

## 3. Troubleshooting (symptoms we actually hit)

| Symptom | Likely cause | Fix |
|---|---|---|
| `no-path`, never establishes | outgoing **dropped** while peer unresolved (announce lost) | buffer-and-flush (§0.4) |
| Resolve never returns | text not reaching the node, or parse mismatch | confirm the node echoes `loc …`; match hex case; ensure newline-terminated |
| App sends but peer gets nothing | BLE writes not chunked to ≤20 B, or no writer lock (interleaving) | chunk writes; serialize (§0.2, §0.6) |
| Delivered packets don't reach the phone | a second BLE client was connected; `host_write` prefers the connected client | one BLE client per node |
| **Healthy link, zero inbound** (not even heartbeat); outbound frames send fine | (a) uplink points at the *attached* node, not the peer; (b) no peer is transmitting in the window; (c) a second BLE client is taking the node's output | point the uplink at the real peer (§0.5); confirm a peer is announcing; one BLE client per node |
| BLE link drops ~5 s after each TX; `status=133` / CCCD-write fails; reconnect storm | central (esp. Samsung) let the connection interval lapse | request `CONNECTION_PRIORITY_HIGH` after connect (§0.8) |
| Connects but no announces/contacts **on Android 13+** | only the deprecated 2-arg notification callback overridden → null value, inbound silently dropped | override the 3-arg `onCharacteristicChanged` (§0.7) |
| Works on bench, flaky in field | channel contention / lossy SF11; one big announce lost | power-cycle to clear stale regs; retry; this is inherent RF, not a logic bug |
| BLE won't pair | node not advertising / wrong PIN | `ble on`; re-read PIN (`blepin`, heartbeat, or `web/manage.html`) |

---

## 4. What's proven vs. what the app adds

- **Proven on hardware (desktop interface):** typed envelope, BLE↔mesh tunnel, directory
  register/resolve, identity-addressed RNS echo `DELIVERED` over the mesh with no hub.
- **The app's job:** the same transport logic on the phone (BLE-NUS instead of serial) +
  the RNS app UX (LXMF/Sideband-style). The transport contract is fixed (§0); if the app
  matches it, it inherits the validated path.
- **Not yet built:** per-packet destination resolution (the interface currently maps one
  `peer_identity` per link). Multi-peer-without-a-hub is a later step (Phase 4b).
