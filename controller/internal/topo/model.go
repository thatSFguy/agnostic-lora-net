// Package topo maintains the live global graph the controller builds from a node's
// console stream: nodes + their metadata, and per-direction links (who hears whom, and
// how well). It is the Go analogue of the model web/map.html keeps in the browser, but
// it is meant to aggregate the whole mesh (via announce-derived [nbrs] lines), not just
// the tethered gateway's own neighbours.
package topo

import (
	"sync"
	"time"

	"agnostic-lora-net/controller/internal/ingest"
)

// Node is one mesh node as the controller currently understands it.
type Node struct {
	ID        string    `json:"id"`
	FW        string    `json:"fw,omitempty"`
	SF        int       `json:"sf,omitempty"`
	Power     int       `json:"power_dbm,omitempty"`
	BattMv    int       `json:"batt_mv,omitempty"`
	BattPct   int       `json:"batt_pct,omitempty"`
	IsGateway bool      `json:"is_gateway,omitempty"`
	Blocked   bool      `json:"blocked,omitempty"` // blocked by the gateway/controller
	LastSeen  time.Time `json:"last_seen"`
}

// Link is a directed edge: receiver `To` hears sender `From` at quality Q (0..1) with
// the last observed RSSI/SNR. Src notes where the observation came from.
type Link struct {
	From string    `json:"from"`
	To   string    `json:"to"`
	Q    float64   `json:"q"`
	RSSI int       `json:"rssi"`
	SNR  int       `json:"snr"`
	Src  string    `json:"src"`
	At   time.Time `json:"at"`
}

// Graph is the controller's world model. Safe for concurrent use.
type Graph struct {
	mu        sync.RWMutex
	Gateway   string
	BeaconSec int
	nodes     map[string]*Node
	links     map[string]*Link // key "FROM>TO"
}

func New() *Graph {
	return &Graph{nodes: map[string]*Node{}, links: map[string]*Link{}}
}

func (g *Graph) node(id string) *Node {
	n := g.nodes[id]
	if n == nil {
		n = &Node{ID: id}
		g.nodes[id] = n
	}
	return n
}

func linkKey(from, to string) string { return from + ">" + to }

func (g *Graph) link(from, to, src string, at time.Time) *Link {
	k := linkKey(from, to)
	l := g.links[k]
	if l == nil {
		l = &Link{From: from, To: to}
		g.links[k] = l
	}
	l.Src, l.At = src, at
	return l
}

// Apply folds one parsed Event into the graph. `at` is the observation time.
func (g *Graph) Apply(e ingest.Event, at time.Time) {
	g.mu.Lock()
	defer g.mu.Unlock()

	switch e.Kind {
	case ingest.KindInfoHeader:
		g.Gateway = e.ID
		n := g.node(e.ID)
		n.IsGateway, n.LastSeen = true, at

	case ingest.KindBlocked:
		set := map[string]bool{}
		for _, id := range e.Blocked {
			set[id] = true
		}
		for id, n := range g.nodes {
			n.Blocked = set[id]
		}

	case ingest.KindNbr:
		// Gateway G's own neighbour X: q_rx = G hears X (link X->G); q_tx = X hears G
		// (link G->X). One line gives us both directions.
		if g.Gateway == "" {
			return
		}
		x := e.ID
		g.node(x).LastSeen = at
		if v, ok := e.Num["q_rx"]; ok {
			l := g.link(x, g.Gateway, "gateway", at)
			l.Q = float64(v) / 100
			if r, ok := e.Num["rssi"]; ok {
				l.RSSI, l.SNR = r, e.Num["snr"]
			}
		}
		if v, ok := e.Num["q_tx"]; ok {
			g.link(g.Gateway, x, "gateway", at).Q = float64(v) / 100
		}

	case ingest.KindAnnNbr:
		// Announce-derived: the gateway has a fresh announce from X (link X->gateway).
		if g.Gateway == "" {
			return
		}
		g.node(e.ID).LastSeen = at
		l := g.link(e.ID, g.Gateway, "announce", at)
		if r, ok := e.Num["rssi"]; ok {
			l.RSSI, l.SNR = r, e.Num["snr"]
		}
		if p, ok := e.Num["pct"]; ok {
			g.node(e.ID).BattPct = p
		}

	case ingest.KindStatus:
		n := g.node(e.ID)
		n.LastSeen = at
		if v, ok := e.Str["fw"]; ok {
			n.FW = v
		}
		if v, ok := e.Num["sf"]; ok {
			n.SF = v
		}
		if v, ok := e.Num["power"]; ok {
			n.Power = v
		}
		if v, ok := e.Num["mv"]; ok {
			n.BattMv, n.BattPct = v, e.Num["pct"]
		}

	case ingest.KindNodeBatt:
		n := g.node(e.ID)
		n.LastSeen = at
		n.BattMv, n.BattPct = e.Num["mv"], e.Num["pct"]

	case ingest.KindFW:
		if g.Gateway != "" {
			g.node(g.Gateway).FW = e.Str["fw"]
		}

	case ingest.KindBatt:
		if g.Gateway != "" {
			n := g.node(g.Gateway)
			if v, ok := e.Num["mv"]; ok {
				n.BattMv, n.BattPct = v, e.Num["pct"]
			}
		}

	case ingest.KindRF:
		if g.Gateway != "" {
			n := g.node(g.Gateway)
			n.SF, n.Power = e.Num["sf"], e.Num["power"]
		}

	case ingest.KindBeaconCfg:
		g.BeaconSec = e.Num["beacon_s"]

	case ingest.KindBeaconRX, ingest.KindFrameRX, ingest.KindBeaconTX:
		if e.ID != "" {
			g.node(e.ID).LastSeen = at
		}
	}
}

// Snapshot returns a stable copy of the graph for serialisation (future httpd / export).
type Snapshot struct {
	Gateway   string `json:"gateway"`
	BeaconSec int    `json:"beacon_s"`
	Nodes     []Node `json:"nodes"`
	Links     []Link `json:"links"`
	At        int64  `json:"at_unix_ms"`
}

func (g *Graph) Snapshot() Snapshot {
	g.mu.RLock()
	defer g.mu.RUnlock()
	s := Snapshot{Gateway: g.Gateway, BeaconSec: g.BeaconSec}
	for _, n := range g.nodes {
		s.Nodes = append(s.Nodes, *n)
	}
	for _, l := range g.links {
		s.Links = append(s.Links, *l)
	}
	return s
}
