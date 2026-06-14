package policy

import (
	"encoding/json"
	"fmt"
	"os"
	"sync"
	"time"
)

// Record is one line of the policy audit trail (JSONL). Decision records embed the full
// Decision; confirm/ack/seed/revert records use Kind + Msg. Everything the optimiser sees
// or does lands here so a surprising power change can always be traced back to its cause.
type Record struct {
	TS      int64  `json:"ts_unix_ms"`
	Kind    string `json:"kind"`           // decision | seed | confirm | revert | ack | note
	Node    string `json:"node,omitempty"` // shallower than Decision.Node -> wins in JSON
	Mode    string `json:"mode,omitempty"`
	Applied bool   `json:"applied,omitempty"`
	Counter uint32 `json:"counter,omitempty"`
	Msg     string `json:"msg,omitempty"`
	*Decision
}

// Logger writes the JSONL audit file, mirrors a human line to stderr, and fans each
// record out to subscribers (e.g. the web dashboard's live feed).
type Logger struct {
	mu   sync.Mutex
	f    *os.File
	enc  *json.Encoder
	subs []func(Record)
}

func NewLogger(path string) (*Logger, error) {
	f, err := os.Create(path)
	if err != nil {
		return nil, err
	}
	return &Logger{f: f, enc: json.NewEncoder(f)}, nil
}

// Subscribe registers a sink called with every record (under the logger lock — keep it
// quick and non-reentrant, e.g. append to a ring buffer).
func (l *Logger) Subscribe(fn func(Record)) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.subs = append(l.subs, fn)
}

func (l *Logger) write(r Record, human string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	_ = l.enc.Encode(r) // JSONL
	fmt.Fprintln(os.Stderr, human)
	for _, fn := range l.subs {
		fn(r)
	}
}

func hhmmss(t time.Time) string { return t.Format("15:04:05") }

// Decision logs a per-node verdict (and, in apply mode, the command that went out).
func (l *Logger) Decision(now time.Time, d Decision, applied bool, ctr uint32, mode string) {
	dd := d
	r := Record{TS: now.UnixMilli(), Kind: "decision", Node: d.Node, Mode: mode, Applied: applied, Counter: ctr, Decision: &dd}
	gov := "" // which neighbour's link bound this decision, and whether it was estimated
	if d.Governs != "" {
		gov = " worst→" + d.Governs
		if d.Soft {
			gov += " (q-est)"
		}
	}
	if d.Mobile {
		gov += " [mobile]"
	}
	var human string
	switch d.Action {
	case Hold:
		human = fmt.Sprintf("%s  %s  SF%d snr=%.1f margin=%.1f%s → HOLD @%ddBm",
			hhmmss(now), d.Node, d.SF, d.HeardSNR, d.Margin, gov, d.CurTarget)
	case Skip:
		human = fmt.Sprintf("%s  %s  → SKIP (%s)", hhmmss(now), d.Node, d.Reason)
	default:
		tag := "[DRY]"
		if applied {
			tag = fmt.Sprintf("[SENT ctr=%d]", ctr)
		}
		human = fmt.Sprintf("%s  %s  SF%d snr=%.1f margin=%.1f%s → %s %d→%ddBm (Δ%+d) %s : %s",
			hhmmss(now), d.Node, d.SF, d.HeardSNR, d.Margin, gov,
			titleUp(string(d.Action)), d.CurTarget, d.NewTarget, d.Delta, tag, d.Reason)
	}
	l.write(r, human)
}

// Event logs a non-decision record (seed/confirm/revert/ack/note).
func (l *Logger) Event(now time.Time, kind, node, msg string) {
	l.write(Record{TS: now.UnixMilli(), Kind: kind, Node: node, Msg: msg},
		fmt.Sprintf("%s  %s  [%s] %s", hhmmss(now), node, kind, msg))
}

func (l *Logger) Close() error { return l.f.Close() }

func titleUp(s string) string {
	if s == "" {
		return s
	}
	return string(s[0]-32) + s[1:]
}
