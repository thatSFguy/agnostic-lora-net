package policy

import (
	"fmt"
	"strconv"
	"sync"
	"time"

	"agnostic-lora-net/controller/internal/commander"
	"agnostic-lora-net/controller/internal/keystore"
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

func parseHexID(s string) uint32 {
	v, _ := strconv.ParseUint(s, 16, 32)
	return uint32(v)
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
	var out []Decision
	for _, n := range snap.Nodes {
		if n.IsGateway || n.ID == snap.Gateway {
			continue
		}
		sf := n.SF
		if sf <= 0 {
			sf = e.cfg.DefaultSF
		}
		obs := observe(snap, n.ID, sf, now, e.fresh)
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

// NoteAck records a node's control ACK in the audit trail (correlated by node + time with
// the decision that triggered it).
func (e *Engine) NoteAck(now time.Time, node string, applied, provisional int) {
	e.log.Event(now, "ack", node, fmt.Sprintf("node ACK: applied=%ddBm provisional=%d", applied, provisional))
}
