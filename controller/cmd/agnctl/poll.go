package main

import (
	"context"
	"fmt"
	"sort"
	"sync"
	"time"

	"agnostic-lora-net/controller/internal/ingest"
	"agnostic-lora-net/controller/internal/topo"
)

// Adaptive telemetry poller. The old loop fired a flooded `status` query every 3 s round-robin —
// at SF9 that telemetry was the network's dominant airtime consumer, oversampling the 15 s
// optimiser 5× and stepping on user traffic. This poller instead:
//
//   - backs each node off while it's STABLE (fixed nodes barely move): the interval grows base →
//     ×factor → max, so a settled node is polled every few minutes, not every few seconds;
//   - snaps a node back to `base` the moment a CHANGE is observed (TX power, neighbour set, or a
//     >1-bucket SNR shift) — detected continuously from the graph, so beacon-visible changes break
//     the backoff without waiting for the node's slow poll to come due;
//   - YIELDS the channel entirely while the mesh is busy carrying messaging (PKT_DATA/ACK),
//     resuming only once data traffic has been quiet for `busyQuiet`.
//
// Local gateway queries (info/nbrdump) cost no airtime, so they keep running on a modest cadence.

// activity tracks the last data-plane (messaging) frame so the poller can yield to user/RNS
// traffic. Fed from the main ingest loop. Safe for concurrent use.
type activity struct {
	mu       sync.Mutex
	lastData time.Time
}

// record notes a frame. Only DATA/ACK count as "messaging" — beacons, telemetry, locator floods,
// and announces are mesh housekeeping, not the traffic we want to yield to.
func (a *activity) record(e ingest.Event, now time.Time) {
	if e.Kind != ingest.KindFrameRX {
		return
	}
	switch e.Num["type"] {
	case 0, 2: // PKT_DATA, PKT_ACK (include/packet.h)
		a.mu.Lock()
		a.lastData = now
		a.mu.Unlock()
	}
}

// busy reports whether messaging has been seen within `window` (so telemetry should hold off).
func (a *activity) busy(now time.Time, window time.Duration) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return !a.lastData.IsZero() && now.Sub(a.lastData) < window
}

// pollCfg tunes the adaptive poller.
type pollCfg struct {
	base      time.Duration // interval right after a change (and the optimiser's natural cadence)
	max       time.Duration // ceiling a stable fixed node backs off to
	factor    float64       // interval multiplier per unchanged poll
	mobileMax time.Duration // a mobile node never backs off past this (it moves)
	busyQuiet time.Duration // suppress telemetry until messaging is this quiet
}

func defaultPollCfg() pollCfg {
	return pollCfg{
		base: 15 * time.Second, max: 5 * time.Minute, factor: 1.6,
		mobileMax: 30 * time.Second, busyQuiet: 8 * time.Second,
	}
}

type nodePoll struct {
	interval time.Duration
	next     time.Time
	sig      string // change-signature captured at the last poll
	started  bool
}

func minDur(a, b time.Duration) time.Duration {
	if a < b {
		return a
	}
	return b
}

func sh8(s string) string {
	if len(s) > 8 {
		return s[:8]
	}
	return s
}

// snrBucket quantises SNR into 3 dB bins so RF jitter doesn't read as a change.
func snrBucket(snr int) int { return snr / 3 }

// nodeSig is the change-signature for a node: TX power, mobility, and the bucketed SNR to each
// neighbour it's heard by. Two snapshots with the same signature mean "nothing the optimiser
// cares about moved" -> safe to keep backing off.
func nodeSig(s topo.Snapshot, id string) string {
	var n *topo.Node
	for i := range s.Nodes {
		if s.Nodes[i].ID == id {
			n = &s.Nodes[i]
			break
		}
	}
	if n == nil {
		return ""
	}
	var links []string
	for _, l := range s.Links {
		if l.From != id {
			continue
		}
		b := 0
		if l.RSSI != 0 { // measured SNR only; quality-only links just count as "present"
			b = snrBucket(l.SNR)
		}
		links = append(links, fmt.Sprintf("%s:%d", sh8(l.To), b))
	}
	sort.Strings(links)
	return fmt.Sprintf("p%d m%t|%v", n.Power, n.Mobile, links)
}

func nodeMobile(s topo.Snapshot, id string) bool {
	for i := range s.Nodes {
		if s.Nodes[i].ID == id {
			return s.Nodes[i].Mobile
		}
	}
	return false
}

// nextInterval is the pure backoff rule: base on the first poll or any change, else grow by
// factor up to max; a mobile node is always capped at mobileMax.
func nextInterval(cur time.Duration, firstPoll, changed, mobile bool, cfg pollCfg) time.Duration {
	iv := cfg.base
	if !firstPoll && !changed {
		iv = minDur(time.Duration(float64(cur)*cfg.factor), cfg.max)
	}
	if mobile {
		iv = minDur(iv, cfg.mobileMax)
	}
	return iv
}

func pollLoop(ctx context.Context, src ingest.Source, graph *topo.Graph, act *activity, cfg pollCfg) {
	time.Sleep(400 * time.Millisecond)
	// reinit re-arms per-connection state on (re)connect: airframe trace + a re-emit of every
	// verified [ann] binding, so a controller that connects after a node's one-shot proof still
	// learns the identity. Zero airtime (local console commands).
	reinit := func() { _ = src.Send("trace on"); _ = src.Send("anndump") }
	reinit()

	tick := time.NewTicker(3 * time.Second)
	defer tick.Stop()
	state := map[string]*nodePoll{}
	var infoNext, nbrNext, reinitNext time.Time
	rr := 0

	for {
		select {
		case <-ctx.Done():
			return
		case now := <-tick.C:
			// Local gateway polls (USB, no airtime) keep its own tables + RF fresh.
			if now.After(infoNext) {
				_ = src.Send("info")
				infoNext = now.Add(10 * time.Second)
			}
			if now.After(nbrNext) {
				_ = src.Send("nbrdump")
				nbrNext = now.Add(30 * time.Second)
			}
			if now.After(reinitNext) {
				reinit()
				reinitNext = now.Add(60 * time.Second)
			}

			// Yield the channel entirely while the mesh is carrying messaging.
			if act.busy(now, cfg.busyQuiet) {
				continue
			}

			ids := graph.ManagedIDs()
			if len(ids) == 0 {
				continue
			}
			snap := graph.Snapshot()

			// Continuous change detection: any node whose signature moved since its last poll is
			// pulled forward to be polled now, breaking its backoff even if it was deep in it.
			for _, id := range ids {
				st := state[id]
				if st == nil {
					st = &nodePoll{interval: cfg.base}
					state[id] = st
				}
				if st.started && nodeSig(snap, id) != st.sig && st.next.After(now) {
					st.next = now
				}
			}

			// Poll at most ONE due node per tick (round-robin), keeping airtime low.
			n := len(ids)
			pick := ""
			for i := 0; i < n; i++ {
				id := ids[(rr+i)%n]
				st := state[id]
				if !st.started || !now.Before(st.next) {
					pick = id
					rr = (rr + i + 1) % n
					break
				}
			}
			if pick == "" {
				continue // nothing due — everyone's still backed off
			}
			st := state[pick]
			cur := nodeSig(snap, pick)
			st.interval = nextInterval(st.interval, !st.started, st.started && cur != st.sig, nodeMobile(snap, pick), cfg)
			st.sig = cur
			st.started = true
			st.next = now.Add(st.interval)
			_ = src.Send("status " + pick)
		}
	}
}
