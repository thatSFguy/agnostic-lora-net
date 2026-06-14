package ingest

import (
	"regexp"
	"strconv"
	"strings"
)

// idHex matches a node id in either width: 8 hex (v1) or 32 hex (v2, the self-certifying
// 16-byte NodeId). Kept tolerant of both so a mixed-version bench still parses during the
// v2 rollout (`self-certifying-identity-plan.md` §2). The greedy 32 alternative wins when
// present. `§` is the placeholder idre() substitutes.
const idHex = `[0-9A-Fa-f]{32}|[0-9A-Fa-f]{8}`

// idre compiles a pattern with each `§` replaced by a (grouped) id matcher.
func idre(pat string) *regexp.Regexp {
	return regexp.MustCompile(strings.ReplaceAll(pat, "§", `(?:`+idHex+`)`))
}

// Compiled once. Patterns mirror web/map.html (which mirrors the firmware
// Serial.println formats), plus the `trace on` airframe lines ([TX]/[RX]).
var (
	reInfoHeader = idre(`^node (§)\s+neighbors=(\d+) routes=(\d+) blocked=(\d+)`)
	reBlocked    = idre(`^\[blocked\]((?:\s+§)*)\s*$`)
	reFW         = regexp.MustCompile(`^fw (\S+)`)
	reBattMv     = regexp.MustCompile(`^batt\b.*mv=(\d+) pct=(\d+)`)
	reBattRaw    = regexp.MustCompile(`^batt raw=(\d+) UNCALIBRATED`)
	reRF         = regexp.MustCompile(`^\[rf\] freq_hz=(\d+) bw_hz=(\d+) sf=(\d+) cr=(\d+) power_dbm=(-?\d+) sync=0x([0-9A-Fa-f]+) preamble=(\d+)`)
	reBeaconCfg  = regexp.MustCompile(`^net beacon=(\d+)s`)
	reNbr        = idre(`^nbr (§)\s+q_rx=(-?\d+) q_tx=(-?\d+).*?(?:rssi=(-?\d+) snr=(-?\d+))?$`)
	reRoute      = idre(`^route dst=(§) via=(§) cost=(-?\d+) hops=(\d+)`)
	reCtrlAck    = idre(`^\[ctrl\] ack (§) cmd=(\d+) applied=(-?\d+) provisional=(\d)`)
	reNodeBatt   = idre(`^\[batt\] (§) mv=(\d+) pct=(\d+) age=(\d+)s`)
	reStatus     = idre(`^\[status\] (§) fw=(\S+) up=(\d+)min sf=(\d+) pwr=(-?\d+) batt=(?:(\d+)mV/(\d+)%|\?)(?: mob=(\d))?(?: ble=(\d))?`)
	reAnnNbr     = idre(`^\[nbrs\] (§) age=(\d+)s rssi=(-?\d+) snr=(-?\d+)(?: batt=(\d+)%)?`)
	reAnn        = idre(`^\[ann\] (§)(?: pub=([0-9A-Fa-f]{64}))? sig=(ok|bad|none)`)
	reBeaconTX   = idre(`^\[TX\] beacon seq=(\d+) from (§)\s+\+announce (\d+)B`)
	reBeaconRX   = idre(`^\[RX\] beacon\s+src=(§) seq=(\d+) up=(\d+)s`)
	reFrameRX    = idre(`^\[RX\] type=(\d+)\s+src=(§) seq=(\d+) len=(\d+)`)
	reHexID      = regexp.MustCompile(idHex)
)

func atoi(s string) int  { n, _ := strconv.Atoi(s); return n }
func up(s string) string { return strings.ToUpper(s) }

// ParseLine classifies one trimmed console line. ok is false for lines we don't model
// (they're still worth logging raw, but carry no structured meaning). The caller stamps
// At; ParseLine leaves it zero so it's deterministic and unit-testable.
func ParseLine(line string) (Event, bool) {
	t := strings.TrimSpace(line)
	if t == "" {
		return Event{}, false
	}

	if m := reInfoHeader.FindStringSubmatch(t); m != nil {
		e := newEvent(KindInfoHeader, t)
		e.ID = up(m[1])
		e.Num["neighbors"], e.Num["routes"], e.Num["blocked"] = atoi(m[2]), atoi(m[3]), atoi(m[4])
		return e, true
	}
	if m := reBlocked.FindStringSubmatch(t); m != nil {
		e := newEvent(KindBlocked, t)
		for _, id := range reHexID.FindAllString(m[1], -1) {
			e.Blocked = append(e.Blocked, up(id))
		}
		return e, true
	}
	if m := reAnn.FindStringSubmatch(t); m != nil {
		// Gateway-verified identity binding: `[ann] <id> pub=<64hex> sig=ok` (or `… sig=bad`,
		// no pub). The gateway ran ed25519_check + id==hash(pub) on the announce; we trust its
		// verdict (Model A, controller-verify-acl-impl.md §0).
		e := newEvent(KindIdentity, t)
		e.ID = up(m[1])
		if m[2] != "" {
			e.Pub = up(m[2])
		}
		e.SigOK = m[3] == "ok"
		return e, true
	}
	if m := reAnnNbr.FindStringSubmatch(t); m != nil { // before reNbr-style lines; distinct prefix
		e := newEvent(KindAnnNbr, t)
		e.ID = up(m[1])
		e.Num["age"], e.Num["rssi"], e.Num["snr"] = atoi(m[2]), atoi(m[3]), atoi(m[4])
		if m[5] != "" {
			e.Num["pct"] = atoi(m[5])
		}
		return e, true
	}
	if m := reNbr.FindStringSubmatch(t); m != nil {
		e := newEvent(KindNbr, t)
		e.ID = up(m[1])
		e.Num["q_rx"], e.Num["q_tx"] = atoi(m[2]), atoi(m[3])
		if m[4] != "" {
			e.Num["rssi"], e.Num["snr"] = atoi(m[4]), atoi(m[5])
		}
		return e, true
	}
	if m := reRoute.FindStringSubmatch(t); m != nil {
		e := newEvent(KindRoute, t)
		e.Dst, e.Peer = up(m[1]), up(m[2])
		e.Num["cost"], e.Num["hops"] = atoi(m[3]), atoi(m[4])
		return e, true
	}
	if m := reStatus.FindStringSubmatch(t); m != nil {
		e := newEvent(KindStatus, t)
		e.ID = up(m[1])
		e.Str["fw"] = m[2]
		e.Num["up_min"], e.Num["sf"], e.Num["power"] = atoi(m[3]), atoi(m[4]), atoi(m[5])
		if m[6] != "" {
			e.Num["mv"], e.Num["pct"] = atoi(m[6]), atoi(m[7])
		}
		if m[8] != "" {
			e.Num["mob"] = atoi(m[8])
		}
		if m[9] != "" {
			e.Num["ble"] = atoi(m[9])
		}
		return e, true
	}
	if m := reNodeBatt.FindStringSubmatch(t); m != nil {
		e := newEvent(KindNodeBatt, t)
		e.ID = up(m[1])
		e.Num["mv"], e.Num["pct"], e.Num["age"] = atoi(m[2]), atoi(m[3]), atoi(m[4])
		return e, true
	}
	if m := reCtrlAck.FindStringSubmatch(t); m != nil {
		e := newEvent(KindCtrlAck, t)
		e.ID = up(m[1])
		e.Num["cmd"], e.Num["applied"], e.Num["provisional"] = atoi(m[2]), atoi(m[3]), atoi(m[4])
		return e, true
	}
	if m := reBeaconTX.FindStringSubmatch(t); m != nil {
		e := newEvent(KindBeaconTX, t)
		e.Num["seq"] = atoi(m[1])
		e.ID = up(m[2])
		e.Num["ann"] = atoi(m[3])
		return e, true
	}
	if m := reBeaconRX.FindStringSubmatch(t); m != nil {
		e := newEvent(KindBeaconRX, t)
		e.ID = up(m[1])
		e.Num["seq"], e.Num["up_s"] = atoi(m[2]), atoi(m[3])
		return e, true
	}
	if m := reFrameRX.FindStringSubmatch(t); m != nil {
		e := newEvent(KindFrameRX, t)
		e.Num["type"] = atoi(m[1])
		e.ID = up(m[2])
		e.Num["seq"], e.Num["len"] = atoi(m[3]), atoi(m[4])
		return e, true
	}
	if m := reRF.FindStringSubmatch(t); m != nil {
		e := newEvent(KindRF, t)
		e.Num["freq_hz"], e.Num["bw_hz"] = atoi(m[1]), atoi(m[2])
		e.Num["sf"], e.Num["cr"], e.Num["power"] = atoi(m[3]), atoi(m[4]), atoi(m[5])
		sync, _ := strconv.ParseInt(m[6], 16, 32)
		e.Num["sync"], e.Num["preamble"] = int(sync), atoi(m[7])
		return e, true
	}
	if m := reBeaconCfg.FindStringSubmatch(t); m != nil {
		e := newEvent(KindBeaconCfg, t)
		e.Num["beacon_s"] = atoi(m[1])
		return e, true
	}
	if m := reFW.FindStringSubmatch(t); m != nil {
		e := newEvent(KindFW, t)
		e.Str["fw"] = m[1]
		return e, true
	}
	if m := reBattRaw.FindStringSubmatch(t); m != nil {
		e := newEvent(KindBatt, t)
		e.Num["raw"] = atoi(m[1])
		return e, true
	}
	if m := reBattMv.FindStringSubmatch(t); m != nil {
		e := newEvent(KindBatt, t)
		e.Num["mv"], e.Num["pct"] = atoi(m[1]), atoi(m[2])
		return e, true
	}
	return Event{Kind: KindUnknown, Raw: t}, false
}
