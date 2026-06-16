# TODO

Working backlog. The **self-certifying identity + membership ACL** effort is planned across
its own doc set and is *not* duplicated here — see
[`docs/self-certifying-identity-plan.md`](docs/self-certifying-identity-plan.md) and the three
implementation notes it links. The items below are adjacent infrastructure work that surfaced
while scoping identity but are **separate from it**.

---

## Controller → v2 (16-byte ids) — ✅ DONE & MERGED (2026-06-14)

Merged to `main` (merge `2fc3ed6`): the self-certifying identity + membership ACL + signed
BLE/retune v2 control plane (`CTRL_VER` 1→2, 16-byte node ids). Both halves landed —

- **Firmware:** 16-byte self-certifying ids (blake2b(pubkey)), per-node keygen, signed announces
  + verify-once, `pub` command, `CTRL_BLE`, `CTRL_RETUNE`. Host **76/76**, all 5 targets build;
  keygen + persistence + id==blake2b(pubkey) bench-validated on two RAKs.
- **Controller:** `sign` widened to 16-byte target/aux (87/103/99-byte POWER/BLOCK/RETUNE),
  `CmdBle`/`BuildBle`/`BuildRetune`; ingest reads `[ann] <id> pub= sig=` → verified id↔pubkey;
  keystore allowlist; `ManagedIDs`/`CommandAllowed` gate (verified + allowed); `/api/acl` +
  **Security** dashboard tab; `agnctl acl list|pending|approve|revoke` + `retune`. Controller
  tests green. Plan: [`docs/controller-verify-acl-impl.md`](docs/controller-verify-acl-impl.md).

Open follow-ups:
- [ ] **Re-pin `test_ctrl_interop` v2 vectors on-device** — `go test ./internal/sign -run EmitVector -v`
      emits them (GO_MSG=87, GO_BLK=103). The firmware test currently uses sign→verify round-trips.
- [ ] **Network-wide retune orchestration** (remote-config.md §4): open BLE rescue → push retune →
      reconcile (nodes reappear on the new PHY) → field-fix stragglers → close rescue. The signed
      `retune` command exists; the multi-node *workflow* + dashboard surface is not built (a naive
      fire-and-forget retune button can strand a node — left deliberately for this considered flow).

> **v2 firmware images are staged in `web/fw/`** (all 5 variants — `agn-rak`, `agn-xiao`,
> `agn-promicro`, `agn-t1000` as `.dfu.json`+`.uf2`; `agn-heltec-v4.bin` for CLI flash), served at
> `/fw/` for the dashboard Flash tab. Re-run `bash scripts/refresh_web_fw.sh` to rebuild after changes.

---

## Relay-only role build (RAM reclaim)

- [ ] Add a **relay-only build variant** for fixed backbone nodes that only forward — they never
      originate or receive SAR image transfers.

**Why.** On nRF52840 the app's RAM is dominated by buffers, not the mesh tables. The biggest
app-controlled consumers are the SAR (store-and-forward image) buffers — `sar_buf` (8 KB) +
`sar_rx` (8 KB) = 16 KB, sized by `SAR_MAX_FILE = 8192` (`lib/mesh/sar.h:26`) — plus
`cache_buffer` (4 KB), `ann_cache[8]` (3.3 KB), and the tx/rx queues. A relay node that never
handles image payloads can shrink/drop `SAR_MAX_FILE` and trim the caches, reclaiming far more
RAM than any other lever, deterministically.

**Note (important):** this is the *real* RAM lever. Turning BLE off at runtime does **not**
reclaim RAM — the SoftDevice's 24 KB is reserved at the bottom of RAM (`RAM` origin `0x20006000`
in `nrf52840_s140_v6.ld`) and fixed at link time; runtime BLE-off saves power, not memory. See
the BLE item below.

**Scope sketch.** A per-role compile flag (e.g. `-DAGN_ROLE_RELAY`) that sets `SAR_MAX_FILE`
small (or compiles SAR out), reduces cache caps, and is selectable per env in `platformio.ini`.
Edge/gateway nodes keep full buffers.

**Acceptance.** Relay image builds clean, RAM `size` drops by ~16–20 KB vs the full build, and a
relay node still routes/forwards correctly on the bench.

---

## Remote BLE enable/disable for fixed nodes

- [ ] Make BLE **off by default** on fixed nodes, enabled only for a provisioning/retune window,
      then disabled again — but gated on *positive confirmation*, never on a timer.

**Why.** Fixed nodes need BLE only ~2 % of the time (retune, provisioning, verification). Keeping
the radio/SoftDevice dark by default saves power and shrinks the attack surface. The intended
flow: enable BLE → push retune → controller verifies the node adopted the new config → disable
BLE.

**HARD CONSTRAINT — this must NOT be a dead-man / auto-revert switch.** BLE may only be disabled
**after the controller has concrete proof that the node is up and operating on the new
configuration** (e.g. the node is reachable on the new PHY and reports the expected config in
telemetry), and then by an explicit "BLE off" command. Rationale: disabling BLE removes the
*local* recovery path. If a retune fails (node not reachable on the new PHY) and BLE had been
auto-disabled on a timer, the only recovery is **physically driving to the node and rolling back
over local BLE**. So BLE must stay **enabled** until the controller positively confirms — confirmation
is the gate, not elapsed time.

This is deliberately the **inverse** of the power-command dead-man rail: power fails *safe* by
auto-reverting to loud; BLE must **not** fail to "off", because off = remotely unrecoverable.

**What already exists (2026-06-14 audit).** The *node-side runtime toggle is already built* —
`ble on` / `ble off` / `ble unbond` console commands (`src/main.cpp:2065`), **lazy SoftDevice
bring-up** (`ble_setup()` only enables the stack on first `ble on`, so a node that never enables
BLE pays zero runtime/RAM-at-init cost — `src/main.cpp:517`), and **enabled-state persisted to
flash** and restored on reboot (`cfg_ble_enabled`, `src/main.cpp:2444`). This confirms the note's
hunch: the SoftDevice is *not* torn down — it's gated via lazy init + advertising start/stop.
So the original "runtime BLE start/stop" sub-part is **done**. What is missing is the **remote**
path: the signed control plane has only four commands —
`CTRL_POWER/CONFIRM/BLOCK/UNBLOCK` (`lib/mesh/control.h:32`), **no `CTRL_BLE`** — so the toggle is
reachable today only over the **local serial console** (i.e. only on the tethered gateway).
Remote nodes cannot be flipped over the air. The remaining work is therefore mostly *wiring the
existing toggle to the signed control plane* + the confirmation gate, not new BLE plumbing.

### Controller work (this is independently buildable now)

- [x] Manual BLE on/off from the **node list** on the dashboard — a clickable pill (color =
      state: green on / grey off / dim "?" unknown). Tethered **gateway** is driven directly via
      the existing `ble on/off` console line (works today); remote nodes send a signed `CTRL_BLE`
      (works once the firmware half lands). *(commander `Ble`, sign `CmdBle=5`/`BuildBle`,
      `/api/cmd` action `ble`, pill in `index.html`.)*
- [ ] **Proof-of-config gate** (pure controller policy): never auto-send "BLE off"; only enable
      the "off" affordance / auto-disable after telemetry proves the node is up on the *expected*
      config. Build against the `ble=` telemetry field (parser already extended to read it).
- [ ] Surface node BLE state in the list/map from telemetry once firmware reports it (controller
      already parses `ble=` from the `[status]` line and carries `Node.BLE` tri-state).

### Firmware work (the other half — pairs with the controller above)

- [x] Runtime BLE start/stop (lazy SoftDevice + advertising gate) — **already done**, see audit
      above (`ble_start_adv`/`ble_stop_adv`, `cfg_ble_enabled` persistence).
- [x] Add `CTRL_BLE = 5` + node handler (arg 1/0 → `ble_start_adv()`/`ble_stop_adv()` + persist).
      **Done on `worktree-self-certifying-identity`** — but in **v2** form (23-byte header,
      16-byte target — NOT the 11-byte v1 layout originally noted here). `#ifdef AGN_BLE` guarded
      (no-op on ESP32). The controller's `BuildBle` must emit the v2 layout to match.
- [x] Enable trigger — signed `CTRL_BLE on` is the remote "enter provisioning mode" opener.
      **Done (branch).** ~~Physical-button fallback~~ CLOSED (operator: "if I
      have to get to it, I can plug into it" — the local USB/serial console already enables BLE on a
      tethered node, so no separate button path is needed).
- [x] **Proof-of-config**: `ble=N` now on the `[status]` line via `TELEM_FLAG_BLE` (set from
      `ble_advertising`). **Done (branch)** — `src/main.cpp` `telem_print_status`.
- [x] Explicit signed "BLE off" honored as a normal command (firmware obeys; the *gate* lives in
      the controller). **Done (branch).**
- [x] The **retune push is now a signed control command** — `CTRL_RETUNE = 6` (v2), PHY-only
      13-byte blob, atomic apply + persist, range-validated, no firmware auto-revert (operational
      BLE-rescue per [`docs/remote-config.md`](docs/remote-config.md) §4). **Done on
      `worktree-self-certifying-identity`.** Controller needs a matching `BuildRetune` (v2) — see
      the Controller→v2 handoff above.

---

## Other infra that surfaced (non-identity)

- [x] **Pin down the true free-RAM margin.** ANSWERED (2026-06-14, v2 branch device build): the
      linker reports **RAM 25% — 62 KB used of 248 KB** on RAK4631. The scary ~234 KB `bss` from
      `arm-none-eabi-size` was indeed the sbrk heap pool, not consumed static RAM. **RAM is NOT
      constrained** → the relay-only build is a nice-to-have, not a necessity; the identity
      widening + keygen + signed announces cost is comfortably absorbed.
- [ ] **(Optional) SoftDevice RAM-config tuning.** The 24 KB SoftDevice reservation is tunable at
      link/config time — fewer concurrent connections, smaller ATT table, shorter event length →
      lower `RAM_START` → relink for a few KB back. Build-time only; lower priority than the
      relay-only buffer trim.

- [x] **Controller optimiser is now runtime-tunable (2026-06-15).** Connectivity-floor governor
      (`-conn-floor`), adaptive telemetry polling (back off stable nodes + yield to messaging),
      and a live governor + dry-run↔APPLY selector in the dashboard Settings tab.
