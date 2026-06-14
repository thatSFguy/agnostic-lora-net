# Implementing node keygen + signed announces (firmware)

Status (2026-06): **planned, not started.** The firmware half of
[`self-certifying-identity-plan.md`](self-certifying-identity-plan.md): mint a per-node
Ed25519 keypair at first boot, persist it, derive the node id from its pubkey, and sign the
node's announces so any receiver (and the controller) can verify identity. Depends on the
type change in [`node-id-widening-impl.md`](node-id-widening-impl.md) — do the widening
first, this on top.

All anchors below are verified against the current tree.

---

## 1. Keypair persistence (clone the `CtrlStore` pattern)

The firmware already has three near-identical flash stores; copy the shape exactly.

- `NodeStore` — `src/main.cpp:302` (magic `"AGND"`, `/agn_node.cfg`, holds `mobile`).
- `CtrlStore` — `src/main.cpp:1083` (magic `"AGCL"`, `/agn_ctrl.cfg`, holds `pubkey[32]`+`counter`),
  with `ctrl_cfg_load()`/`ctrl_cfg_save()` at `:1144`/`:1157`.
- Dirty-compare-before-write — `rf_save()` `src/main.cpp:256` reads current, `memcmp`s, skips the
  write if unchanged. **Adopt this** so we never rewrite the key page needlessly.

New store:

```c
static const char     KEY_PATH[]  = "/agn_key.cfg";
static const uint32_t KEY_MAGIC   = 0x59454B41;   // "AKEY"
struct __attribute__((packed)) KeyStore { uint32_t magic; uint8_t sk[64]; uint8_t pk[32]; };

static uint8_t node_sk[64], node_pk[32];          // RAM copies

static bool key_cfg_load();   // returns false if absent/invalid (→ first boot)
static void key_cfg_save();   // dirty-compare, then remove+write like ctrl_cfg_save
```

Persist **both** `sk` and `pk` (deriving `pk` from `sk` at boot is possible but storing it is
cheaper and matches how `CtrlStore` already caches the controller pubkey). +96 B flash.

---

## 2. First-boot keygen — and the entropy gap (security-critical)

**There is no cryptographic RNG helper today.** The only randomness in `src/main.cpp` is
`randomSeed(my_id)` (`:2413`) + Arduino `random()` (`:457`, `:699`) — a *deterministic PRNG
seeded from public, low-entropy values*. Seeding an Ed25519 key from that would make every
node's key predictable. This must be fixed as part of this work.

Add a hardware-TRNG entropy helper (new `src/entropy.cpp` or inline, platform-branched on the
macros already used at `src/main.cpp:686` — `NRF_FICR` / `ESP32`):

```c
void entropy_fill(uint8_t* buf, size_t n) {
#if defined(NRF52840_XXAA) || defined(NRF52)   // nRF52: RNG peripheral (bias-corrected)
    NRF_RNG->CONFIG = RNG_CONFIG_DERCEN_Msk; NRF_RNG->TASKS_START = 1;
    for (size_t i = 0; i < n; i++) { NRF_RNG->EVENTS_VALRDY = 0;
        while (!NRF_RNG->EVENTS_VALRDY) {} buf[i] = (uint8_t)NRF_RNG->VALUE; }
    NRF_RNG->TASKS_STOP = 1;
#elif defined(ESP32)
    for (size_t i = 0; i < n; i += 4) { uint32_t r = esp_random();
        memcpy(buf + i, &r, (n - i >= 4) ? 4 : (n - i)); }
#else
    #error "no TRNG for this target — keygen would be insecure"
#endif
}
```

The `#error` on the fallback is deliberate: a node with no real entropy must fail to build,
not silently mint a guessable key. (T1000-E / Heltec V4 from [[new-flash-targets]] are nRF/ESP
— both covered — but assert it rather than assume.)

Keygen, first boot only:

```c
if (!key_cfg_load()) {
    uint8_t seed[32]; entropy_fill(seed, 32);
    crypto_ed25519_key_pair(node_sk, node_pk, seed);   // seed is wiped by monocypher
    key_cfg_save();
}
```

---

## 3. Use the SHA-512 Ed25519 variant (match the control plane)

monocypher ships two EdDSA flavors. The control plane already uses the **SHA-512** variant —
`control.cpp:1` includes `monocypher-ed25519.h` and calls `crypto_ed25519_sign` (`:44`,`:60`) /
`crypto_ed25519_check` (`:74`). Use the **same** for node keys so there's one crypto path:

```c
void crypto_ed25519_key_pair(uint8_t sk[64], uint8_t pk[32], uint8_t seed[32]);
void crypto_ed25519_sign (uint8_t sig[64], const uint8_t sk[64], const uint8_t* m, size_t n);
int  crypto_ed25519_check(const uint8_t sig[64], const uint8_t pk[32], const uint8_t* m, size_t n); // 0 = ok
void crypto_blake2b(uint8_t* h, size_t hlen, const uint8_t* m, size_t mlen);   // for nid_from_pubkey
```

Do **not** use the `crypto_eddsa_*` (BLAKE2b) variant — it would be a second, incompatible
signature scheme on the same wire.

`nid_from_pubkey()` (from the widening doc) = `crypto_blake2b(out, 16, pk, 32)`.

---

## 4. Boot ordering (the id now depends on the key)

Today: `my_id = derive_node_id()` at `src/main.cpp:2412`, then `randomSeed(my_id)`, then the
Router is constructed `mesh::Router router_inst(my_id)` (~`:2414`). Since `my_id` will become
`hash(pubkey)`, the **keypair must exist before `my_id` is set**. Reorder `setup()`:

1. `entropy_fill` available (no init needed beyond the helper).
2. `key_cfg_load()` → if absent, generate + save (§2).
3. `my_id = nid_from_pubkey(node_pk)` (replaces `derive_node_id()`; drop the `-DAGN_NODE_ID`
   override — self-derived ids can't be overridden meaningfully).
4. Router construction and everything downstream unchanged.

`ctrl_cfg_load()` (`:2439`) and the rest stay where they are.

---

## 5. Signing the announce

Path: `send_beacon()` `src/main.cpp:776`. It lays down headers, then at `:813`–`:815`:

```c
const uint16_t base = HEADER_BYTES + sizeof(BeaconPayload);   // 21 B
mesh::Announce ann; router->build_announce(ann);              // router.h:53
uint16_t ann_len = announce_serialize(ann, frame + base, sizeof(frame) - base);  // ~179 B budget
```

Add the identity tail **after** `announce_serialize`, **before** `txq_push` (`:819`):

```c
uint8_t* p = frame + base + ann_len;
memcpy(p, node_pk, 32); p += 32;                              // pubkey
// sign domain || pubkey || announce-body  (announce body binds the routing claims to the key)
crypto_ed25519_sign(p, node_sk, /* region */, /* len */);     // 64-byte sig, domain tag "AGN-ANN-1"
uint16_t frame_len = base + ann_len + 32 + 64;
```

- **Domain tag** `"AGN-ANN-1"`, mirroring control's `"AGN-CTRL-1"` (`control.cpp:10`), so an
  announce signature can never be replayed as any other signed message.
- **Signed region** = `domain || node_pk || (announce bytes)`. The receiver checks
  `nid_from_pubkey(pk) == net.src`, then `crypto_ed25519_check` over the same region. That
  binds *who sent it* (src), *what key* (pk), and *what they claimed* (the routing reports).
- Codec changes live in `announce_codec.{h,cpp}` — add `announce_serialize_signed()` /
  `announce_deserialize_and_verify()` rather than overloading the existing fns, so unsigned
  paths (host tests) stay simple.

### The MTU tension — sign on a slower cadence

21 (base) + 96 (pk+sig) = 117 B leaves only ~83 B for the announce body, and at the widened
18 B/report that's ~4 reports. Two ways to live within it:

- **(Recommended) Identity-beacon cadence.** Don't sign *every* beacon. Most beacons carry the
  full routing announce, unsigned, as today; every Nth beacon (or on a separate ~minutes timer)
  emit a compact **signed identity announce** (pk + sig over `{src, id, epoch}`, few routing
  reports). Receivers cache the verified id↔pubkey binding; routing keeps flowing on the dense
  unsigned beacons in between. This decouples identity airtime from routing airtime and is the
  cleanest fit with the rotate-bindings idea in the design plan §3.
- **(Alternative) Sign every beacon, shrink the body.** Simpler code, but permanently trims
  routing capacity per beacon. Only acceptable in small/sparse meshes.

Pick the cadence model unless bench data says routing convergence tolerates the smaller body.

---

## 6. Verifying *received* announces (node side)

A node that receives a signed announce should: parse → `nid_from_pubkey(pk) == src`? →
`crypto_ed25519_check`? → only then trust the binding. On failure, **drop silently** (the
control plane's "never ack an auth failure" rule, `control.h:18`, applies — no oracle).
Unsigned/legacy announces: accept routing data but mark the id↔pubkey binding as *unverified*
(it just won't be ACL-eligible at the controller). This keeps mixed-version benches functional
during rollout.

Control *command* verification (`control.cpp:64`) is untouched — that still uses the
controller's pubkey, not the node's.

---

## 7. Tests & sequencing

- Host tests: add a `test_identity` suite — keygen determinism from a fixed seed, `nid_from_pubkey`
  vector, sign→check round-trip, and `announce_deserialize_and_verify` rejecting a flipped byte.
- Keep this branch **firmware-identity-only**; the controller verify/ACL
  ([`controller-verify-acl-impl.md`](controller-verify-acl-impl.md)) composes after.
- Bench: reflash the RAK pair ([[hardware]]); confirm each mints a distinct key, the BLE name
  shows the new 32-hex id, and a peer verifies the signed identity announce. **Watch entropy on
  the very first boot** — verify two freshly-flashed boards get different keys (the entropy gap
  in §2 is the one thing that fails silently and catastrophically).

Related: [[node-id-identity]], [[new-flash-targets]], [[hardware]],
[`self-certifying-identity-plan.md`](self-certifying-identity-plan.md).
