# TODO

Working backlog. The **self-certifying identity + membership ACL** effort is planned across
its own doc set and is *not* duplicated here — see
[`docs/self-certifying-identity-plan.md`](docs/self-certifying-identity-plan.md) and the three
implementation notes it links. The items below are adjacent infrastructure work that surfaced
while scoping identity but are **separate from it**.

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
- [ ] Add `CTRL_BLE = 5` to `lib/mesh/control.h` + a node handler that maps arg 1/0 →
      `ble_start_adv()` / `ble_stop_adv()` and persists. Mirror the controller wire format
      (11-byte unsigned header + 64-byte sig, arg = enable flag — same layout as POWER).
- [ ] Enable trigger — the signed `CTRL_BLE on` *is* the "enter provisioning mode" remote opener;
      keep a physical-button fallback for the field.
- [ ] **Proof-of-config**: add `ble=N` to the `[status]` telemetry line (`src/main.cpp:1335`) so
      the controller can confirm the node's actual advertising state. (Controller-side regex +
      `Node.BLE` already in place to consume it.)
- [ ] Explicit signed "BLE off" honored only as a normal command (the *gate* lives in the
      controller, per above — firmware just obeys).
- [ ] Dependency: the **retune push itself should be a signed control command** (see
      [`docs/remote-config.md`](docs/remote-config.md) — signing was noted pending).

---

## Other infra that surfaced (non-identity)

- [ ] **Pin down the true free-RAM margin.** `arm-none-eabi-size` reports ~234 KB bss on the RAK
      build, but named symbols sum to only ~50 KB — the rest is almost certainly the sbrk heap
      pool, not consumed static RAM. Run `pio run -e wiscore_rak4631 -t size` for the authoritative
      `RAM: x% (N of M)` line, or generate the `.map` and measure the `__bss_end__ → __StackTop`
      gap. This determines whether RAM is actually constrained or already comfortable (it decides
      how urgent the relay-only build is).
- [ ] **(Optional) SoftDevice RAM-config tuning.** The 24 KB SoftDevice reservation is tunable at
      link/config time — fewer concurrent connections, smaller ATT table, shorter event length →
      lower `RAM_START` → relink for a few KB back. Build-time only; lower priority than the
      relay-only buffer trim.
