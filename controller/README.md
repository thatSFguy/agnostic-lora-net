# agnctl — Phase 4 controller (Go scaffold)

The Tier-1 controller, written in Go so it runs on **this laptop first** and deploys to
the RPi Zero 2 W unchanged. See [`../docs/phase4-controller-plan.md`](../docs/phase4-controller-plan.md)
for the full plan.

**Covers workstream 4a (ingest), §8 (passive RF-utilisation capture), and 4b (signed
control: POWER/CONFIRM/BLOCK/UNBLOCK).** It reads a tethered node's console stream, builds
the live topology, logs chattiness data to CSV, and — with a controller key — signs and
pushes commands into the mesh. The RF policy engine and served dashboard come next. **No
external deps (Go stdlib only)** — builds offline; Ed25519 signing is `crypto/ed25519`.

## Layout

```
controller/
  cmd/agnctl/         CLI entrypoint + interactive command REPL
  internal/ingest/    serial/file sources + the console line parser (Go port of web/map.html)
  internal/topo/      live global graph (nodes + per-direction links)
  internal/capture/   passive airtime/chattiness recorder → CSV
  internal/sign/      Ed25519 control codec — byte-exact mirror of lib/mesh/control.*
  internal/keystore/  controller key + monotonic replay counter (import browser / mint)
  internal/commander/ build POWER/BLOCK/… as the node's `ctrlsend <hex>` line
  internal/policy/    autonomous RF power-optimisation loop + JSONL audit log
  internal/httpd/     live read-only web dashboard (topology + decision feed)
  testdata/           a sample console log for replay (no hardware needed)
```

## Run

```bash
# Replay the bundled sample — no hardware, proves the pipeline:
go run ./cmd/agnctl -file testdata/session.log -summary 1s -snapshot topo.json

# Live, against a tethered node (enables `trace on` + polls info/nbrdump):
go run ./cmd/agnctl -port /dev/ttyACM0 -csv capture.csv

# Pipe any console log in:
cat session.log | go run ./cmd/agnctl -file -

go test ./...    # parser, codec, keystore, commander unit tests
```

## Issuing signed commands

With a tethered node, the controller signs and pushes commands. Pick key custody once:

```bash
# reuse the browser controller key (export a backup from the map app first):
go run ./cmd/agnctl -port /dev/ttyACM0 -import-backup agnlora-controller-backup.json
# or mint a fresh production key (prints the pubkey + the `ctrlkey` line to provision):
go run ./cmd/agnctl -port /dev/ttyACM0 -mint
```

The key + replay counter persist under `-keydir` (default `state/`). Provision each node
once with the printed `ctrlkey <pubhex>`. Then type commands on stdin:

```
power <id> <dbm>            # lower/raise a node's TX power (decreases auto-revert in 60s)
confirm <id> <dbm>          # disarm the revert for a provisional power change
block <recip> <victim> [m]  # tell <recip> to drop its link to <victim> for [m] min (default 30)
unblock <recip> <victim>    # clear that block
pub                         # print the controller pubkey
```

Acks (`[ctrl] ack …`) come back through the normal console stream. The replay counter is
seeded from wall-clock seconds, so it always clears a node's existing floor — even when
reusing a key the browser already used.

## Autonomous RF optimisation (watch it live)

`-optimize` runs the power loop: each cycle it computes every managed node's SNR margin
(observed SNR − the SF floor) and step-limits its TX power toward a target band — trimming
nodes heard too loudly (the "all nodes turned way down" fix, done *measured*), boosting
marginal ones. **Dry-run by default**; add `-apply` to actually send.

```bash
# preview the reasoning with no hardware (final eval over a replay):
go run ./cmd/agnctl -file testdata/session.log -optimize

# live, dry-run — narrates decisions every cycle, sends nothing:
go run ./cmd/agnctl -port /dev/ttyACM0 -optimize

# live, acting — signs + sends, confirms decreases once it re-sees the node:
go run ./cmd/agnctl -port /dev/ttyACM0 -optimize -apply
```

### Watch it in a browser

Add `-http :8080` and open **http://localhost:8080** — a read-only live dashboard showing
each node's power/margin and the streaming decision feed (the same records as `policy.jsonl`,
colour-coded). Controls stay on the trusted CLI; the page only watches.

```bash
go run ./cmd/agnctl -port /dev/ttyACM0 -optimize -http :8080
```

Tuning: `-margin-low 6 -margin-high 12 -max-step 3 -policy-interval 15s`. Every
observation → decision → command → ack is logged to a **JSONL audit trail**
(`-policy-log policy.jsonl`) *and* narrated to the console, e.g.:

```
14:03:21  8EA09546  SF9 snr=9.0 margin=21.5 → Lower 22→19dBm (Δ-3) [SENT ctr=…] : margin above band …
14:03:36  8EA09546  [confirm] still reachable after decrease — CONFIRMed 19dBm (ctr=…)
```

**Safety:** the controller owns each node's power (absolute POWER), step-limits every
change, and only CONFIRMs a *decrease* after re-observing the node reachable — so a bad
call (or the controller dying) self-heals via the on-device 60 s revert. *Scope
(mesh-wide):* a node has one TX power, so it optimises against the **weakest outbound link
the node must keep** — the neighbour that hears it worst — gathered from every link
`node→X`. The tethered gateway's own links carry measured SNR; remote links arrive via
round-robin `status <id>` telemetry polls as link *quality*, which is inverted to an
approximate SNR. That estimate saturates at high SNR, so a quality-only link can trigger a
**raise** (a weak link is unambiguous) but never a **lower** — power is only trimmed on a
measured SNR. Per-link SNR everywhere (so remote links can be trimmed too) needs the
telemetry-frame change in §4c.

Outputs: `capture.csv` (one row per console event; `airframe=1` marks real on-air
frames — beacon TX/RX, forwards — visible with `trace on`) and an optional topology
`-snapshot` JSON. The periodic `[capture]` summary on stderr is the first-order
chattiness read (airframes/sec, broken down by class).

## Honest scope

- **Capture today = frame counts/classes, not true time-on-air.** Real ToA-by-class
  needs the node-side instrumentation in plan §8; this is the harness that data plugs
  into. Counts already surface beacon overhead, forward volume, and retransmit pressure.
- **Signing + send: done for POWER/CONFIRM/BLOCK/UNBLOCK.** `internal/sign` builds them
  byte-identical to the firmware (gold-standard gate in `test/test_ctrl_interop`);
  `keystore`+`commander` push them via the node's `ctrlsend` bridge. Every connectivity-
  reducing command has an auto-revert rail (power 60 s; block TTL), so killing the
  controller returns the mesh to a safe state. **ROUTE override is deferred** (optional;
  needs a Router override API). **Web block button** still uses the local console path
  until the controller `httpd` lands.
- **The `state/` dir holds the secret controller key — gitignored, never commit it.** It
  is the network's write credential; back it up like one.
- **Serial source is Linux/WSL/RPi** (raw tty via best-effort `stty`, USB-CDC ignores
  baud). For other hosts, swap in a serial library behind `ingest.Source`.

## Toolchain

Go is **not yet installed** on this laptop. Install it, then the commands above work:

```bash
# Debian/Ubuntu/WSL:
sudo apt install golang-go        # or grab the latest from https://go.dev/dl/
go version
```
