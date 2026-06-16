package policy

import (
	"fmt"
	"sort"
	"sync"
	"time"

	"agnostic-lora-net/controller/internal/commander"
	"agnostic-lora-net/controller/internal/keystore"
	"agnostic-lora-net/controller/internal/sign"
	"agnostic-lora-net/controller/internal/topo"
)

// Engine runs the policy each cycle against a topology snapshot, tracking the power it
// targets per node and managing the decrease->confirm safety dance. All I/O goes through
// the injected send func + keystore so it stays testable.
type Engine struct {
	mu        sync.Mutex
	cfg       Config
	log       *Logger
	ks        *keystore.Store
	send      func(string) error
	apply     bool
	fresh     time.Duration
	heartbeat time.Duration // re-assert a held node this often (keeps its #3 watchdog fresh)

	targets  map[string]int8      // power the controller currently targets per node
	pending  map[string]int8      // nodes whose decrease awaits a CONFIRM (value = power)
	miss     map[string]int       // cycles a pending node has been unreachable
	lastSent map[string]time.Time // last time we sent any command to a node
	overband map[string]int       // mobile nodes: consecutive over-band cycles (slow-trim gate)
}

func NewEngine(cfg Config, log *Logger, ks *keystore.Store, send func(string) error, apply bool, fresh, heartbeat time.Duration) *Engine {
	return &Engine{
		cfg: cfg, log: log, ks: ks, send: send, apply: apply, fresh: fresh, heartbeat: heartbeat,
		targets: map[string]int8{}, pending: map[string]int8{}, miss: map[string]int{},
		lastSent: map[string]time.Time{}, overband: map[string]int{},
	}
}

func (e *Engine) modeStr() string {
	if e.apply {
		return "apply"
	}
	return "dry-run"
}

// parseHexID decodes a node id string to a wire NodeID. On a malformed id it returns the
// zero id, which the commander builders reject — so a bad id can never produce a command.
func parseHexID(s string) sign.NodeID {
	id, _ := sign.ParseNodeID(s)
	return id
}

// SNR_FLOOR_DB / SNR_GOOD_DB mirror lib/mesh/link_metric.h: the firmware maps SNR linearly
// onto link quality across this band. qToSNR inverts that map so a quality-only link (from
// routed telemetry, which carries no SNR) can be placed on the same margin scale. It
// SATURATES: q==1 only means SNR ≥ SNR_GOOD_DB, so the result is a lower bound at the top —
// good for spotting a marginal link, not for judging "too loud" (hence Observation.Soft).
const snrFloorDB, snrGoodDB = -17.0, 8.0

func qToSNR(q float64) float64 { return snrFloorDB + q*(snrGoodDB-snrFloorDB) }

// observe builds the worst-case view of how `node`'s neighbours hear it: it scans every
// fresh outbound link (node->X) and keeps the minimum margin — the weakest link the node's
// single shared TX power must satisfy. Measured SNR is used where present; quality-only
// links estimate SNR via qToSNR and are flagged Soft. HasObs is false when no neighbour
// currently hears the node (it isn't reachable / observable this cycle).
func observe(snap topo.Snapshot, node string, sf int, now time.Time, fresh time.Duration) Observation {
	obs := Observation{Node: node, SF: sf}
	limit := SNRLimit(sf)
	best := 0.0
	for _, l := range snap.Links {
		if l.From != node || now.Sub(l.At) >= fresh {
			continue
		}
		snr, soft := 0.0, true
		switch {
		case l.RSSI != 0: // measured SNR on this link (LoRa RSSI is always negative)
			snr, soft = float64(l.SNR), false
		case l.Q > 0: // quality only (routed telemetry) — estimate, saturates high
			snr = qToSNR(l.Q)
		default:
			continue // no usable signal on this link
		}
		if m := snr - limit; !obs.HasObs || m < best {
			best = m
			obs.HasObs, obs.Margin, obs.SNR, obs.Soft, obs.GovPeer = true, m, snr, soft, l.To
		}
	}
	return obs
}

// --- connectivity-floor governor (experimental, enabled by Config.ConnFloor > 0) ------------
//
// observe (above) keeps EVERY direct link healthy, which over-provisions power: a node blasts to
// satisfy a marginal peer even when a stronger repeater already reaches it. observeConnFloor keeps
// only the links the network needs:
//   - CRITICAL links — to a peer that would be cut off from the gateway if THIS node vanished (a
//     downstream relay/leaf that depends on it) — are always kept.
//   - UPLINKS — to a peer that reaches the gateway independently of this node — are kept only for
//     the best `redundancy` of them (that many good paths to the gateway; the rest may fade).
// Everything else is redundant and allowed to weaken, so the node can turn down. If no link leads
// to the gateway (isolated cluster, or gateway unknown) every link is treated as critical and kept,
// so we never trim a node into the dark — it degrades to the classic worst-neighbour rule.

// adjUndirected builds an undirected adjacency map from fresh, usable links (once per tick). The
// mesh is flood-based and links are ~symmetric, so either direction proves the pair can relay.
func adjUndirected(snap topo.Snapshot, now time.Time, fresh time.Duration) map[string][]string {
	adj := map[string][]string{}
	for _, l := range snap.Links {
		if now.Sub(l.At) >= fresh || (l.RSSI == 0 && l.Q <= 0) {
			continue // stale or no usable signal
		}
		adj[l.From] = append(adj[l.From], l.To)
		adj[l.To] = append(adj[l.To], l.From)
	}
	return adj
}

// reachExcluding returns the nodes reachable from `start` (the gateway) without ever passing
// through `exclude` — i.e. who stays gateway-connected if `exclude` (the node we're tuning) were
// removed. A peer NOT in this set depends on `exclude` to reach the gateway (a critical link).
func reachExcluding(adj map[string][]string, start, exclude string) map[string]bool {
	seen := map[string]bool{}
	if start == "" || start == exclude {
		return seen
	}
	seen[start] = true
	q := []string{start}
	for len(q) > 0 {
		n := q[0]
		q = q[1:]
		for _, m := range adj[n] {
			if m == exclude || seen[m] {
				continue
			}
			seen[m] = true
			q = append(q, m)
		}
	}
	return seen
}

type floorLink struct {
	margin float64
	snr    float64
	soft   bool
	peer   string
	uplink bool // peer reaches the gateway WITHOUT this node (else it depends on us -> critical)
}

func shortID(s string) string {
	if len(s) > 8 {
		return s[:8]
	}
	return s
}

func observeConnFloor(snap topo.Snapshot, node string, sf int, now time.Time, fresh time.Duration, redundancy int, adj map[string][]string) Observation {
	obs := Observation{Node: node, SF: sf}
	limit := SNRLimit(sf)
	reach := reachExcluding(adj, snap.Gateway, node) // peers that keep the gateway without us
	var links []floorLink
	for _, l := range snap.Links {
		if l.From != node || now.Sub(l.At) >= fresh {
			continue
		}
		snr, soft := 0.0, true
		switch {
		case l.RSSI != 0: // measured SNR (LoRa RSSI is always negative)
			snr, soft = float64(l.SNR), false
		case l.Q > 0: // quality only — estimate, saturates high
			snr = qToSNR(l.Q)
		default:
			continue
		}
		links = append(links, floorLink{margin: snr - limit, snr: snr, soft: soft, peer: l.To, uplink: reach[l.To]})
	}
	if len(links) == 0 {
		return obs // HasObs stays false — not reachable this cycle
	}
	if redundancy < 1 {
		redundancy = 1
	}
	// Strongest first: keep the best `redundancy` uplinks and let the weaker uplinks fade. The
	// governing link is the WEAKEST one we decide to keep (critical ∪ best-N uplinks).
	sort.Slice(links, func(i, j int) bool { return links[i].margin > links[j].margin })
	var gov *floorLink
	uplinkRank, kept, faded, crit := 0, 0, 0, 0
	for i := range links {
		keep := false
		if links[i].uplink {
			keep = uplinkRank < redundancy // keep only the best `redundancy` gateway-ward links
			uplinkRank++
		} else {
			keep, crit = true, crit+1 // critical: a peer that depends on us — never drop
		}
		if keep {
			kept++
			if gov == nil || links[i].margin < gov.margin {
				gov = &links[i]
			}
		} else {
			faded++
		}
	}
	if gov == nil { // defensive: nothing kept (only if reach is empty -> all critical, can't happen)
		gov = &links[len(links)-1]
		kept, faded = len(links), 0
	}
	obs.HasObs = true
	obs.Margin, obs.SNR, obs.Soft, obs.GovPeer = gov.margin, gov.snr, gov.soft, gov.peer
	obs.FloorNote = fmt.Sprintf("conn-floor: keep %d/%d links (gov %s m=%.1f, %d critical), %d redundant fading",
		kept, len(links), shortID(gov.peer), gov.margin, crit, faded)
	return obs
}

// Tick processes one cycle: confirm/expire pending decreases, then decide + (in apply
// mode) command each managed node. Returns the decisions for inspection/tests.
func (e *Engine) Tick(snap topo.Snapshot, now time.Time) []Decision {
	e.mu.Lock()
	defer e.mu.Unlock()

	// 1) Resolve pending decreases: confirm if the node is still reachable, else let the
	//    firmware's 60 s dead-man revert it (and stop tracking after a couple of misses).
	for node, pwr := range e.pending {
		if observe(snap, node, e.cfg.DefaultSF, now, e.fresh).HasObs {
			if e.apply && e.ks != nil {
				if ctr, err := e.ks.Next(); err == nil {
					if line, err := commander.Confirm(parseHexID(node), pwr, ctr, e.ks.Priv()); err == nil && e.send(line) == nil {
						e.lastSent[node] = now
						e.log.Event(now, "confirm", node, fmt.Sprintf("still reachable after decrease — CONFIRMed %ddBm (ctr=%d)", pwr, ctr))
					}
				}
			}
			delete(e.pending, node)
			delete(e.miss, node)
		} else {
			e.miss[node]++
			if e.miss[node] >= 2 {
				e.log.Event(now, "revert", node, "decrease unconfirmed — node unreachable; firmware auto-reverts in 60 s")
				delete(e.pending, node)
				delete(e.miss, node)
			}
		}
	}

	// 2) Decide per managed (non-gateway) node.
	// Connectivity-floor mode needs the whole-mesh adjacency to tell each node's redundant links
	// from the ones it must keep — build it once per cycle.
	var gwAdj map[string][]string
	if e.cfg.ConnFloor > 0 {
		gwAdj = adjUndirected(snap, now, e.fresh)
	}
	var out []Decision
	for _, n := range snap.Nodes {
		if n.IsGateway || n.ID == snap.Gateway {
			continue
		}
		// ACL: when membership is configured, only tune verified-and-allowed nodes. ACL=="" means
		// no ACL (replay / no key) → manage everything, as before identity landed.
		if n.ACL != "" && n.ACL != "allowed" {
			continue
		}
		sf := n.SF
		if sf <= 0 {
			sf = e.cfg.DefaultSF
		}
		var obs Observation
		if e.cfg.ConnFloor > 0 {
			obs = observeConnFloor(snap, n.ID, sf, now, e.fresh, e.cfg.ConnFloor, gwAdj)
		} else {
			obs = observe(snap, n.ID, sf, now, e.fresh)
		}
		obs.Mobile = n.Mobile // the node self-reports mobility in telemetry (mob=1)

		cur, seeded := e.targets[n.ID]
		if !seeded {
			cur = e.cfg.SeedPower
			src := "assumed default"
			if n.Power != 0 {
				cur, src = int8(n.Power), "reported by node"
			}
			e.targets[n.ID] = cur
			e.log.Event(now, "seed", n.ID, fmt.Sprintf("managing node — seed target power %ddBm (%s)", cur, src))
		}

		d := Decide(obs, cur, e.cfg)
		if obs.FloorNote != "" {
			d.Reason += " | " + obs.FloorNote // connectivity-floor audit trail
		}

		// Mobile slow-trim hysteresis: only trim a mobile node after MobileLowerHold
		// consecutive over-band cycles, so a transient strong reading from a moving node
		// (it'll drift again) doesn't cut its movement headroom. Raises are never gated.
		if obs.Mobile && d.Action == Lower {
			e.overband[n.ID]++
			if e.overband[n.ID] < e.cfg.MobileLowerHold {
				d.Action, d.Delta, d.NewTarget = Hold, 0, cur
				d.Reason = fmt.Sprintf("mobile: over band, holding for sustained strong margin (%d/%d cycles)", e.overband[n.ID], e.cfg.MobileLowerHold)
			} else {
				e.overband[n.ID] = 0 // sustained — allow this trim, reset the streak
			}
		} else {
			delete(e.overband, n.ID)
		}

		var ctr uint32
		applied := false
		if e.apply && e.ks != nil && (d.Action == Lower || d.Action == Raise) {
			if c, err := e.ks.Next(); err == nil {
				if line, err := commander.Power(parseHexID(n.ID), d.NewTarget, c, e.ks.Priv()); err == nil && e.send(line) == nil {
					applied, ctr = true, c
					e.lastSent[n.ID] = now
					e.targets[n.ID] = d.NewTarget // controller owns the power; track what we set
					if d.Action == Lower {
						e.pending[n.ID] = d.NewTarget // a decrease must be CONFIRMed next cycle
						e.miss[n.ID] = 0
					}
				}
			}
		} else if e.apply && e.ks != nil && d.Action == Hold && e.heartbeat > 0 {
			// Heartbeat: re-assert a held node's runtime power so its node-side #3 watchdog
			// doesn't expire and snap it back to the loud flash default. Only for nodes we've
			// actually commanded (lastSent set).
			if t, ok := e.lastSent[n.ID]; ok && now.Sub(t) >= e.heartbeat {
				if c, err := e.ks.Next(); err == nil {
					if line, err := commander.Power(parseHexID(n.ID), cur, c, e.ks.Priv()); err == nil && e.send(line) == nil {
						e.lastSent[n.ID] = now
						e.log.Event(now, "heartbeat", n.ID, fmt.Sprintf("re-assert %ddBm (keep node watchdog fresh)", cur))
					}
				}
			}
		}
		e.log.Decision(now, d, applied, ctr, e.modeStr())
		out = append(out, d)
	}
	return out
}

// SetConnFloor switches the governor at runtime: 0 = classic worst-neighbour, N>=1 =
// connectivity-floor keeping N gateway-ward uplinks. Safe to call from the dashboard while
// Tick runs (shares e.mu with Tick, which reads cfg.ConnFloor).
func (e *Engine) SetConnFloor(n int) {
	if n < 0 {
		n = 0
	}
	e.mu.Lock()
	e.cfg.ConnFloor = n
	e.mu.Unlock()
}

// Governor returns the current ConnFloor setting (0 = worst-neighbour, N = keep N uplinks).
func (e *Engine) Governor() int {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.cfg.ConnFloor
}

// NoteAck records a node's control ACK in the audit trail (correlated by node + time with
// the decision that triggered it).
func (e *Engine) NoteAck(now time.Time, node string, applied, provisional int) {
	e.log.Event(now, "ack", node, fmt.Sprintf("node ACK: applied=%ddBm provisional=%d", applied, provisional))
}
