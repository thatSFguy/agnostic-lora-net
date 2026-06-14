package ingest

import (
	"regexp"
	"strconv"
	"strings"
)

// Compiled once. Patterns mirror web/map.html (which mirrors the firmware
// Serial.println formats), plus the `trace on` airframe lines ([TX]/[RX]).
var (
	reInfoHeader = regexp.MustCompile(`^node ([0-9A-Fa-f]{8})\s+neighbors=(\d+) routes=(\d+) blocked=(\d+)`)
	reBlocked    = regexp.MustCompile(`^\[blocked\]((?:\s+[0-9A-Fa-f]{8})*)\s*$`)
	reFW         = regexp.MustCompile(`^fw (\S+)`)
	reBattMv     = regexp.MustCompile(`^batt\b.*mv=(\d+) pct=(\d+)`)
	reBattRaw    = regexp.MustCompile(`^batt raw=(\d+) UNCALIBRATED`)
	reRF         = regexp.MustCompile(`^\[rf\] freq_hz=(\d+) bw_hz=(\d+) sf=(\d+) cr=(\d+) power_dbm=(-?\d+) sync=0x([0-9A-Fa-f]+) preamble=(\d+)`)
	reBeaconCfg  = regexp.MustCompile(`^net beacon=(\d+)s`)
	reNbr        = regexp.MustCompile(`^nbr ([0-9A-Fa-f]{8})\s+q_rx=(-?\d+) q_tx=(-?\d+).*?(?:rssi=(-?\d+) snr=(-?\d+))?$`)
	reRoute      = regexp.MustCompile(`^route dst=([0-9A-Fa-f]{8}) via=([0-9A-Fa-f]{8}) cost=(-?\d+) hops=(\d+)`)
	reCtrlAck    = regexp.MustCompile(`^\[ctrl\] ack ([0-9A-Fa-f]{8}) cmd=(\d+) applied=(-?\d+) provisional=(\d)`)
	reNodeBatt   = regexp.MustCompile(`^\[batt\] ([0-9A-Fa-f]{8}) mv=(\d+) pct=(\d+) age=(\d+)s`)
	reStatus     = regexp.MustCompile(`^\[status\] ([0-9A-Fa-f]{8}) fw=(\S+) up=(\d+)min sf=(\d+) pwr=(-?\d+) batt=(?:(\d+)mV/(\d+)%|\?)(?: mob=(\d))?(?: ble=(\d))?`)
	reAnnNbr     = regexp.MustCompile(`^\[nbrs\] ([0-9A-Fa-f]{8}) age=(\d+)s rssi=(-?\d+) snr=(-?\d+)(?: batt=(\d+)%)?`)
	reBeaconTX   = regexp.MustCompile(`^\[TX\] beacon seq=(\d+) from ([0-9A-Fa-f]{8})\s+\+announce (\d+)B`)
	reBeaconRX   = regexp.MustCompile(`^\[RX\] beacon\s+src=([0-9A-Fa-f]{8}) seq=(\d+) up=(\d+)s`)
	reFrameRX    = regexp.MustCompile(`^\[RX\] type=(\d+)\s+src=([0-9A-Fa-f]{8}) seq=(\d+) len=(\d+)`)
	reHexID      = regexp.MustCompile(`[0-9A-Fa-f]{8}`)
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
