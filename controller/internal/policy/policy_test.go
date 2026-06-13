package policy

import "testing"

func TestDecide(t *testing.T) {
	cfg := DefaultConfig() // band [6,12], mid 9, step 3, power [-9,22], SF9 limit -12.5
	lim := SNRLimit(9)     // -12.5

	cases := []struct {
		name      string
		snr       float64
		hasObs    bool
		soft      bool
		curTarget int8
		want      Action
		newTarget int8
	}{
		// margin = snr - (-12.5). Heard loud (margin 12.5 > 12) -> lower, clamped to step 3.
		{"loud->lower", 0.0, true, false, 22, Lower, 19},
		// margin 0 < 6 -> raise, clamped to step 3.
		{"weak->raise", lim, true, false, 10, Raise, 13},
		// margin 9 in band -> hold.
		{"inband->hold", lim + 9, true, false, 14, Hold, 14},
		// no observation -> skip.
		{"noobs->skip", 0, false, false, 22, Skip, 22},
		// want to raise but already at max power -> hold (at clamp).
		{"raise-at-clamp", lim, true, false, 22, Hold, 22},
		// very loud: delta is still capped at MaxStep (3), not the full margin.
		{"loud-step-capped", 20, true, false, 22, Lower, 19},
		// loud BUT the governing link is quality-only (saturates) -> hold, never trim blind.
		{"loud-but-soft->hold", 20, true, true, 22, Hold, 22},
		// weak via a quality-only link is still trustworthy -> raise.
		{"weak-soft->raise", lim, true, true, 10, Raise, 13},
	}
	for _, c := range cases {
		d := Decide(Observation{Node: "AABBCCDD", SF: 9, SNR: c.snr, Margin: c.snr - lim, HasObs: c.hasObs, Soft: c.soft}, c.curTarget, cfg)
		if d.Action != c.want {
			t.Errorf("%s: action=%s want %s (margin=%.1f reason=%q)", c.name, d.Action, c.want, d.Margin, d.Reason)
		}
		if d.NewTarget != c.newTarget {
			t.Errorf("%s: newTarget=%d want %d", c.name, d.NewTarget, c.newTarget)
		}
		if d.Delta > cfg.MaxStep || d.Delta < -cfg.MaxStep {
			t.Errorf("%s: delta %d exceeds MaxStep %d", c.name, d.Delta, cfg.MaxStep)
		}
	}
}
