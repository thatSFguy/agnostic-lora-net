package policy

import (
	"fmt"
	"math"
	"sort"
	"sync"
	"time"

	"agnostic-lora-net/controller/internal/commander"
	"agnostic-lora-net/controller/internal/keystore"
	"agnostic-lora-net/controller/internal/sign"
	"agnostic-lora-net/controller/internal/topo"
)

// linkLoad computes the routed "load" of every directed fresh+usable link: the share of all-pairs
// quality-weighted shortest paths that cross it, normalised to [0,1]. Per-link cost is 1/q — the
// SAME metric the firmware's DV uses (tx_cost = 1/q_tx) — so the paths it counts are the ones the
// mesh actually routes data over (shortcuts included). Keyed "from>to". Empty when no graph.
func linkLoad(snap topo.Snapshot, now time.Time, fresh time.Duration) map[string]float64 {
	type edge struct {
		to   string
		cost float64
	}
	adj := map[string][]edge{}
	nodes := map[string]struct{}{}
	for _, l := range snap.Links {
		if now.Sub(l.At) >= fresh {
			continue
		}
		q := l.Q
		if q <= 0 {
			if l.RSSI == 0 {
				continue // no quality and no measurement — unusable
			}
			q = 0.5 // measured but no q reported — assume mid
		}
		if q < 0.05 {
			q = 0.05 // clamp so a near-dead link is costly but finite (matches firmware Q_MIN)
		}
		adj[l.From] = append(adj[l.From], edge{l.To, 1.0 / q})
		nodes[l.From], nodes[l.To] = struct{}{}, struct{}{}
	}
	load := map[string]float64{}
	for src := range nodes {
		dist := map[string]float64{}
		prev := map[string]string{}
		visited := map[string]bool{}
		for id := range nodes {
			dist[id] = math.Inf(1)
		}
		dist[src] = 0
		for {
			u, best := "", math.Inf(1)
			for id := range nodes {
				if !visited[id] && dist[id] < best {
					best, u = dist[id], id
				}
			}
			if u == "" {
				break
			}
			visited[u] = true
			for _, e := range adj[u] {
				if nd := dist[u] + e.cost; nd < dist[e.to] {
					dist[e.to], prev[e.to] = nd, u
				}
			}
		}
		for dst := range nodes {
			if dst == src || math.IsInf(dist[dst], 1) {
				continue
			}
			for cur := dst; cur != src; {
				p, ok := prev[cur]
				if !ok {
					break
				}
				load[p+">"+cur]++
				cur = p
			}
		}
	}
	max := 0.0
	for _, v := range load {
		if v > max {
			max = v
		}
	}
	if max > 0 {
		for k := range load {
			load[k] /= max
		}
	}
	return load
}

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

	targets      map[string]int8      // power the controller currently targets per node
	pending      map[string]int8      // nodes whose decrease awaits a CONFIRM (value = power)
	miss         map[string]int       // cycles a pending node has been unreachable
	lastSent     map[string]time.Time // last time we sent any command to a node
	overband     map[string]int       // mobile nodes: consecutive over-band cycles (slow-trim gate)
	lastMutation time.Time            // last time ANY node's power was changed (for Settle serialisation)
}

// mutationPriority ranks which single node gets to change in a serialised cycle: RAISES
// (under-powered, marginal links) always outrank LOWERS (optimisation); within each, the more
// out-of-band wins (most-marginal raise / loudest lower).
func mutationPriority(d Decision) float64 {
	if d.Action == Raise {
		return 1e6 - d.Margin // any raise >> any lower; lower margin (more marginal) ranks higher
	}
	return d.Margin // lowers: loudest (highest margin) first
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

	// 2) Decide per managed (non-gateway) node — compute every decision first, then apply AT MOST
	// ONE power change per Settle window (serialise mutations so two neighbours never retune under
	// each other). Connectivity-floor needs whole-mesh adjacency; build it once per cycle.
	var gwAdj map[string][]string
	if e.cfg.ConnFloor > 0 {
		gwAdj = adjUndirected(snap, now, e.fresh)
	}
	var load map[string]float64 // criticality: routed load per directed link
	if e.cfg.Criticality {
		load = linkLoad(snap, now, e.fresh)
	}
	type cand struct {
		id  string
		d   Decision
		cur int8
	}
	var cands []cand
	for _, n := range snap.Nodes {
		if n.IsGateway || n.ID == snap.Gateway {
			continue
		}
		// ACL: when membership is configured, only tune verified-and-allowed nodes.
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
		if load != nil && obs.GovPeer != "" {
			obs.Load = load[n.ID+">"+obs.GovPeer] // routed load of THIS node's governing link
		}

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
		if d.Reserve > 0 {
			d.Reason += fmt.Sprintf(" | load %.0f%% → +%.1fdB reserve", d.Load*100, d.Reserve)
		}

		// Mobile slow-trim hysteresis: only trim a mobile node after MobileLowerHold consecutive
		// over-band cycles, so a transient strong reading from a moving node doesn't cut its headroom.
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
		cands = append(cands, cand{n.ID, d, cur})
	}

	// Serialisation: when Settle>0, pick the single node allowed to change this cycle, and only once
	// the previous change has had Settle to settle. Raises outrank lowers (mutationPriority).
	// Settle==0 disables it — every change applies each cycle (the original behaviour).
	serialize := e.cfg.Settle > 0
	pick := -1
	if serialize && now.Sub(e.lastMutation) >= e.cfg.Settle {
		for i := range cands {
			if a := cands[i].d.Action; a != Lower && a != Raise {
				continue
			}
			if pick < 0 || mutationPriority(cands[i].d) > mutationPriority(cands[pick].d) {
				pick = i
			}
		}
	}

	var out []Decision
	for i := range cands {
		id, cur := cands[i].id, cands[i].cur
		d := cands[i].d
		var ctr uint32
		applied := false
		isChange := d.Action == Lower || d.Action == Raise
		switch {
		case isChange && serialize && i != pick:
			// Not this node's turn — defer until the RF settles.
			wait := e.cfg.Settle - now.Sub(e.lastMutation)
			if wait < 0 {
				wait = 0
			}
			was := d.Action
			d.Action, d.Delta, d.NewTarget = Hold, 0, cur
			d.Reason = fmt.Sprintf("serialised: %s deferred — one change per %s, RF settling (%.0fs left)", was, e.cfg.Settle, wait.Seconds())
		case isChange:
			// Apply this change (not serialising, or this is the chosen node).
			if e.apply && e.ks != nil {
				if c, err := e.ks.Next(); err == nil {
					if line, err := commander.Power(parseHexID(id), d.NewTarget, c, e.ks.Priv()); err == nil && e.send(line) == nil {
						applied, ctr = true, c
						e.lastSent[id] = now
						e.targets[id] = d.NewTarget // controller owns the power; track what we set
						if d.Action == Lower {
							e.pending[id] = d.NewTarget // a decrease must be CONFIRMed next cycle
							e.miss[id] = 0
						}
						if serialize {
							e.lastMutation = now
						}
					}
				}
			} else if serialize {
				e.lastMutation = now // dry-run: advance the window so the log previews the cadence
			}
		case d.Action == Hold && e.apply && e.ks != nil && e.heartbeat > 0:
			// Heartbeat: re-assert a held node's runtime power so its #3 watchdog doesn't expire
			// and snap it to the loud flash default. Not an RF change — never gated by Settle.
			if t, ok := e.lastSent[id]; ok && now.Sub(t) >= e.heartbeat {
				if c, err := e.ks.Next(); err == nil {
					if line, err := commander.Power(parseHexID(id), cur, c, e.ks.Priv()); err == nil && e.send(line) == nil {
						e.lastSent[id] = now
						e.log.Event(now, "heartbeat", id, fmt.Sprintf("re-assert %ddBm (keep node watchdog fresh)", cur))
					}
				}
			}
		}
		e.log.Decision(now, d, applied, ctr, e.modeStr())
		out = append(out, d)
	}
	return out
}

// SetApply flips the optimiser between dry-run (log only) and APPLY (send live power commands).
// Safe to call while Tick runs. Applying still requires a controller key — without one Tick's
// e.ks==nil guard makes apply a no-op, so the dashboard refuses the toggle when there's no key.
func (e *Engine) SetApply(on bool) {
	e.mu.Lock()
	e.apply = on
	e.mu.Unlock()
}

// Apply reports whether the optimiser is currently sending live commands (vs dry-run).
func (e *Engine) Apply() bool {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.apply
}

// NoteAck records a node's control ACK in the audit trail (correlated by node + time with
// the decision that triggered it).
func (e *Engine) NoteAck(now time.Time, node string, applied, provisional int) {
	e.log.Event(now, "ack", node, fmt.Sprintf("node ACK: applied=%ddBm provisional=%d", applied, provisional))
}
