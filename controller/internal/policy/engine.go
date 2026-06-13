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
	mu    sync.Mutex
	cfg   Config
	log   *Logger
	ks    *keystore.Store
	send  func(string) error
	apply bool
	fresh time.Duration

	targets map[string]int8 // power the controller currently targets per node
	pending map[string]int8 // nodes whose decrease awaits a CONFIRM (value = power)
	miss    map[string]int  // cycles a pending node has been unreachable
}

func NewEngine(cfg Config, log *Logger, ks *keystore.Store, send func(string) error, apply bool, fresh time.Duration) *Engine {
	return &Engine{
		cfg: cfg, log: log, ks: ks, send: send, apply: apply, fresh: fresh,
		targets: map[string]int8{}, pending: map[string]int8{}, miss: map[string]int{},
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

// gwSNR returns the SNR the gateway currently hears `node` at (link node->gateway), and
// whether it's a fresh, real observation. RSSI==0 means "no measurement" (LoRa RSSI is
// always negative), and a stale link doesn't count as "reachable now".
func gwSNR(snap topo.Snapshot, node string, now time.Time, fresh time.Duration) (float64, bool) {
	for _, l := range snap.Links {
		if l.From == node && l.To == snap.Gateway && l.RSSI != 0 && now.Sub(l.At) < fresh {
			return float64(l.SNR), true
		}
	}
	return 0, false
}

// Tick processes one cycle: confirm/expire pending decreases, then decide + (in apply
// mode) command each managed node. Returns the decisions for inspection/tests.
func (e *Engine) Tick(snap topo.Snapshot, now time.Time) []Decision {
	e.mu.Lock()
	defer e.mu.Unlock()

	// 1) Resolve pending decreases: confirm if the node is still reachable, else let the
	//    firmware's 60 s dead-man revert it (and stop tracking after a couple of misses).
	for node, pwr := range e.pending {
		if _, ok := gwSNR(snap, node, now, e.fresh); ok {
			if e.apply && e.ks != nil {
				if ctr, err := e.ks.Next(); err == nil {
					if line, err := commander.Confirm(parseHexID(node), pwr, ctr, e.ks.Priv()); err == nil && e.send(line) == nil {
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
		snr, ok := gwSNR(snap, n.ID, now, e.fresh)
		obs := Observation{Node: n.ID, SF: sf, HeardSNR: snr, HasSNR: ok}

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

		var ctr uint32
		applied := false
		if e.apply && e.ks != nil && (d.Action == Lower || d.Action == Raise) {
			if c, err := e.ks.Next(); err == nil {
				if line, err := commander.Power(parseHexID(n.ID), d.NewTarget, c, e.ks.Priv()); err == nil && e.send(line) == nil {
					applied, ctr = true, c
					e.targets[n.ID] = d.NewTarget // controller owns the power; track what we set
					if d.Action == Lower {
						e.pending[n.ID] = d.NewTarget // a decrease must be CONFIRMed next cycle
						e.miss[n.ID] = 0
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
