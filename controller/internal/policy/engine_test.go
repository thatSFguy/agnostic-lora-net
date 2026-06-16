package policy

import (
	"crypto/ed25519"
	"encoding/hex"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"agnostic-lora-net/controller/internal/keystore"
	"agnostic-lora-net/controller/internal/sign"
	"agnostic-lora-net/controller/internal/topo"
)

// v2 32-hex node ids (the engine builds commands via sign.ParseNodeID, which needs 16 bytes).
const (
	gwID = "00000000000000000000000000000001"
	aID  = "aaaa00010000000000000000000000a1" // loud node
	bID  = "bbbb00020000000000000000000000b2" // in-band node
	nID  = "cccc00030000000000000000000000c3" // connectivity-floor node under test
	rID  = "dddd00040000000000000000000000d4" // repeater
	fID  = "eeee00050000000000000000000000e5" // far peer, reachable via the repeater
	lID  = "ffff00060000000000000000000000f6" // leaf that depends on nID
)

func mustNID(s string) sign.NodeID {
	id, err := sign.ParseNodeID(s)
	if err != nil {
		panic(err)
	}
	return id
}

// A gateway hearing a LOUD node (snr 9 -> margin 21.5, too high) and an IN-BAND node
// (snr -4 -> margin 8.5). SF9 floor is -12.5.
func snap(now time.Time) topo.Snapshot {
	return topo.Snapshot{
		Gateway: gwID,
		Nodes: []topo.Node{
			{ID: gwID, IsGateway: true},
			{ID: aID, SF: 9, Power: 22}, // loud -> should lower
			{ID: bID, SF: 9, Power: 14}, // in band -> hold
		},
		Links: []topo.Link{
			{From: aID, To: gwID, RSSI: -42, SNR: 9, At: now},
			{From: bID, To: gwID, RSSI: -95, SNR: -4, At: now},
		},
	}
}

func newLogger(t *testing.T) *Logger {
	t.Helper()
	l, err := NewLogger(filepath.Join(t.TempDir(), "policy.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	return l
}

func find(ds []Decision, node string) Decision {
	for _, d := range ds {
		if d.Node == node {
			return d
		}
	}
	return Decision{}
}

func TestEngineDryRun(t *testing.T) {
	eng := NewEngine(DefaultConfig(), newLogger(t), nil, nil, false, time.Minute, time.Hour)
	ds := eng.Tick(snap(time.Now()), time.Now())
	if d := find(ds, aID); d.Action != Lower || d.NewTarget != 19 {
		t.Fatalf("loud node: %+v want Lower->19", d)
	}
	if d := find(ds, bID); d.Action != Hold {
		t.Fatalf("in-band node: %+v want Hold", d)
	}
}

// Mesh behaviour: a node's single TX power must satisfy its WEAKEST listener. A node that
// blasts the gateway (margin 21.5, would lower in isolation) but is only marginally heard by
// a far neighbour (q=0.3 -> margin ~3) must RAISE, not lower — the far link governs.
func TestEngineWorstLinkGoverns(t *testing.T) {
	now := time.Now()
	s := topo.Snapshot{
		Gateway: "GW000001",
		Nodes: []topo.Node{
			{ID: "GW000001", IsGateway: true},
			{ID: "MESH0001", SF: 9, Power: 10},
		},
		Links: []topo.Link{
			{From: "MESH0001", To: "GW000001", RSSI: -42, SNR: 9, At: now}, // loud to gateway
			{From: "MESH0001", To: "FARN0002", Q: 0.3, At: now},            // marginal to far node
		},
	}
	eng := NewEngine(DefaultConfig(), newLogger(t), nil, nil, false, time.Minute, time.Hour)
	d := find(eng.Tick(s, now), "MESH0001")
	if d.Action != Raise || d.Governs != "FARN0002" {
		t.Fatalf("worst-link should govern: %+v want Raise governed by FARN0002", d)
	}
}

// Connectivity-floor: a node loud to the gateway (margin 21.5, would lower) but only marginally
// heard by a far peer F. Classic worst-neighbour RAISES (the F link governs). But F is reachable
// via a repeater, so under the floor the weak N->F link is redundant and fades — N is governed by
// its strong gateway uplink and LOWERS. Same snapshot, opposite verdict: the whole point.
func connFloorSnap(now time.Time) topo.Snapshot {
	return topo.Snapshot{
		Gateway: gwID,
		Nodes: []topo.Node{
			{ID: gwID, IsGateway: true},
			{ID: nID, SF: 9, Power: 10}, // mid-range so a raise/lower isn't clamped
			{ID: rID, SF: 9, Power: 14},
			{ID: fID, SF: 9, Power: 14},
		},
		Links: []topo.Link{
			{From: nID, To: gwID, RSSI: -42, SNR: 9, At: now}, // N's strong uplink (margin 21.5)
			{From: nID, To: fID, Q: 0.3, At: now},             // N's weak link to F (margin ~3)
			{From: rID, To: gwID, RSSI: -50, SNR: 7, At: now}, // repeater <-> gateway
			{From: fID, To: rID, RSSI: -60, SNR: 5, At: now},  // F is reachable via the repeater
		},
	}
}

func TestEngineConnFloorFadesRedundantLink(t *testing.T) {
	now := time.Now()
	s := connFloorSnap(now)
	// Classic worst-neighbour: the weak N->F link governs -> RAISE.
	clas := NewEngine(DefaultConfig(), newLogger(t), nil, nil, false, time.Minute, time.Hour)
	if d := find(clas.Tick(s, now), nID); d.Action != Raise {
		t.Fatalf("classic: %+v want Raise (weak far link governs)", d)
	}
	// Connectivity-floor keep=1: F is covered by the repeater, so N->F fades and the strong
	// gateway uplink governs -> LOWER.
	cfg := DefaultConfig()
	cfg.ConnFloor = 1
	cf := NewEngine(cfg, newLogger(t), nil, nil, false, time.Minute, time.Hour)
	if d := find(cf.Tick(s, now), nID); d.Action != Lower || d.Governs != gwID {
		t.Fatalf("conn-floor: %+v want Lower governed by gateway", d)
	}
}

// Connectivity-floor must NOT strand a dependent: if a leaf reaches the gateway ONLY through this
// node, its weak link is critical and is kept even though it's the node's worst link -> RAISE.
func TestEngineConnFloorKeepsDependentLink(t *testing.T) {
	now := time.Now()
	s := topo.Snapshot{
		Gateway: gwID,
		Nodes: []topo.Node{
			{ID: gwID, IsGateway: true},
			{ID: nID, SF: 9, Power: 10}, // mid-range so the protective raise isn't clamped
			{ID: lID, SF: 9, Power: 14},
		},
		Links: []topo.Link{
			{From: nID, To: gwID, RSSI: -42, SNR: 9, At: now}, // strong uplink
			{From: nID, To: lID, Q: 0.3, At: now},             // weak link to a leaf that depends on N
			{From: lID, To: nID, RSSI: -60, SNR: 5, At: now},  // L only connects back through N
		},
	}
	cfg := DefaultConfig()
	cfg.ConnFloor = 1
	cf := NewEngine(cfg, newLogger(t), nil, nil, false, time.Minute, time.Hour)
	if d := find(cf.Tick(s, now), nID); d.Action != Raise || d.Governs != lID {
		t.Fatalf("conn-floor must keep the dependent leaf: %+v want Raise governed by %s", d, lID)
	}
}

// Sanity: ConnFloor=0 leaves the classic worst-neighbour verdict identical.
func TestEngineConnFloorDisabledMatchesClassic(t *testing.T) {
	now := time.Now()
	s := connFloorSnap(now)
	cfg := DefaultConfig()
	cfg.ConnFloor = 0
	eng := NewEngine(cfg, newLogger(t), nil, nil, false, time.Minute, time.Hour)
	if d := find(eng.Tick(s, now), nID); d.Action != Raise {
		t.Fatalf("conn-floor disabled should match classic worst-neighbour: %+v want Raise", d)
	}
}

// Serialisation: with Settle>0 and two loud nodes both wanting to lower, only ONE changes per
// settle window; the other is deferred until the window elapses — so neighbours never retune at once.
func TestEngineSerialisesPowerChanges(t *testing.T) {
	now := time.Now()
	s := topo.Snapshot{
		Gateway: gwID,
		Nodes: []topo.Node{
			{ID: gwID, IsGateway: true},
			{ID: aID, SF: 9, Power: 22}, // loud -> wants to lower
			{ID: nID, SF: 9, Power: 22}, // loud -> wants to lower
		},
		Links: []topo.Link{
			{From: aID, To: gwID, RSSI: -42, SNR: 9, At: now},
			{From: nID, To: gwID, RSSI: -42, SNR: 9, At: now},
		},
	}
	cfg := DefaultConfig()
	cfg.Settle = time.Minute
	// fresh window > the test's 90s span so the static links don't age out (in production beacons
	// keep links fresh every ~25s, so the settle window never outruns freshness).
	eng := NewEngine(cfg, newLogger(t), nil, nil, false, 10*time.Minute, time.Hour)

	count := func(ds []Decision) (lowers, serDefers int) {
		for _, d := range ds {
			if d.Action == Lower {
				lowers++
			}
			if d.Action == Hold && strings.Contains(d.Reason, "serialised") {
				serDefers++
			}
		}
		return
	}
	// t0: exactly one lowers, the other is serialised-deferred.
	if l, sd := count(eng.Tick(s, now)); l != 1 || sd != 1 {
		t.Fatalf("t0: want 1 lower + 1 serialised defer, got lowers=%d defers=%d", l, sd)
	}
	// t0+30s (inside the 60s window): nothing changes.
	if l, _ := count(eng.Tick(s, now.Add(30*time.Second))); l != 0 {
		t.Fatalf("within settle window a change leaked: %d lowers", l)
	}
	// t0+90s (window elapsed): one node changes again.
	if l, _ := count(eng.Tick(s, now.Add(90*time.Second))); l != 1 {
		t.Fatalf("after settle: want exactly 1 lower, got %d", l)
	}
}

// Criticality: a loaded backbone link gets a margin reserve, so a node that would normally trim
// instead holds it up. Same margin, opposite verdict depending on the link's routed load.
func TestCriticalityReserveHardensLoadedLink(t *testing.T) {
	cfg := DefaultConfig() // band [6,12], reserve_db 8
	obs := Observation{Node: "NODE0001", SF: 9, HasObs: true, Margin: 13, SNR: 0.5, GovPeer: "PEER0002"}
	// Baseline: margin 13 is above the band -> Lower (trim the loud link).
	if d := Decide(obs, 10, cfg); d.Action != Lower {
		t.Fatalf("baseline margin 13: want Lower, got %v", d.Action)
	}
	// Fully-loaded backbone link + criticality on: band shifts up +8 to [14,20], so margin 13 is
	// now BELOW band -> Raise (keep the backbone robust) with the reserve recorded.
	cfg.Criticality = true
	obs.Load = 1.0
	d := Decide(obs, 10, cfg)
	if d.Action != Raise || d.Reserve != 8 {
		t.Fatalf("loaded backbone: want Raise +8dB reserve, got action=%v reserve=%v", d.Action, d.Reserve)
	}
}

// The generic tuning registry: list knobs, set one, clamp to range, reject unknown.
func TestTuneRegistry(t *testing.T) {
	eng := NewEngine(DefaultConfig(), newLogger(t), nil, nil, false, time.Minute, time.Hour)
	if len(eng.Tunables()) == 0 {
		t.Fatal("no tunables registered")
	}
	if _, err := eng.SetTune("governor", 2); err != nil {
		t.Fatalf("set governor: %v", err)
	}
	if s, err := eng.SetTune("reserve_db", 999); err != nil || s.Value != 15 { // clamp to Max
		t.Fatalf("clamp reserve_db: err=%v val=%v", err, s.Value)
	}
	if _, err := eng.SetTune("nope", 1); err == nil {
		t.Fatal("unknown key should error")
	}
	for _, s := range eng.Tunables() {
		if s.Key == "governor" && s.Value != 2 {
			t.Fatalf("governor not reflected: %v", s.Value)
		}
	}
}

// A node reachable only by quality-only (telemetry) links: a weak one raises (low q is
// trustworthy), but a "loud" one holds — the q->SNR estimate saturates, so we never trim
// power blind on it.
func TestEngineQualityOnlyLinks(t *testing.T) {
	now := time.Now()
	s := topo.Snapshot{
		Gateway: "GW000001",
		Nodes: []topo.Node{
			{ID: "GW000001", IsGateway: true},
			{ID: "WEAK0001", SF: 9, Power: 10}, // q=0.3 -> margin ~3 -> raise
			{ID: "LOUD0001", SF: 9, Power: 22}, // q=1.0 -> margin ~20 but soft -> hold
		},
		Links: []topo.Link{
			{From: "WEAK0001", To: "PEER0009", Q: 0.3, At: now},
			{From: "LOUD0001", To: "PEER0009", Q: 1.0, At: now},
		},
	}
	eng := NewEngine(DefaultConfig(), newLogger(t), nil, nil, false, time.Minute, time.Hour)
	ds := eng.Tick(s, now)
	if d := find(ds, "WEAK0001"); d.Action != Raise || !d.Soft {
		t.Fatalf("weak quality-only node: %+v want Raise (soft)", d)
	}
	if d := find(ds, "LOUD0001"); d.Action != Hold || !d.Soft {
		t.Fatalf("loud quality-only node must hold (no blind trim): %+v", d)
	}
}

// Mobile strategy: a loud measured node holds for MobileLowerHold cycles (hysteresis), then
// trims SLOWLY (by MobileLowerStep) — keeping movement headroom but not pinning it forever.
func TestEngineMobileSlowTrim(t *testing.T) {
	cfg := DefaultConfig()
	now := time.Now()
	s := topo.Snapshot{
		Gateway: "GW000001",
		Nodes: []topo.Node{
			{ID: "GW000001", IsGateway: true},
			{ID: "MOVE0001", SF: 9, Power: 22, Mobile: true}, // margin 21.5, measured -> over the reserve band
		},
		Links: []topo.Link{{From: "MOVE0001", To: "GW000001", RSSI: -42, SNR: 9, At: now}},
	}
	eng := NewEngine(cfg, newLogger(t), nil, nil, false, time.Minute, time.Hour)
	for i := 1; i < cfg.MobileLowerHold; i++ { // first N-1 cycles: hold (sustained-strong gate)
		if d := find(eng.Tick(s, now), "MOVE0001"); d.Action != Hold {
			t.Fatalf("cycle %d: mobile should hold for hysteresis, got %+v", i, d)
		}
	}
	if d := find(eng.Tick(s, now), "MOVE0001"); d.Action != Lower || d.Delta != -cfg.MobileLowerStep {
		t.Fatalf("after %d cycles mobile should trim by %d, got %+v", cfg.MobileLowerHold, cfg.MobileLowerStep, d)
	}
}

// Mobile raises FAST and on a higher band: a node a fixed peer would hold (in [6,12]) is below
// the mobile reserve band [12,18], so it raises — by up to MobileUpStep, not MaxStep.
func TestEngineMobileRaisesFast(t *testing.T) {
	cfg := DefaultConfig()
	now := time.Now()
	s := topo.Snapshot{
		Gateway: "GW000001",
		Nodes: []topo.Node{
			{ID: "GW000001", IsGateway: true},
			{ID: "MOVE0001", SF: 9, Power: 5, Mobile: true}, // snr -6 -> margin 6.5: in [6,12] (fixed holds) but < 12 (mobile raises)
		},
		Links: []topo.Link{{From: "MOVE0001", To: "GW000001", RSSI: -100, SNR: -6, At: now}},
	}
	eng := NewEngine(cfg, newLogger(t), nil, nil, false, time.Minute, time.Hour)
	d := find(eng.Tick(s, now), "MOVE0001")
	if d.Action != Raise || !d.Mobile {
		t.Fatalf("weak mobile should raise on the reserve band: %+v", d)
	}
	if d.Delta > cfg.MobileUpStep || d.Delta <= cfg.MaxStep {
		t.Fatalf("mobile raise step %d should use MobileUpStep (%d), bigger than MaxStep (%d)", d.Delta, cfg.MobileUpStep, cfg.MaxStep)
	}
}

func decodeCtrlsend(t *testing.T, line string, pub ed25519.PublicKey) sign.Command {
	t.Helper()
	f := strings.Fields(line)
	if len(f) != 2 || f[0] != "ctrlsend" {
		t.Fatalf("not a ctrlsend line: %q", line)
	}
	raw, err := hex.DecodeString(f[1])
	if err != nil {
		t.Fatal(err)
	}
	c, err := sign.VerifyControl(raw, pub, 0)
	if err != nil {
		t.Fatalf("verify: %v", err)
	}
	return c
}

// Apply mode: the loud node gets a real signed POWER decrease, and on the next cycle —
// because it's still heard — a CONFIRM (the dead-man dance).
func TestEngineApplyAndConfirm(t *testing.T) {
	ks, err := keystore.Mint(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	var sent []string
	send := func(s string) error { sent = append(sent, s); return nil }
	eng := NewEngine(DefaultConfig(), newLogger(t), ks, send, true, time.Minute, time.Hour)
	pub := ks.Pub()

	// Cycle 1: expect a signed POWER 22->19 to the loud node.
	eng.Tick(snap(time.Now()), time.Now())
	var gotPower bool
	for _, l := range sent {
		if c := decodeCtrlsend(t, l, pub); c.Cmd == sign.CmdPower && c.Target == mustNID(aID) && c.Arg == 19 {
			gotPower = true
		}
	}
	if !gotPower {
		t.Fatalf("cycle1: no POWER 22->19 to AAAA0001 in %v", sent)
	}

	// Cycle 2: node still heard -> the pending decrease is CONFIRMed.
	sent = nil
	eng.Tick(snap(time.Now()), time.Now())
	var gotConfirm bool
	for _, l := range sent {
		if c := decodeCtrlsend(t, l, pub); c.Cmd == sign.CmdConfirm && c.Target == mustNID(aID) && c.Arg == 19 {
			gotConfirm = true
		}
	}
	if !gotConfirm {
		t.Fatalf("cycle2: no CONFIRM of the decrease in %v", sent)
	}
}
