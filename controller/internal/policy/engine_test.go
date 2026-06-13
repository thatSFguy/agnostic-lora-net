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

// A gateway hearing a LOUD node (snr 9 -> margin 21.5, too high) and an IN-BAND node
// (snr -4 -> margin 8.5). SF9 floor is -12.5.
func snap(now time.Time) topo.Snapshot {
	return topo.Snapshot{
		Gateway: "GW000001",
		Nodes: []topo.Node{
			{ID: "GW000001", IsGateway: true},
			{ID: "AAAA0001", SF: 9, Power: 22}, // loud -> should lower
			{ID: "BBBB0002", SF: 9, Power: 14}, // in band -> hold
		},
		Links: []topo.Link{
			{From: "AAAA0001", To: "GW000001", RSSI: -42, SNR: 9, At: now},
			{From: "BBBB0002", To: "GW000001", RSSI: -95, SNR: -4, At: now},
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
	if d := find(ds, "AAAA0001"); d.Action != Lower || d.NewTarget != 19 {
		t.Fatalf("loud node: %+v want Lower->19", d)
	}
	if d := find(ds, "BBBB0002"); d.Action != Hold {
		t.Fatalf("in-band node: %+v want Hold", d)
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
		if c := decodeCtrlsend(t, l, pub); c.Cmd == sign.CmdPower && c.Target == 0xAAAA0001 && c.Arg == 19 {
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
		if c := decodeCtrlsend(t, l, pub); c.Cmd == sign.CmdConfirm && c.Target == 0xAAAA0001 && c.Arg == 19 {
			gotConfirm = true
		}
	}
	if !gotConfirm {
		t.Fatalf("cycle2: no CONFIRM of the decrease in %v", sent)
	}
}
