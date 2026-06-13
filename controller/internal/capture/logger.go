// Package capture is the passive RF-utilisation recorder (Phase 4 plan §8). It writes
// one CSV row per parsed console event and keeps rolling per-kind counts, so a tethered
// node + this controller is a zero-extra-hardware "airtime recorder" you can run on the
// next bench/field pass and analyse offline.
//
// Caveat (honest): from the console stream alone we have frame *counts and classes*
// (with `trace on`, every beacon TX/RX and forwarded frame is a row) but NOT true
// time-on-air. Real ToA-by-class needs the node-side instrumentation in plan §8; this
// logger is the harness that data will plug into. Counts already expose the dominant
// chattiness signal (beacon overhead, forward volume, retransmits).
package capture

import (
	"encoding/csv"
	"fmt"
	"os"
	"sort"
	"strconv"
	"sync"
	"time"

	"agnostic-lora-net/controller/internal/ingest"
)

var header = []string{"ts_unix_ms", "kind", "airframe", "id", "peer", "type", "seq", "len", "rssi", "snr"}

type Logger struct {
	mu        sync.Mutex
	f         *os.File
	w         *csv.Writer
	total     map[ingest.Kind]int // cumulative since start
	window    map[ingest.Kind]int // since last Summary()
	start     time.Time
	lastSumAt time.Time
}

func NewLogger(path string) (*Logger, error) {
	f, err := os.Create(path)
	if err != nil {
		return nil, err
	}
	w := csv.NewWriter(f)
	if err := w.Write(header); err != nil {
		f.Close()
		return nil, err
	}
	w.Flush()
	now := time.Now()
	return &Logger{
		f: f, w: w,
		total: map[ingest.Kind]int{}, window: map[ingest.Kind]int{},
		start: now, lastSumAt: now,
	}, nil
}

func num(e ingest.Event, k string) string {
	if v, ok := e.Num[k]; ok {
		return strconv.Itoa(v)
	}
	return ""
}

// Log records one event. Unknown/empty events are still counted (they're airtime-free
// console chatter) but only structured ones carry useful columns.
func (l *Logger) Log(e ingest.Event, at time.Time) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.total[e.Kind]++
	l.window[e.Kind]++
	air := "0"
	if e.Kind.IsAirframe() {
		air = "1"
	}
	_ = l.w.Write([]string{
		strconv.FormatInt(at.UnixMilli(), 10),
		e.Kind.String(), air, e.ID, e.Peer,
		num(e, "type"), num(e, "seq"), num(e, "len"), num(e, "rssi"), num(e, "snr"),
	})
	l.w.Flush()
}

// Summary returns a human-readable rate report since the last call: airframes/sec total
// and the per-kind breakdown, plus the airframe share — the first-order chattiness read.
func (l *Logger) Summary() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := time.Now()
	dt := now.Sub(l.lastSumAt).Seconds()
	if dt <= 0 {
		dt = 1
	}
	var airframes, allEvents int
	type kv struct {
		k ingest.Kind
		n int
	}
	var rows []kv
	for k, n := range l.window {
		rows = append(rows, kv{k, n})
		allEvents += n
		if k.IsAirframe() {
			airframes += n
		}
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i].n > rows[j].n })

	s := fmt.Sprintf("[capture] %.0fs window: %d airframes (%.2f/s), %d console events",
		dt, airframes, float64(airframes)/dt, allEvents)
	for _, r := range rows {
		tag := ""
		if r.k.IsAirframe() {
			tag = " *air"
		}
		s += fmt.Sprintf("\n           %-10s %4d (%.2f/s)%s", r.k.String(), r.n, float64(r.n)/dt, tag)
	}
	l.window = map[ingest.Kind]int{}
	l.lastSumAt = now
	return s
}

func (l *Logger) Close() error {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.w.Flush()
	return l.f.Close()
}
