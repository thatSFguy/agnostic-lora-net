# Implementing controller-side identity verify + membership ACL

Status (2026-06): **planned, not started.** The controller half of
[`self-certifying-identity-plan.md`](self-certifying-identity-plan.md): learn each node's
verified pubkey, enforce a membership allowlist over approved pubkeys, and refuse to manage
(command) nodes that aren't approved. Composes on top of the firmware work
([`node-keygen-signed-announce-impl.md`](node-keygen-signed-announce-impl.md)) and the type
change ([`node-id-widening-impl.md`](node-id-widening-impl.md)).

All anchors verified against the current tree.

---

## 0. The architectural fact that shapes everything: the controller reads *text*

The controller does **not** ingest binary frames. It parses the gateway node's **console log
lines** with regexes — e.g. `parse.go:12` matches `^node ([0-9A-Fa-f]{8})`, `:23`
`^\[status\] …`, `:24` `^\[nbrs\] ([0-9A-Fa-f]{8})`. The `Event` struct
(`ingest/event.go:62`) carries `ID/Peer/Dst` as hex *strings*, normalized uppercase.

So "verify the announce signature" is not something the controller does over raw bytes off the
air — it never sees them. Two viable trust models:

| | Who runs `ed25519_check` | Over serial | Trust anchor |
|---|---|---|---|
| **A (recommended)** | the **gateway node** (it has the frame + codec) | gateway prints `id`, `pub`, `ok=1` | the controller already trusts its own gateway's console for *everything* |
| **B** | the **controller** (Go `crypto/ed25519`) | gateway dumps `pub` + `sig` + signed-region as hex | controller-authoritative, but heavy on serial |

**Model A** is the right default: the gateway is the controller's own radio and the controller
already believes every `[status]`/`[nbrs]` line it prints — adding "I verified this announce's
signature" to that trust is free and keeps serial lean. The firmware
(`node-keygen-signed-announce-impl.md` §6) already verifies received announces; it just needs to
**emit the binding + verdict** in its console output. Add a line like:

```
[ann] <id 32hex> pub=<64hex> sig=ok        # or sig=bad / sig=none (legacy/unsigned)
```

The controller keys the ACL on `pub` and checks `nid_from_pubkey(pub) == id` defensively (cheap
in Go even under Model A). Reserve Model B for if/when the gateway itself shouldn't be trusted.

> Action for the firmware side: this `[ann] … pub= … sig=` line is the contract between the two
> halves. Define it before building either.

---

## 1. Keystore — add the allowlist (`controller/internal/keystore/keystore.go`)

Today (`:30`–`:41`):

```go
type persisted struct { SeedHex string `json:"seed_hex"`; Counter uint32 `json:"counter"` }
type Store struct { mu sync.Mutex; dir string; priv ed25519.PrivateKey; counter uint32 }
```

Add the allowlist alongside the controller's own key (it's controller policy, persisted in the
same `controller.json`, and carried in backups):

```go
type persisted struct {
    SeedHex   string            `json:"seed_hex"`
    Counter   uint32            `json:"counter"`
    Allowlist map[string]int64  `json:"allowlist,omitempty"`  // pubkeyHex → approved unix-secs (0=pending/revoked absent)
}
type Store struct { /* …existing… */ allow map[string]int64 }

func (s *Store) Approve(pubHex string, atUnix int64) error  // add + save()
func (s *Store) Revoke(pubHex string) error                 // delete + save()
func (s *Store) IsAllowed(pubHex string) bool
func (s *Store) Allowlist() map[string]int64                // copy for UI/CLI
```

- Wire into `Open()` (`:46`) load and `save()` (`:186`); include in `Export()` (`:162`) and the
  backup/restore path so ACL survives a controller migration (the recent map-app backup work).
- **Replay counter is unchanged** — it's per-controller, not per-node; `Next()` (`:175`) stays as
  is. Node identity doesn't touch it.
- Pass timestamps in (don't call `time.Now()` deep in the store) to keep it testable.

---

## 2. Ingest — carry the pubkey + verdict through (`controller/internal/ingest/`)

- `parse.go` — add a matcher for the new `[ann] <id> pub=<hex> sig=<ok|bad|none>` line → a new
  `KindIdentity` event (or fold onto the existing announce event). **Widen the id regex** from
  `[0-9A-Fa-f]{8}` to `[0-9A-Fa-f]{32}` across `:12`,`:23`,`:24` (the node-id widening). Keep it
  tolerant of both widths during rollout if you want mixed-version benches to parse.
- `event.go` — extend `Event` (`:62`): add `Pub string` and `SigOK bool` (and `Verified` if you
  prefer explicit). Everything else (`ID/Peer/Dst/Num/Str`) is unchanged.

---

## 3. Topology — verify, bind, gate (`controller/internal/topo/model.go`)

`Node` (`:17`) and `Graph` (`:43`, keyed by id string) gain identity state:

```go
type Node struct {
    /* …existing… */
    Pub      string `json:"pub,omitempty"`     // verified node pubkey hex
    Verified bool   `json:"verified"`          // sig=ok AND nid_from_pubkey(pub)==id
    ACL      string `json:"acl"`               // "allowed" | "pending" | "blocked"
}
```

In `Apply()` (`:83`) when handling the identity/announce event:

1. If `SigOK` and (defensively) `hash(pub)==id` → set `Pub`, `Verified=true`; else `Verified=false`.
2. `ACL = allowed | pending` from `keystore.IsAllowed(pub)` (a verified node not yet approved is
   `pending`; an unverified node is shown but never `allowed`).
3. `ManagedIDs()` / whatever feeds the autonomous power loop ([[controller-power-mesh]]) must
   **filter to `Verified && ACL=="allowed"`** — the optimizer should never tune an unapproved node.

The `Graph` needs the `*keystore.Store` (or an `IsAllowed` func) injected so `Apply` can classify.

---

## 4. Commands — widen the target, gate on ACL (`sign/control.go`, `httpd`, `agnctl`)

**Wire (the widening):** `sign/control.go` — `Command.Target uint32` and `Aux uint32` (`:86`)
become 16-byte; `unsignedBytes` 11→23, `unsignedBytesBlock` 15→39; `MsgBytes` 75→87,
`BlkBytes` 79→103; bump `CtrlVer` 1→2 (`:18`). `BuildControl`/`BuildBlock`/`VerifyControl`
(`:96`/`:114`/`:137`) take/emit the 16-byte target. Mirrors `control.h` in firmware.

**ID parsing:** the two `strconv.ParseUint(s, 16, 32)` sites — `httpd/server.go hexID()` (`:143`)
and `agnctl parseID()` (`:338`) — become a 16-byte hex decoder (`hex.DecodeString`, expect 32
chars → `[16]byte`).

**ACL gate (the enforcement point):** in `httpd issue()` (`:150`) and the `agnctl`
power/confirm/block/unblock handlers (`:365`–`:415`), **before** `ks.Next()` + signing, resolve
the target id → its verified `Pub` (from the graph) and reject if `!ks.IsAllowed(pub)` with a
clear message. This is what makes the ACL a real boundary rather than a label. (`raw` serial
passthrough stays ungated — it's operator-direct.)

---

## 5. HTTP API + dashboard (`controller/internal/httpd/`)

- `stateJSON` (`server.go:192`) — `Node` already serializes; the new `Pub/Verified/ACL` fields
  flow to the UI for free. Add an `Allowlist map[string]int64` field for the management view.
- New route `/api/acl` (POST) next to `/api/cmd` (`:303`): `{action:"approve"|"revoke", pub:"…"}`
  → `ks.Approve/Revoke` → persist. Reuse the existing handler/error plumbing.
- `index.html` — the node cards (`.node`, around `:81`) gain a trust badge (✓ verified /
  ⚠ unverified) and an ACL pill (Allowed / Pending / Blocked). Add a **Security** view beside the
  existing tabs (Dashboard | Map | Add/Configure | Settings) listing pending-approval nodes with
  Approve/Revoke buttons and the full 32-hex pubkey. A header warning when any `pending`/unverified
  node is present draws the operator's eye.
- `uistate.go` — no new persistence needed (the ACL lives in the keystore, not `ui.json`); just
  make sure backup/restore (`backupJSON` `:85`) includes the keystore allowlist.

---

## 6. CLI (`controller/cmd/agnctl/main.go`)

Add an `acl` subcommand in `handleCommand()` (`:343`) and the help string (`:328`,`:365`):

```
acl list                 # approved pubkeys + approval time
acl pending              # verified-but-unapproved nodes (id + pub)
acl approve <pubhex>     # ks.Approve
acl revoke  <pubhex>
```

`acl pending` reads the graph for `Verified && ACL=="pending"`; the others call the keystore.
Same dispatch shape as the existing `power/block/pub` cases.

---

## 7. Sequencing & tests

1. **Agree the `[ann] … pub= … sig=` console contract** with the firmware side first (§0).
2. Land keystore allowlist + tests (pure CRUD + persistence round-trip, incl. backup export).
3. Widen command targeting (`sign/control.go`) in lockstep with the firmware `CTRL_VER`→2 — this
   is the breaking bit; gate it behind the same reflash.
4. Ingest/topo verify + gate; HTTP/CLI surfaces last.
5. Tests: a `sign` round-trip at 16-byte target; an ingest test feeding a synthetic
   `[ann] … sig=ok/bad` line and asserting `Verified`/`ACL`; an `issue()` test asserting an
   unapproved target is rejected before `Next()` advances the counter (no counter burn on reject).

Honest caveat to keep visible: under Model A the controller's identity trust is **only as strong
as its own gateway node**. That's consistent with today's trust model (the controller already
believes everything the gateway prints), but it means a compromised gateway could assert false
bindings. If that ever matters, switch to Model B (controller re-verifies raw sig) — the keystore
ACL and UI/CLI don't change, only the ingest path does.

Related: [[controller-power-mesh]], [[node-id-identity]],
[`self-certifying-identity-plan.md`](self-certifying-identity-plan.md),
[`phase4-controller-plan.md`](phase4-controller-plan.md).
