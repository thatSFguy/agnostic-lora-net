package topo

import (
	"testing"
	"time"

	"agnostic-lora-net/controller/internal/ingest"
)

// feed parses a console line and folds it into the graph at time t.
func feed(g *Graph, t time.Time, line string) {
	e, _ := ingest.ParseLine(line)
	g.Apply(e, t)
}

// A remote node's telemetry reply (`[status] N` then indented `nbr X …` lines) must build a
// real N<->X link keyed by the actual endpoints — NOT a spoke to the gateway. This is the
// whole point of Phase 1: optimising nodes that never talk to the gateway directly.
func TestRemoteTelemetryBuildsNodeToNodeLinks(t *testing.T) {
	g := New()
	now := time.Unix(1_000_000, 0)

	// Gateway's own info block: its `nbr` lines are gateway spokes (carry measured SNR).
	feed(g, now, "node AAAA0001 neighbors=1 routes=0 blocked=0")
	feed(g, now, "nbr BBBB0002 q_rx=90 q_tx=80 rssi=-50 snr=5")

	// Remote node CCCC0003's telemetry reply: its neighbour DDDD0004 is nowhere near the
	// gateway. q_rx = C hears D (link D->C); q_tx = D hears C (link C->D).
	feed(g, now, "[status] CCCC0003 fw=x up=10min sf=9 pwr=14 batt=? mob=1")
	feed(g, now, "  nbr DDDD0004 q_rx=70 q_tx=60")

	s := g.Snapshot()
	byKey := map[string]Link{}
	byNode := map[string]Node{}
	for _, l := range s.Links {
		byKey[l.From+">"+l.To] = l
	}
	for _, n := range s.Nodes {
		byNode[n.ID] = n
	}
	// The node self-reported mobile (mob=1) in its telemetry status line.
	if !byNode["CCCC0003"].Mobile {
		t.Fatalf("CCCC0003 should be Mobile (reported mob=1), got %+v", byNode["CCCC0003"])
	}
	if byNode["DDDD0004"].Mobile {
		t.Fatalf("DDDD0004 reported no mob= — should default to fixed")
	}

	// Gateway spoke, with measured SNR preserved.
	if l, ok := byKey["BBBB0002>AAAA0001"]; !ok || l.SNR != 5 || l.RSSI != -50 {
		t.Fatalf("gateway spoke BBBB0002->AAAA0001 wrong: %+v ok=%v", l, ok)
	}
	// True node<->node links from remote telemetry, neither endpoint the gateway.
	if l, ok := byKey["CCCC0003>DDDD0004"]; !ok || l.Q != 0.6 {
		t.Fatalf("want C->D link q=0.6, got %+v ok=%v", l, ok)
	}
	if l, ok := byKey["DDDD0004>CCCC0003"]; !ok || l.Q != 0.7 {
		t.Fatalf("want D->C link q=0.7, got %+v ok=%v", l, ok)
	}
	// The remote neighbour table must NOT have been mis-attributed to the gateway.
	if _, ok := byKey["DDDD0004>AAAA0001"]; ok {
		t.Fatalf("remote nbr wrongly anchored to gateway (DDDD0004->AAAA0001 should not exist)")
	}
	// ManagedIDs excludes the gateway, includes the rest.
	ids := g.ManagedIDs()
	want := map[string]bool{"BBBB0002": true, "CCCC0003": true, "DDDD0004": true}
	if len(ids) != len(want) {
		t.Fatalf("ManagedIDs=%v want keys %v", ids, want)
	}
	for _, id := range ids {
		if id == "AAAA0001" {
			t.Fatalf("ManagedIDs must exclude the gateway, got %v", ids)
		}
	}
}
