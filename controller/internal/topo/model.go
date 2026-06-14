// Package topo maintains the live global graph the controller builds from a node's
// console stream: nodes + their metadata, and per-direction links (who hears whom, and
// how well). It is the Go analogue of the model web/map.html keeps in the browser, but
// it is meant to aggregate the whole mesh (via announce-derived [nbrs] lines), not just
// the tethered gateway's own neighbours.
package topo

import (
	"sort"
	"strings"
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
	Mobile    bool      `json:"mobile,omitempty"`  // node self-reports it moves (telemetry mob=1)
	BLE       *bool     `json:"ble,omitempty"`     // node-reported BLE/BT advertising state (nil = unknown until firmware reports ble=)
	Pub       string    `json:"pub,omitempty"`     // verified node pubkey hex (upper), from a gateway-verified announce
	Verified  bool      `json:"verified"`          // gateway reported sig=ok for this node's announce
	ACL       string    `json:"acl,omitempty"`     // "allowed"|"pending"|"unverified" (derived; "" = no ACL configured)
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

	// reportOwner is whose neighbour table the current `nbr X q_rx q_tx` lines belong to:
	// the gateway after its `node …` info header, or a remote node after its `[status] N`
	// telemetry reply. Lets one parser shape build real node<->node links, not just spokes.
	reportOwner string

	// allow is the membership check (keystore.IsAllowed) injected by the server. nil means
	// no ACL is configured (replay / no controller key) → the graph is permissive: every
	// verified-or-not node is manageable, exactly as before identity landed. When set, only
	// verified-and-allowed nodes are managed/commandable (self-certifying-identity §6).
	allow func(pubHex string) bool
}

func New() *Graph {
	return &Graph{nodes: map[string]*Node{}, links: map[string]*Link{}}
}

// SetAllowFunc installs the membership check (typically keystore.IsAllowed). Call once at
// startup before commands/optimisation run. nil leaves the graph permissive (no ACL).
func (g *Graph) SetAllowFunc(f func(pubHex string) bool) {
	g.mu.Lock()
	defer g.mu.Unlock()
	g.allow = f
}

// aclLabel derives a node's membership label. "" when no ACL is configured. Caller holds mu.
func (g *Graph) aclLabel(n *Node) string {
	if g.allow == nil {
		return ""
	}
	switch {
	case n.Verified && g.allow(n.Pub):
		return "allowed"
	case n.Verified:
		return "pending"
	default:
		return "unverified"
	}
}

// manageable reports whether the controller may tune/command this node. Permissive without
// an ACL; otherwise requires verified + allowlisted. Caller holds mu.
func (g *Graph) manageable(n *Node) bool {
	return g.allow == nil || (n.Verified && g.allow(n.Pub))
}

// CommandAllowed reports whether a signed command may be issued to node id, returning the
// resolved pubkey for messaging. Permissive when no ACL is configured; otherwise the target
// must be verified and on the allowlist. The HTTP/CLI command paths gate on this before
// advancing the replay counter (so a rejected command never burns a counter).
func (g *Graph) CommandAllowed(id string) (pub string, ok bool) {
	g.mu.RLock()
	defer g.mu.RUnlock()
	if g.allow == nil {
		return "", true
	}
	n := g.nodes[strings.ToUpper(id)]
	if n == nil || !n.Verified {
		return "", false
	}
	return n.Pub, g.allow(n.Pub)
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
		g.reportOwner = e.ID // the gateway's own `nbr …` lines follow this header

	case ingest.KindBlocked:
		set := map[string]bool{}
		for _, id := range e.Blocked {
			set[id] = true
		}
		for id, n := range g.nodes {
			n.Blocked = set[id]
		}

	case ingest.KindNbr:
		// A `nbr X q_rx q_tx [rssi snr]` line owned by reportOwner O (the gateway after its
		// info header, or a remote node after its `[status]` telemetry reply): q_rx = O hears
		// X (link X->O); q_tx = X hears O (link O->X). One line gives both directions, keyed by
		// the real endpoints — so remote telemetry yields true node<->node links, not spokes.
		owner := g.reportOwner
		if owner == "" {
			owner = g.Gateway
		}
		if owner == "" {
			return
		}
		src := "telemetry"
		if owner == g.Gateway {
			src = "gateway"
		}
		x := e.ID
		g.node(owner).LastSeen = at
		g.node(x).LastSeen = at
		if v, ok := e.Num["q_rx"]; ok && v > 0 { // O hears X => link X->O
			l := g.link(x, owner, src, at)
			l.Q = float64(v) / 100
			if r, ok := e.Num["rssi"]; ok {
				l.RSSI, l.SNR = r, e.Num["snr"]
			}
		}
		if v, ok := e.Num["q_tx"]; ok && v > 0 { // X hears O => link O->X
			g.link(owner, x, src, at).Q = float64(v) / 100
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
		g.reportOwner = e.ID // remote telemetry reply — its `nbr …` lines belong to this node
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
		if v, ok := e.Num["mob"]; ok {
			n.Mobile = v != 0
		}
		if v, ok := e.Num["ble"]; ok {
			b := v != 0
			n.BLE = &b
		}
		if v, ok := e.Num["mv"]; ok {
			n.BattMv, n.BattPct = v, e.Num["pct"]
		}

	case ingest.KindIdentity:
		// Gateway-verified id↔pubkey binding. We trust the gateway's verdict (Model A): it ran
		// ed25519_check + id==hash(pubkey) before printing sig=ok. ACL is derived at Snapshot
		// time from the live allowlist, so an approve/revoke reflects without a fresh announce.
		n := g.node(e.ID)
		n.LastSeen = at
		if e.SigOK && e.Pub != "" {
			n.Pub, n.Verified = e.Pub, true
		} else {
			n.Verified = false
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

// ManagedIDs returns the non-gateway node IDs (sorted, stable) the controller knows about —
// the set worth polling for remote telemetry. Excludes the tethered gateway itself.
func (g *Graph) ManagedIDs() []string {
	g.mu.RLock()
	defer g.mu.RUnlock()
	var ids []string
	for id, n := range g.nodes {
		if id == g.Gateway || n.IsGateway {
			continue
		}
		if !g.manageable(n) { // ACL configured & node not verified+allowed → don't poll/tune it
			continue
		}
		ids = append(ids, id)
	}
	sort.Strings(ids)
	return ids
}

// GatewayID returns the tethered gateway's node id ("" if not yet known). Used by the
// write API to drive the gateway's BLE directly over the console instead of the mesh.
func (g *Graph) GatewayID() string {
	g.mu.RLock()
	defer g.mu.RUnlock()
	return g.Gateway
}

func (g *Graph) Snapshot() Snapshot {
	g.mu.RLock()
	defer g.mu.RUnlock()
	s := Snapshot{Gateway: g.Gateway, BeaconSec: g.BeaconSec}
	for _, n := range g.nodes {
		c := *n
		c.ACL = g.aclLabel(n) // derive fresh so approve/revoke reflects without a new announce
		s.Nodes = append(s.Nodes, c)
	}
	for _, l := range g.links {
		s.Links = append(s.Links, *l)
	}
	// Map iteration order is randomised per call, so without this the dashboard
	// reshuffles its node list and teleports unpositioned map nodes every poll.
	// Emit a stable order: nodes by ID, links by (from, to).
	sort.Slice(s.Nodes, func(i, j int) bool { return s.Nodes[i].ID < s.Nodes[j].ID })
	sort.Slice(s.Links, func(i, j int) bool {
		if s.Links[i].From != s.Links[j].From {
			return s.Links[i].From < s.Links[j].From
		}
		return s.Links[i].To < s.Links[j].To
	})
	return s
}
