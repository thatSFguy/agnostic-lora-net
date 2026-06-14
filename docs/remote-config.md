# Remote node configuration & the network-wide retune protocol

This note specifies how node configuration — especially the radio PHY (frequency,
bandwidth, spreading factor, coding rate, sync word, TX power, preamble) — is changed
**locally today** and **remotely over LoRa** once the Tier‑1 central controller exists.
It also defines the **safety protocol** the central node MUST follow for a network-wide
retune, because a careless PHY change can strand nodes.

> Status (2026-06): the **local** path (USB / BLE console, plus the `agnctl` dashboard's
> Configure tab) is implemented and persisted. The **signed remote** path now ships for
> POWER / CONFIRM / BLOCK / UNBLOCK (Ed25519, replay-countered, byte-identical Go signer
> in `controller/internal/sign` ↔ firmware `lib/mesh/control`; auto-revert rails). Signed
> **ROUTE override and remote PHY retune** are still TODO. The network-wide retune safety
> protocol below still applies to whoever drives the change (today: local/Configure tab).

---

## 1. What is configurable

| Setting | Console | Network-wide? | Notes |
|---|---|---|---|
| Frequency (`freq_hz`) | `rf freq <hz>` | **yes — critical** | all nodes must match |
| Bandwidth (`bw_hz`) | `rf bw <kHz>` | **yes — critical** | all nodes must match |
| Spreading factor (`sf`) | `rf sf <5..12>` | **yes — critical** | all nodes must match |
| Coding rate (`cr`) | `rf cr <5..8>` | yes | 4/5 … 4/8 |
| Sync word (`sync`) | `rf sync <0xNN>` | yes | network “colour” |
| Preamble (`preamble`) | `rf preamble <syms>` | yes | RX must match |
| TX power (`power_dbm`) | `rf power <-9..22>` | **no** | per-node, safe to change alone |
| BLE enable / PIN | `ble on\|off`, `blepin …` | no | per-node |

“Critical” = changing it on one node **without** changing it on the others makes that
node deaf to the network. TX power is the one PHY knob that is always safe to tune
independently.

Edits are **staged** (`rf <field> <val>`) and committed atomically with `rf apply`,
which reconfigures the SX1262 and persists the result (save-if-dirty, Agent.md Req 4).
`rf revert` discards staged edits; `rf default` stages the compile-time network
defaults. `rf show` prints the active config in a machine-parseable line:

```
[rf] freq_hz=904375000 bw_hz=250000 sf=11 cr=5 power_dbm=14 sync=0x4D preamble=16 (active)
```

`web/manage.html` reads that line on connect to pre-fill the form with the node's
current settings, and surfaces TX power as a dedicated slider.

---

## 2. Local configuration (today)

Two out-of-band transports, identical command set:

- **USB serial** — `web/manage.html` (Web Serial) or any terminal at 115200.
- **BLE** — pair (PIN), then the *same* console over the Nordic UART Service. The NUS
  carries two channels on one characteristic, told apart with no ambiguity: bytes inside
  `0x7E`-delimited HDLC frames are the mesh **tunnel**; plain text lines ending in `\n`
  are **console commands**, and responses come back over BLE. `web/manage.html` can
  connect over BLE (Web Bluetooth) as well as USB. *(BLE cannot change its own security,
  so `ble`/`blepin` still require USB.)*

Local config is the ground truth and the recovery path: a node you can physically reach
can always be fixed over USB regardless of its radio state — and now over BLE too, which
is what makes the §4 field-fix step real.

---

## 3. Authenticated remote configuration (Tier‑1, over LoRa)

The central controller changes settings on a remote node by sending a **signed config
control packet** as opaque mesh payload addressed to that node:

```
ConfigCmd = { seq, target_node_id, op, params… }   // op ∈ {rf_set, rf_apply, ble_on(pin), ble_off, …}
Frame     = ConfigCmd || signature
```

Requirements:

1. **Authentication.** The node verifies `signature` against the **central node's public
   key**, provisioned at flash time (ties into the pub‑key node IDs of §3/§5). Only a
   correctly signed command is acted on; everything else is ignored. (Interim before PKI:
   an HMAC over a pre-shared key may stand in, but the signed-pubkey design is preferred
   and is what this targets.)
2. **Anti-replay.** A monotonic `seq` (persisted) rejects replayed/rolled-back commands.
3. **Apply semantics mirror the console.** `rf_set` stages, `rf_apply` commits + persists.
   The firmware seam already exists: a verified command calls the same
   `RadioHal::apply_config()` + `rf_save()` the console uses.
4. **Acknowledged.** The node returns a signed ACK with the resulting `rf show` so the
   controller can confirm the change took before moving on.

This means **no new radio code** is needed for remote config — only the signed-control
envelope and key handling. The risky part isn't applying a setting; it's applying a
*critical* one network-wide. That's §4.

---

## 4. Network-wide retune — the safety protocol (central node MUST follow)

Changing a **critical** parameter (frequency / bandwidth / SF) across the whole network
is the dangerous operation: if a node doesn't take the new PHY (missed command, bad
flash, power glitch, half-applied), it is left on the **old** PHY and can no longer hear
the controller to be corrected — it's stranded and needs a physical visit.

To bound that risk, **before** pushing any critical retune the central node MUST:

1. **Open a BLE rescue channel with a fixed, known PIN.** Remotely command every node
   `blepin <fixed-network-PIN>` then `ble on`. Now every node is reachable out-of-band
   over BLE with a PIN the operator already knows — independent of the LoRa PHY.
   *(Primitives exist today: `blepin`, `ble on`. The fixed PIN is an operational choice,
   distributed to field operators ahead of the retune.)*
2. **Push the retune** (`rf_set …` → `rf_apply`) to all nodes and collect ACKs.
3. **Reconcile.** Nodes that ACK on the new PHY are good. Any node that does **not**
   re-appear on the new PHY is presumed stranded.
4. **Field-fix stragglers over BLE.** An operator walks to each missing node, connects
   over BLE with the fixed PIN (LoRa-independent), and sets the correct PHY locally
   (`rf …` → `rf apply`). The node rejoins.
5. **Close the rescue channel.** Once the network is whole on the new PHY, the central
   node remotely commands `ble off` on all nodes (and optionally rotates the PIN back to
   a per-node random one with `blepin random`), returning to BLE-off-by-default.

> The BLE rescue channel is the *insurance*: enabled **before** the change so it's already
> up if the change fails, and torn down **after** the network is confirmed healthy. It is
> never the primary path — it exists only so a failed retune is a walk to the node, not a
> bricked network.

### Ordering invariant
`enable BLE rescue (fixed PIN)` → `retune` → `reconcile/field-fix` → `disable BLE rescue`.
Never retune first; the rescue channel must exist *before* the node can fall off-air.

---

## 5. Firmware seam (where this plugs in)

- `RadioHal::apply_config(const RadioCfg&)` / `RadioHal::config()` — live PHY reconfig +
  readback (`src/radio_hal.*`).
- `rf …` console + `rf_save()`/`rf_load()` persistence — `src/main.cpp`.
- `ble on|off`, `blepin …` + persistence — `src/main.cpp` (the BLE rescue primitives).
- **Config-over-BLE** — `ble_poll()` demuxes the NUS into tunnel frames vs. text console
  lines; `console_exec()` routes command output back to the source transport. So the §4
  field-fix-over-BLE step works today.
- **TODO (Tier‑1):** signed control-packet verify/ACK + the central-node orchestration of
  §4. The data-plane, apply paths, and out-of-band rescue console are already in place.
