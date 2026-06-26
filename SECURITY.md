# Security Policy

> ⚠️ **agnostic-LoRa-Net is Alpha software** (0.x). It is a research/hacking platform,
> not a hardened product, and ships with **no security guarantee**. Do not rely on it to
> protect life-safety or high-value traffic. That said, the cryptography is meant to be
> correct, the project is **feature-frozen specifically to keep the attack surface small**,
> and reports are very welcome.

## Trust model (what's in scope to attack)

The design assumes a clear boundary between trusted local control and the untrusted air:

- **LoRa air interface — UNTRUSTED.** Anything arriving over the radio is hostile until
  proven otherwise. Frame parsing is bounds-checked; node identity and the control plane are
  Ed25519-verified; DATA/ACK/flood paths carry replay/dedup protection. This is the surface
  that matters most, and the one the frozen feature set protects.
- **USB serial console — TRUSTED (physical access).** The console can reflash
  (`bootloader`/`dfu`), set the controller key, and retune the radio. Physical USB access to
  a node is therefore full control of that node, by design. Protect the hardware.
- **BLE — TRUSTED after pairing.** The Nordic SoftDevice enforces pairing with MITM
  protection and a per-node PIN; BLE is off by default and is the operator's commissioning
  channel, not an open service.
- **Controller key — the network write credential.** Whoever holds it can issue signed
  POWER/CONFIRM/BLOCK/UNBLOCK. Treat it like a root key (`state/`, gitignored; the CLI
  controller keeps it in a file with OS permissions). If you use the browser map app, note
  it stores the key in `localStorage` — convenient but **not encrypted at rest**; prefer the
  CLI `agnctl` for any key you actually care about.
- **End-to-end payloads — opaque.** Reticulum rides as ciphertext; the backbone never sees
  plaintext. App-level peer authentication is the app's job, not the mesh's.

## Supported versions

Being Alpha, only the **latest release** and the current `main` branch receive fixes.
There are no backported patches for older tags.

## Reporting a vulnerability

**Please do not open a public issue for a security vulnerability.** Use one of:

1. **GitHub private vulnerability reporting** (preferred) — the **Security** tab →
   **"Report a vulnerability"**. This keeps the report private until a fix is ready.
2. **Email** — `rob@woodhousellc.com` (Woodhouse LLC). PGP available on request.

Please include: affected version/commit, a description, and ideally a proof-of-concept or
the steps to reproduce. This is a single-maintainer project, so response is **best-effort**
— expect an acknowledgement within a week or so, not an SLA.

## Scope & already-known limitations

The interesting surface is the crypto and control plane:

- **Self-certifying node identity** — each node mints its own Ed25519 keypair from a
  hardware TRNG; the node id is `blake2b(pubkey)[0:16]`, and signed announces let peers
  verify the id↔key binding (`src/main.cpp`, `lib/mesh/announce_codec.*`).
- **Signed control plane** — Ed25519-signed `POWER/CONFIRM/BLOCK/UNBLOCK` with a monotonic
  replay counter (`lib/mesh/control.*`, `controller/internal/sign`).
- **End-to-end transport** — Reticulum rides as opaque payload; the backbone never sees
  plaintext.

A few weaknesses are **known**, so they are not news (but a concrete exploit or a fix
still is):

- **Controller-key rotation / re-key is not implemented yet.** The controller key is the
  network's write credential; protect it accordingly (`state/`, gitignored). The browser map
  app keeps it in unencrypted `localStorage` — use the CLI `agnctl` for a key you care about.
- **The `-DAGN_NODE_ID=…` build override** mints a node id with no keypair (bench/debug
  only) — such a node cannot sign its announces. Don't ship production firmware built with it;
  a build that uses it is unsigned and self-announces unverifiably.
- **No per-peer verify-rate-limiting yet.** A peer that floods announces or signed-control
  copies forces an Ed25519 verify per first-seen frame; dedup limits reflood, but a stolen
  controller key or a churn of bogus identities can still cost CPU/airtime. Hardening is on
  the backlog (see the README's *What's left*).
- **No firmware-image signing.** The web flasher writes whatever image it's given; provenance
  is on you (build from source, or trust the GitHub release you're pulling).
- A node id authenticates the *node*, not an application user — it is a routing **locator**.
  Apps must still authenticate peers at their own layer (Reticulum does this end-to-end).
- The mesh is a **shared-airtime broadcast medium**: traffic analysis, jamming, and replay
  at the radio layer are inherent to LoRa and out of scope for a fix here.

Out of scope: vulnerabilities in vendored third-party code (e.g. `lib/monocypher`, RadioLib,
Reticulum) — please report those upstream, though a heads-up here is appreciated.
