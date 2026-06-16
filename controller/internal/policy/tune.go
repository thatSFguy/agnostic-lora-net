package policy

import (
	"fmt"
	"time"
)

// Generic runtime-tuning registry. Every optimiser knob is declared ONCE in tuneKnobs below; the
// HTTP API (/api/tune) and the dashboard render from it automatically. To add or remove a future
// knob, add/remove a Config field and one tuneKnob entry — nothing else to wire. (The dry-run↔APPLY
// toggle is intentionally NOT here: it's a fleet-affecting safety switch with its own confirm.)

// TuneSpec is one knob's self-description — enough for the UI to draw a control and the API to
// validate a change. Value carries the current setting when read.
type TuneSpec struct {
	Key     string   `json:"key"`
	Label   string   `json:"label"`
	Kind    string   `json:"kind"` // "enum" | "bool" | "int" | "float" | "seconds"
	Min     float64  `json:"min,omitempty"`
	Max     float64  `json:"max,omitempty"`
	Step    float64  `json:"step,omitempty"`
	Unit    string   `json:"unit,omitempty"`
	Options []string `json:"options,omitempty"` // "enum": Value is the selected index
	Help    string   `json:"help,omitempty"`
	Value   float64  `json:"value"`
}

// tuneKnob binds a spec to a Config field via get/set. APPEND HERE to add a knob.
type tuneKnob struct {
	spec TuneSpec
	get  func(*Config) float64
	set  func(*Config, float64)
}

func b2f(b bool) float64 {
	if b {
		return 1
	}
	return 0
}

var tuneKnobs = []tuneKnob{
	{
		TuneSpec{Key: "governor", Label: "Governor", Kind: "enum",
			Options: []string{"worst-neighbour", "conn-floor: keep 1", "conn-floor: keep 2", "conn-floor: keep 3"},
			Help:    "How each node's binding link is chosen. Worst-neighbour satisfies every link; connectivity-floor keeps N gateway-ward uplinks and lets redundant links fade."},
		func(c *Config) float64 { return float64(c.ConnFloor) },
		func(c *Config, v float64) { c.ConnFloor = int(v) },
	},
	{
		TuneSpec{Key: "criticality", Label: "Criticality reserve", Kind: "bool",
			Help: "Harden links proportional to routed traffic: backbone links carrying many paths get extra margin and resist trimming; true leaves keep trimming."},
		func(c *Config) float64 { return b2f(c.Criticality) },
		func(c *Config, v float64) { c.Criticality = v != 0 },
	},
	{
		TuneSpec{Key: "reserve_db", Label: "Max reserve", Kind: "float", Min: 0, Max: 15, Step: 1, Unit: "dB",
			Help: "Extra target margin added to a fully-loaded backbone link; scales with the link's routed load."},
		func(c *Config) float64 { return c.ReserveDB },
		func(c *Config, v float64) { c.ReserveDB = v },
	},
	{
		TuneSpec{Key: "settle_s", Label: "Settle window", Kind: "seconds", Min: 0, Max: 600, Step: 15, Unit: "s",
			Help: "Apply at most one node's power change per this window so neighbours never retune at once. 0 = off."},
		func(c *Config) float64 { return c.Settle.Seconds() },
		func(c *Config, v float64) { c.Settle = time.Duration(v) * time.Second },
	},
	{
		TuneSpec{Key: "margin_low", Label: "Raise below margin", Kind: "float", Min: 0, Max: 20, Step: 0.5, Unit: "dB",
			Help: "Below this SNR margin the optimiser raises power."},
		func(c *Config) float64 { return c.MarginLow },
		func(c *Config, v float64) { c.MarginLow = v },
	},
	{
		TuneSpec{Key: "margin_high", Label: "Lower above margin", Kind: "float", Min: 0, Max: 30, Step: 0.5, Unit: "dB",
			Help: "Above this SNR margin the optimiser lowers power."},
		func(c *Config) float64 { return c.MarginHigh },
		func(c *Config, v float64) { c.MarginHigh = v },
	},
	{
		TuneSpec{Key: "max_step", Label: "Max step / cycle", Kind: "int", Min: 1, Max: 6, Step: 1, Unit: "dB",
			Help: "Largest power change applied per cycle (gradual and safe)."},
		func(c *Config) float64 { return float64(c.MaxStep) },
		func(c *Config, v float64) { c.MaxStep = int8(v) },
	},
}

// Tunables returns every knob's spec with its current value. Safe while Tick runs.
func (e *Engine) Tunables() []TuneSpec {
	e.mu.Lock()
	defer e.mu.Unlock()
	out := make([]TuneSpec, len(tuneKnobs))
	for i, k := range tuneKnobs {
		out[i] = k.spec
		out[i].Value = k.get(&e.cfg)
	}
	return out
}

// SetTune sets one knob by key, clamping numeric kinds to [Min,Max]. Unknown key -> error.
func (e *Engine) SetTune(key string, v float64) (TuneSpec, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	for _, k := range tuneKnobs {
		if k.spec.Key != key {
			continue
		}
		if (k.spec.Kind != "bool" && k.spec.Kind != "enum") && k.spec.Max > k.spec.Min {
			if v < k.spec.Min {
				v = k.spec.Min
			}
			if v > k.spec.Max {
				v = k.spec.Max
			}
		}
		k.set(&e.cfg, v)
		s := k.spec
		s.Value = k.get(&e.cfg)
		return s, nil
	}
	return TuneSpec{}, fmt.Errorf("unknown tunable %q", key)
}
