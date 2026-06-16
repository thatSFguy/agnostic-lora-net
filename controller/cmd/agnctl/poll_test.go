package main

import (
	"testing"
	"time"

	"agnostic-lora-net/controller/internal/ingest"
	"agnostic-lora-net/controller/internal/topo"
)

func dataFrame(typ int) ingest.Event {
	return ingest.Event{Kind: ingest.KindFrameRX, Num: map[string]int{"type": typ}}
}

func TestActivityBusyOnlyForMessaging(t *testing.T) {
	t0 := time.Unix(1000, 0)
	a := &activity{}
	// A telemetry frame (PKT_TELEM=5) or beacon must NOT mark the mesh busy.
	a.record(dataFrame(5), t0)
	a.record(ingest.Event{Kind: ingest.KindBeaconRX}, t0)
	if a.busy(t0, 8*time.Second) {
		t.Fatal("telemetry/beacon must not count as messaging-busy")
	}
	// A DATA frame (PKT_DATA=0) does, within the window only.
	a.record(dataFrame(0), t0)
	if !a.busy(t0.Add(3*time.Second), 8*time.Second) {
		t.Fatal("DATA frame should make the mesh busy within the window")
	}
	if a.busy(t0.Add(9*time.Second), 8*time.Second) {
		t.Fatal("should be quiet again after the window")
	}
}

func snapFor(power int, mobile bool, snr int) topo.Snapshot {
	return topo.Snapshot{
		Nodes: []topo.Node{{ID: "NODE0001", Power: power, Mobile: mobile}},
		Links: []topo.Link{{From: "NODE0001", To: "PEER0002", RSSI: -90, SNR: snr}},
	}
}

func TestNodeSigDetectsRealChangesNotJitter(t *testing.T) {
	base := nodeSig(snapFor(14, false, 6), "NODE0001")
	// Same state -> same signature.
	if nodeSig(snapFor(14, false, 6), "NODE0001") != base {
		t.Fatal("identical state must yield identical signature")
	}
	// SNR jitter within a 3 dB bucket (6 -> 7) -> still the same.
	if nodeSig(snapFor(14, false, 7), "NODE0001") != base {
		t.Fatal("sub-bucket SNR jitter must not look like a change")
	}
	// Real shifts must change the signature.
	if nodeSig(snapFor(11, false, 6), "NODE0001") == base {
		t.Fatal("power change must change the signature")
	}
	if nodeSig(snapFor(14, false, 12), "NODE0001") == base {
		t.Fatal("a multi-bucket SNR shift must change the signature")
	}
	// Neighbour set change must change the signature.
	s := snapFor(14, false, 6)
	s.Links = append(s.Links, topo.Link{From: "NODE0001", To: "PEER0003", RSSI: -100, SNR: 2})
	if nodeSig(s, "NODE0001") == base {
		t.Fatal("gaining a neighbour must change the signature")
	}
}

func TestNextIntervalBackoff(t *testing.T) {
	cfg := defaultPollCfg() // base 15s, max 5m, factor 1.6, mobileMax 30s
	// First poll -> base.
	if got := nextInterval(0, true, false, false, cfg); got != cfg.base {
		t.Fatalf("first poll: got %s want %s", got, cfg.base)
	}
	// Unchanged -> grows by factor.
	iv := nextInterval(cfg.base, false, false, false, cfg)
	if iv != time.Duration(float64(cfg.base)*cfg.factor) {
		t.Fatalf("unchanged: got %s want %s", iv, time.Duration(float64(cfg.base)*cfg.factor))
	}
	// Keeps growing but never past max.
	for i := 0; i < 50; i++ {
		iv = nextInterval(iv, false, false, false, cfg)
	}
	if iv != cfg.max {
		t.Fatalf("backoff should saturate at max: got %s want %s", iv, cfg.max)
	}
	// A change snaps it back to base.
	if got := nextInterval(cfg.max, false, true, false, cfg); got != cfg.base {
		t.Fatalf("changed: got %s want %s", got, cfg.base)
	}
	// A mobile node is capped at mobileMax even when "unchanged".
	if got := nextInterval(cfg.max, false, false, true, cfg); got != cfg.mobileMax {
		t.Fatalf("mobile cap: got %s want %s", got, cfg.mobileMax)
	}
}
