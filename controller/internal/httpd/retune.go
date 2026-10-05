package httpd

import (
	"errors"
	"fmt"
	"os"
	"regexp"
	"strconv"
	"strings"
	"time"

	"agnostic-lora-net/controller/internal/policy"
	"agnostic-lora-net/controller/internal/sign"
)

// Network-wide retune tracking (docs/remote-config.md §4). The server — not the browser —
// remembers each remote retune it sent and whether the node ACKed it, so the state survives
// page reloads and console-buffer scroll, every step lands in agnctl's log + the decision
// feed, and the gateway retune (the step that strands anyone left behind) is refused until
// every known remote node has ACKed the same PHY.

const cmdRetune = 6 // CTRL_RETUNE

// phyJSON is the PHY a retune carries (TX power excluded — it stays under power/confirm).
type phyJSON struct {
	FreqHz   int `json:"freq_hz"`
	BwHz     int `json:"bw_hz"`
	SF       int `json:"sf"`
	CR       int `json:"cr"`
	Sync     int `json:"sync"`
	Preamble int `json:"preamble"`
}

func (p phyJSON) String() string {
	return fmt.Sprintf("%.3fMHz bw=%gkHz sf=%d cr=4/%d sync=0x%02X pre=%d",
		float64(p.FreqHz)/1e6, float64(p.BwHz)/1000, p.SF, p.CR, p.Sync, p.Preamble)
}

func (p phyJSON) cfg() sign.RetuneCfg {
	return sign.RetuneCfg{FreqHz: uint32(p.FreqHz), BwHz: uint32(p.BwHz), SF: uint8(p.SF),
		CR: uint8(p.CR), Sync: uint8(p.Sync), Preamble: uint16(p.Preamble)}
}

// retuneRec is the latest retune sent to one node. Ack is nil until the node ACKs
// (1 = staged + rebooting onto the new PHY, 0 = rejected as a bad PHY).
type retuneRec struct {
	Ctr    uint32  `json:"ctr"`
	Phy    phyJSON `json:"phy"`
	SentMs int64   `json:"sent_ms"`
	Ack    *int    `json:"ack,omitempty"`
	AckMs  int64   `json:"ack_ms,omitempty"`
	Err    string  `json:"err,omitempty"` // the gateway refused to send it (never went on air)
}

var reRetuneAck = regexp.MustCompile(`^\[ctrl\] ack ([0-9A-Fa-f]{32}) cmd=(\d+) applied=(-?\d+) provisional=\d+ counter=(\d+)`)

// note records a retune step in the dashboard feed and on stderr (agnctl's log).
func (s *Server) note(kind, node, msg string) {
	now := time.Now()
	fmt.Fprintf(os.Stderr, "%s  %s  [%s] %s\n", now.Format("15:04:05"), node, kind, msg)
	s.Sink(policy.Record{TS: now.UnixMilli(), Kind: kind, Node: node, Msg: msg})
}

// retuneSent records a signed retune that was handed to the gateway.
func (s *Server) retuneSent(node string, phy phyJSON, ctr uint32) {
	node = strings.ToUpper(node)
	s.mu.Lock()
	if s.retunes == nil {
		s.retunes = map[string]*retuneRec{}
	}
	s.retunes[node] = &retuneRec{Ctr: ctr, Phy: phy, SentMs: time.Now().UnixMilli()}
	s.mu.Unlock()
	s.note("retune", node, fmt.Sprintf("SENT %s (ctr=%d) — awaiting ACK", phy, ctr))
}

// retuneAck matches a console line against outstanding retunes (called for every line).
func (s *Server) retuneAck(line string) {
	if strings.HasPrefix(line, "usage: ctrlsend") {
		s.retuneRefused(line)
		return
	}
	m := reRetuneAck.FindStringSubmatch(line)
	if m == nil || m[2] != strconv.Itoa(cmdRetune) {
		return
	}
	node := strings.ToUpper(m[1])
	applied, _ := strconv.Atoi(m[3])
	ctr, _ := strconv.ParseUint(m[4], 10, 32)
	s.mu.Lock()
	r := s.retunes[node]
	match := r != nil && r.Ctr == uint32(ctr) && r.Ack == nil
	if match {
		r.Ack, r.AckMs = &applied, time.Now().UnixMilli()
	}
	s.mu.Unlock()
	if !match {
		return
	}
	if applied > 0 {
		s.note("retune", node, fmt.Sprintf("ACK ctr=%d — staged %s, rebooting onto it", ctr, r.Phy))
	} else {
		s.note("retune", node, fmt.Sprintf("REJECTED ctr=%d — node refused %s as a bad PHY", ctr, r.Phy))
	}
}

// retuneRefused marks the newest still-pending retune (sent in the last few seconds) as never
// sent: the gateway's `ctrlsend` rejected the blob (e.g. firmware that predates RETUNE's size).
func (s *Server) retuneRefused(line string) {
	var node string
	var r *retuneRec
	cutoff := time.Now().Add(-10 * time.Second).UnixMilli()
	s.mu.Lock()
	for id, rec := range s.retunes {
		if rec.Ack == nil && rec.Err == "" && rec.SentMs >= cutoff && (r == nil || rec.SentMs > r.SentMs) {
			node, r = id, rec
		}
	}
	if r != nil {
		r.Err = "gateway refused the command (" + line + ") — never sent on air"
	}
	s.mu.Unlock()
	if r != nil {
		s.note("retune", node, fmt.Sprintf("FAILED ctr=%d — %s; gateway firmware too old for RETUNE?", r.Ctr, r.Err))
	}
}

// retuneGateway retunes the tethered gateway over its console. Unless force is set it
// refuses while any known remote node lacks an applied ACK for this exact PHY.
func (s *Server) retuneGateway(phy phyJSON, force bool) (string, error) {
	snap := s.graph.Snapshot()
	var missing []string
	s.mu.Lock()
	for _, n := range snap.Nodes {
		if n.IsGateway {
			continue
		}
		r := s.retunes[strings.ToUpper(n.ID)]
		if r == nil || r.Phy != phy || r.Ack == nil || *r.Ack <= 0 {
			missing = append(missing, n.ID)
		}
	}
	s.mu.Unlock()
	if len(missing) > 0 && !force {
		return "", fmt.Errorf("refused: %d remote node(s) have no ACK for this PHY (%s) — the gateway would lose them", len(missing), strings.Join(missing, ", "))
	}
	gw := snap.Gateway
	if len(missing) > 0 {
		s.note("retune", gw, fmt.Sprintf("GATEWAY FORCED to %s with %d un-ACKed node(s): %s", phy, len(missing), strings.Join(missing, ", ")))
	}
	if s.send == nil {
		return "", errors.New("no serial link to the gateway")
	}
	for _, l := range []string{
		"rf freq " + strconv.Itoa(phy.FreqHz),
		"rf bw " + strconv.FormatFloat(float64(phy.BwHz)/1000, 'f', -1, 64),
		"rf sf " + strconv.Itoa(phy.SF),
		"rf cr " + strconv.Itoa(phy.CR),
		fmt.Sprintf("rf sync 0x%02X", phy.Sync),
		"rf preamble " + strconv.Itoa(phy.Preamble),
		"rf apply", "rf show",
	} {
		if err := s.send(l); err != nil {
			s.note("retune", gw, "GATEWAY retune FAILED at '"+l+"': "+err.Error())
			return "", err
		}
	}
	s.note("retune", gw, "GATEWAY retuned to "+phy.String())
	return "gateway retuned to " + phy.String(), nil
}

func (s *Server) retuneSnapshot() map[string]retuneRec {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make(map[string]retuneRec, len(s.retunes))
	for k, v := range s.retunes {
		out[k] = *v
	}
	return out
}
