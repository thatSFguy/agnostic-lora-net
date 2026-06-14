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

## Runtime BLE enable/disable for fixed nodes

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

**Sub-parts.**
- [ ] Runtime BLE start/stop (Bluefruit advertising + SoftDevice gating) — verify the SoftDevice
      can be quiesced/re-activated cleanly within one boot (it likely cannot be torn down; gating
      advertising/connectability is the pragmatic path).
- [ ] Enable trigger — preferred: a **signed control-plane command** ("enter provisioning mode"),
      so a node can be opened for retune remotely without physical access. Physical-button fallback
      for the field.
- [ ] **Proof-of-config**: node reports its current PHY/config (telemetry REPLY already carries
      `sf`/`power_dbm`; extend as needed); controller compares against the pushed config and only
      marks the node "confirmed on new config" on a match.
- [ ] Explicit **signed "BLE off"** command, issued only after proof-of-config succeeds.
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
