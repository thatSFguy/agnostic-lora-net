// Package policy is the autonomous RF-optimisation loop (Phase 4 plan §4d). Each cycle it
// reads the live topology, computes for every managed node how much SNR margin the gateway
// hears it with, and nudges that node's TX power toward a target margin band — lower when
// it's heard too loudly (saves airtime/power, the "all nodes turned way down" fix done
// *measured* instead of guessed), higher when it's marginal.
//
// Scope (mesh-wide): a node transmits at one power, so the binding constraint is the
// *weakest outbound link it must keep* — the neighbour that hears it worst. The engine
// gathers every link node->X (how each neighbour hears it) and optimises against the
// minimum margin. As of fw 0.10.0 the telemetry frame carries per-neighbour SNR/RSSI
// (Phase 2), so remote links arrive measured too — and can be trimmed, not just raised. Any
// link still lacking RSSI (older fw, or a neighbour not yet RF-measured) falls back to link
// *quality* (0..1), inverted to an approximate SNR; that estimate saturates at high SNR, so
// a quality-only link can prove a link marginal (-> raise) but never too loud (-> lower) —
// power is only trimmed on a measured SNR.
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

	// Mobile nodes: fast feedback isn't possible over the mesh (cellular-style closed-loop
	// power control runs at ~kHz; ours runs every cycle over a slow shared link), so we
	// approximate it — a higher target margin (movement reserve, the cushion that absorbs
	// motion between cycles) plus an asymmetric response: raise fast, trim slowly and only
	// after the strong margin holds for several cycles (so a transient good reading from a
	// moving node doesn't cut its headroom).
	MobileReserve   float64 // dB added to both band edges for mobile nodes
	MobileUpStep    int8    // raise step for mobile (fast up); <=0 falls back to MaxStep
	MobileLowerStep int8    // trim step for mobile (slow down)
	MobileLowerHold int     // consecutive over-band cycles before a mobile node is trimmed
}

func DefaultConfig() Config {
	return Config{MarginLow: 6, MarginHigh: 12, MaxStep: 3, PowerMin: -9, PowerMax: 22, DefaultSF: 9, SeedPower: 22,
		MobileReserve: 6, MobileUpStep: 5, MobileLowerStep: 1, MobileLowerHold: 3}
}

// Observation is what we know about one managed node this cycle: the worst-case view of
// how its neighbours hear it. Because a node has one shared TX power, the binding link is
// the weakest outbound one it must keep — that's what Margin/SNR describe.
type Observation struct {
	Node    string
	SF      int
	HasObs  bool    // at least one fresh outbound link observed (node is reachable now)
	Margin  float64 // worst (minimum) SNR margin across the node's outbound links (dB)
	SNR     float64 // SNR of that governing link — measured, or estimated from quality
	Soft    bool    // governing link is quality-only (estimate saturates; not safe to trim on)
	Mobile  bool    // node self-reports it moves — higher reserve band, raise fast / trim slow
	GovPeer string  // receiver of the governing (weakest) link, for the audit log
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
	Soft      bool    `json:"soft,omitempty"`    // governing margin estimated from quality, not measured SNR
	Mobile    bool    `json:"mobile,omitempty"`  // node flagged mobile — reserve band, raise fast / trim slow
	Governs   string  `json:"governs,omitempty"` // receiver of the weakest outbound link
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
	d := Decision{Node: obs.Node, SF: obs.SF, HasSNR: obs.HasObs, CurTarget: curTarget, NewTarget: curTarget,
		Soft: obs.Soft, Mobile: obs.Mobile, Governs: obs.GovPeer}
	if !obs.HasObs {
		d.Action = Skip
		d.Reason = "no fresh outbound link observed — no neighbour currently hears this node"
		return d
	}
	margin := obs.Margin // already the worst-link margin (min over outbound links)
	d.HeardSNR, d.Margin = obs.SNR, margin

	// Mobile nodes target a higher band (movement reserve) and respond asymmetrically: raise
	// fast (big step), trim slow (small step). The trim is further gated to N consecutive
	// over-band cycles in the engine — this just sets the per-cycle band/steps.
	low, high := cfg.MarginLow, cfg.MarginHigh
	upStep, downStep := cfg.MaxStep, cfg.MaxStep
	if obs.Mobile {
		low += cfg.MobileReserve
		high += cfg.MobileReserve
		if cfg.MobileUpStep > 0 {
			upStep = cfg.MobileUpStep
		}
		downStep = cfg.MobileLowerStep
	}
	mid := (low + high) / 2

	switch {
	case margin < low:
		// The weakest neighbour that must hear this node is marginal — raise. Reliable even
		// from quality (a low q unambiguously means a weak link). Mobile raises fast.
		want := int8(math.Round(mid - margin))
		d.Delta = clampI8(want, 0, upStep)
	case margin > high:
		// Heard too loudly — trim, EXCEPT a quality-only governing link: the q->SNR estimate
		// saturates at the top, so a "loud" reading can't tell +8 from +30 dB — don't trim
		// blind (fixed and mobile alike).
		if obs.Soft {
			d.Action = Hold
			d.Reason = "margin above band but governing link is quality-only (estimate saturates) — hold rather than trim blind"
			return d
		}
		want := int8(math.Round(margin - mid)) // ~1 dB power ≈ 1 dB SNR
		d.Delta = -clampI8(want, 0, downStep)
	default:
		d.Action = Hold
		if obs.Mobile {
			d.Reason = "mobile: worst-link margin within the reserve band — hold"
		} else {
			d.Reason = "worst-link margin in band — hold"
		}
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
