// Package ingest turns a node's console/tunnel text stream into structured Events.
//
// The line formats are the same ones web/map.html already parses (they come straight
// from the firmware's Serial.println()s). This package is the Go port of that parser,
// shared by the topology model (topo) and the passive capture logger (capture).
package ingest

import "time"

// Kind classifies a parsed console line. The capture logger buckets airtime/chattiness
// by Kind; the topology model reacts to the structural ones.
type Kind int

const (
	KindUnknown    Kind = iota
	KindInfoHeader      // "node X  neighbors=N routes=N blocked=N"
	KindBlocked         // "[blocked] X X ..."
	KindFW              // "fw <ver>"
	KindBatt            // gateway battery: "batt mv=N pct=N" / "batt raw=N UNCALIBRATED"
	KindRF              // "[rf] freq_hz=.. sf=.. power_dbm=.."
	KindBeaconCfg       // "net beacon=Ns"
	KindNbr             // gateway's own neighbour (from info): "nbr X q_rx=.. q_tx=.."
	KindRoute           // "route dst=X via=X cost=N hops=N"
	KindCtrlAck         // "[ctrl] ack X cmd=N applied=N provisional=N"
	KindNodeBatt        // "[batt] X mv=N pct=N age=Ns"
	KindStatus          // "[status] X fw=V up=Nmin sf=N pwr=N batt=..."
	KindAnnNbr          // "[nbrs] X age=Ns rssi=R snr=S" (announce-derived topology)
	KindBeaconTX        // "[TX] beacon seq=N from X +announce NB"   (trace on)
	KindBeaconRX        // "[RX] beacon src=X seq=N up=Ns ..."        (trace on)
	KindFrameRX         // "[RX] type=N src=X seq=N len=N ..."        (trace on)
)

var kindNames = map[Kind]string{
	KindUnknown: "unknown", KindInfoHeader: "info", KindBlocked: "blocked",
	KindFW: "fw", KindBatt: "batt", KindRF: "rf", KindBeaconCfg: "beaconcfg",
	KindNbr: "nbr", KindRoute: "route", KindCtrlAck: "ctrlack", KindNodeBatt: "nodebatt",
	KindStatus: "status", KindAnnNbr: "annnbr", KindBeaconTX: "beacon_tx",
	KindBeaconRX: "beacon_rx", KindFrameRX: "frame_rx",
}

func (k Kind) String() string {
	if s, ok := kindNames[k]; ok {
		return s
	}
	return "unknown"
}

// IsAirframe reports whether this Kind represents an actual frame on the air — the set
// the chattiness/airtime capture counts. (Requires `trace on` for the beacon/frame
// kinds; the structural kinds above are console replies, not airframes.)
func (k Kind) IsAirframe() bool {
	switch k {
	case KindBeaconTX, KindBeaconRX, KindFrameRX:
		return true
	}
	return false
}

// Event is one parsed console line. Numeric fields land in Num (only when present, so
// callers can distinguish "0" from "absent"); free-form strings land in Str. Hex node
// ids are normalised to upper-case in ID/Peer/Dst/Blocked.
type Event struct {
	Kind    Kind
	At      time.Time
	Raw     string
	ID      string            // primary node id the line is about ("" if none)
	Peer    string            // secondary id (nbr peer / route via)
	Dst     string            // route destination
	Blocked []string          // ids, for KindBlocked
	Num     map[string]int    // q_rx q_tx rssi snr sf power seq len cost hops mv pct age ann freq_hz bw_hz cr sync preamble applied provisional
	Str     map[string]string // fw, …
}

func newEvent(k Kind, raw string) Event {
	return Event{Kind: k, Raw: raw, Num: map[string]int{}, Str: map[string]string{}}
}
