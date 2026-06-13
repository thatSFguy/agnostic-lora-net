// Package policy is the autonomous RF-optimisation loop (Phase 4 plan §4d). Each cycle it
// reads the live topology, computes for every managed node how much SNR margin the gateway
// hears it with, and nudges that node's TX power toward a target margin band — lower when
// it's heard too loudly (saves airtime/power, the "all nodes turned way down" fix done
// *measured* instead of guessed), higher when it's marginal.
//
// Scope of this first version (honest): it optimises each non-gateway node against the SNR
// the *tethered gateway* directly measures (the link node->gateway). That's exactly right
// for a star/bench layout and a sound first approximation elsewhere; true global
// per-direction optimisation waits on the telemetry hardening in §4c.
//
// Safety: the controller owns each node's power (POWER sets an absolute dBm), tracks its
// own target per node, step-limits and clamps every change, and — because a power DECREASE
// auto-reverts on-device in 60 s — only CONFIRMs a decrease once it has re-observed the
// node still reachable. So a bad call (or the controller dying) self-heals.
//
// Everything it observes and decides is logged (JSONL + console) for troubleshooting.
package policy

import "math"

// SNRLimit is the demodulation floor (dB) per spreading factor — the same table the map
// app uses. Margin = observed SNR - this.
func SNRLimit(sf int) float64 {
	t := map[int]float64{5: -2.5, 6: -5, 7: -7.5, 8: -10, 9: -12.5, 10: -15, 11: -17.5, 12: -20}
	if v, ok := t[sf]; ok {
		return v
	}
	return -12.5 // default to SF9 (the network default)
}

// Config tunes the loop. Margins are dB above the SF floor; powers are dBm.
type Config struct {
	MarginLow  float64 // below this -> raise power
	MarginHigh float64 // above this -> lower power
	MaxStep    int8    // max dBm change per cycle (gradual, safe)
	PowerMin   int8
	PowerMax   int8
	DefaultSF  int
	SeedPower  int8 // assumed power for a node we haven't commanded yet (if unreported)
}

func DefaultConfig() Config {
	return Config{MarginLow: 6, MarginHigh: 12, MaxStep: 3, PowerMin: -9, PowerMax: 22, DefaultSF: 9, SeedPower: 22}
}

func (c Config) mid() float64 { return (c.MarginLow + c.MarginHigh) / 2 }

// Observation is what we know about one managed node this cycle.
type Observation struct {
	Node     string
	SF       int
	HeardSNR float64 // SNR the gateway hears this node at (link node->gateway)
	HasSNR   bool
}

// Action is the verdict for one node.
type Action string

const (
	Hold  Action = "hold"
	Lower Action = "lower"
	Raise Action = "raise"
	Skip  Action = "skip" // no usable signal (e.g. gateway never heard it with SNR)
)

// Decision is the fully-explained outcome for one node — the unit of the audit log.
type Decision struct {
	Node      string  `json:"node"`
	SF        int     `json:"sf"`
	HeardSNR  float64 `json:"heard_snr,omitempty"`
	HasSNR    bool    `json:"has_snr"`
	Margin    float64 `json:"margin,omitempty"`
	CurTarget int8    `json:"cur_target"`
	Action    Action  `json:"action"`
	Delta     int8    `json:"delta"`
	NewTarget int8    `json:"new_target"`
	Reason    string  `json:"reason"`
}

func clampI8(v, lo, hi int8) int8 {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

// Decide is the pure per-node policy: given the observation and the power the controller
// currently targets for this node, return the (explained) next step. No I/O, no state.
func Decide(obs Observation, curTarget int8, cfg Config) Decision {
	d := Decision{Node: obs.Node, SF: obs.SF, HasSNR: obs.HasSNR, CurTarget: curTarget, NewTarget: curTarget}
	if !obs.HasSNR {
		d.Action = Skip
		d.Reason = "no SNR for node->gateway link (gateway hasn't heard it) — cannot judge margin"
		return d
	}
	limit := SNRLimit(obs.SF)
	margin := obs.HeardSNR - limit
	d.HeardSNR, d.Margin = obs.HeardSNR, margin

	switch {
	case margin > cfg.MarginHigh:
		// Heard too loudly — we can trim power. ~1 dB power ≈ 1 dB SNR.
		want := int8(math.Round(margin - cfg.mid()))
		d.Delta = -clampI8(want, 0, cfg.MaxStep)
	case margin < cfg.MarginLow:
		want := int8(math.Round(cfg.mid() - margin))
		d.Delta = clampI8(want, 0, cfg.MaxStep)
	default:
		d.Action = Hold
		d.Reason = "margin in band — hold"
		return d
	}

	d.NewTarget = clampI8(curTarget+d.Delta, cfg.PowerMin, cfg.PowerMax)
	if d.NewTarget == curTarget {
		d.Action, d.Delta = Hold, 0
		d.Reason = "would change power but already at the clamp limit"
		return d
	}
	if d.NewTarget < curTarget {
		d.Action = Lower
		d.Reason = "margin above band — lower power to save airtime/budget"
	} else {
		d.Action = Raise
		d.Reason = "margin below band — raise power to keep the link"
	}
	return d
}
