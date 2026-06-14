package ingest

import "testing"

func TestParseLine(t *testing.T) {
	cases := []struct {
		line    string
		wantOK  bool
		want    Kind
		id      string
		checkN  map[string]int
		checkS  map[string]string
		blocked []string
	}{
		{line: "node 1A2B3C4D  neighbors=3 routes=5 blocked=1", wantOK: true, want: KindInfoHeader,
			id: "1A2B3C4D", checkN: map[string]int{"neighbors": 3, "routes": 5, "blocked": 1}},
		{line: "[blocked] 8EA09546 1FAE0DBD", wantOK: true, want: KindBlocked,
			blocked: []string{"8EA09546", "1FAE0DBD"}},
		{line: "[blocked]", wantOK: true, want: KindBlocked, blocked: nil},
		{line: "  nbr 8EA09546  q_rx=97 q_tx=99 (x100)  myAlias=12 theirAlias=7 rssi=-42 snr=9",
			wantOK: true, want: KindNbr, id: "8EA09546",
			checkN: map[string]int{"q_rx": 97, "q_tx": 99, "rssi": -42, "snr": 9}},
		{line: "  nbr 8EA09546  q_rx=97 q_tx=99 (x100)  myAlias=12 theirAlias=7",
			wantOK: true, want: KindNbr, id: "8EA09546", checkN: map[string]int{"q_rx": 97, "q_tx": 99}},
		{line: "[nbrs] 1FAE0DBD age=12s rssi=-80 snr=-5 batt=88%", wantOK: true, want: KindAnnNbr,
			id: "1FAE0DBD", checkN: map[string]int{"age": 12, "rssi": -80, "snr": -5, "pct": 88}},
		{line: "[rf] freq_hz=904375000 bw_hz=250000 sf=9 cr=5 power_dbm=22 sync=0x4D preamble=16 (active)",
			wantOK: true, want: KindRF, checkN: map[string]int{"sf": 9, "power": 22, "sync": 0x4D, "preamble": 16}},
		{line: "net beacon=10s", wantOK: true, want: KindBeaconCfg, checkN: map[string]int{"beacon_s": 10}},
		{line: "route dst=1FAE0DBD via=8EA09546 cost=120 hops=2", wantOK: true, want: KindRoute,
			checkN: map[string]int{"cost": 120, "hops": 2}},
		{line: "[ctrl] ack 1FAE0DBD cmd=1 applied=14 provisional=1", wantOK: true, want: KindCtrlAck,
			id: "1FAE0DBD", checkN: map[string]int{"cmd": 1, "applied": 14, "provisional": 1}},
		{line: "[batt] 1FAE0DBD mv=3850 pct=60 age=30s", wantOK: true, want: KindNodeBatt,
			id: "1FAE0DBD", checkN: map[string]int{"mv": 3850, "pct": 60, "age": 30}},
		{line: "[status] 1FAE0DBD fw=0.8.2 up=42min sf=9 pwr=14 batt=3850mV/60%", wantOK: true, want: KindStatus,
			id: "1FAE0DBD", checkN: map[string]int{"sf": 9, "power": 14, "mv": 3850, "pct": 60},
			checkS: map[string]string{"fw": "0.8.2"}},
		{line: "[status] 1FAE0DBD fw=0.8.2 up=42min sf=9 pwr=14 batt=?", wantOK: true, want: KindStatus,
			id: "1FAE0DBD", checkN: map[string]int{"sf": 9, "power": 14}},
		{line: "[status] 1FAE0DBD fw=0.9.0 up=42min sf=9 pwr=14 batt=3850mV/60% mob=0 ble=1", wantOK: true, want: KindStatus,
			id: "1FAE0DBD", checkN: map[string]int{"sf": 9, "power": 14, "mv": 3850, "pct": 60, "mob": 0, "ble": 1}},
		{line: "[TX] beacon seq=5 from 1A2B3C4D  +announce 24B", wantOK: true, want: KindBeaconTX,
			id: "1A2B3C4D", checkN: map[string]int{"seq": 5, "ann": 24}},
		{line: "[RX] beacon  src=8EA09546 seq=3 up=120s  rssi=-42.0 snr=9.0", wantOK: true, want: KindBeaconRX,
			id: "8EA09546", checkN: map[string]int{"seq": 3, "up_s": 120}},
		{line: "[RX] type=4  src=8EA09546 seq=2 len=37  rssi=-50.0 snr=8.0", wantOK: true, want: KindFrameRX,
			id: "8EA09546", checkN: map[string]int{"type": 4, "seq": 2, "len": 37}},
		{line: "fw 0.8.2  built Jun 12 2026", wantOK: true, want: KindFW, checkS: map[string]string{"fw": "0.8.2"}},
		{line: "batt mv=3850 pct=60", wantOK: true, want: KindBatt, checkN: map[string]int{"mv": 3850, "pct": 60}},
		{line: "some unmodelled debug line", wantOK: false, want: KindUnknown},
		{line: "   ", wantOK: false, want: KindUnknown},
	}

	for _, c := range cases {
		e, ok := ParseLine(c.line)
		if ok != c.wantOK {
			t.Errorf("ParseLine(%q) ok=%v want %v", c.line, ok, c.wantOK)
			continue
		}
		if e.Kind != c.want {
			t.Errorf("ParseLine(%q) kind=%v want %v", c.line, e.Kind, c.want)
		}
		if c.id != "" && e.ID != c.id {
			t.Errorf("ParseLine(%q) id=%q want %q", c.line, e.ID, c.id)
		}
		for k, v := range c.checkN {
			if got, present := e.Num[k]; !present || got != v {
				t.Errorf("ParseLine(%q) Num[%q]=%d present=%v want %d", c.line, k, got, present, v)
			}
		}
		for k, v := range c.checkS {
			if e.Str[k] != v {
				t.Errorf("ParseLine(%q) Str[%q]=%q want %q", c.line, k, e.Str[k], v)
			}
		}
		if len(c.blocked) != len(e.Blocked) {
			t.Errorf("ParseLine(%q) blocked=%v want %v", c.line, e.Blocked, c.blocked)
		}
	}
}

// v2 identity: the `[ann]` contract line carries a gateway-verified id↔pubkey binding, with
// 32-hex (16-byte) ids. sig=ok yields a pub + SigOK; sig=bad yields neither.
func TestParseIdentity(t *testing.T) {
	const id = "9828f51b1122334455667788990011aa" // 32 hex
	const pub = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
	e, ok := ParseLine("[ann] " + id + " pub=" + pub + " sig=ok")
	if !ok || e.Kind != KindIdentity {
		t.Fatalf("ann sig=ok: ok=%v kind=%v", ok, e.Kind)
	}
	if e.ID != up(id) || e.Pub != pub || !e.SigOK {
		t.Fatalf("ann sig=ok decoded: id=%q pub=%q sigok=%v", e.ID, e.Pub, e.SigOK)
	}
	eb, ok := ParseLine("[ann] " + id + " sig=bad")
	if !ok || eb.Kind != KindIdentity || eb.SigOK || eb.Pub != "" {
		t.Fatalf("ann sig=bad: ok=%v kind=%v sigok=%v pub=%q", ok, eb.Kind, eb.SigOK, eb.Pub)
	}
}

// The id regexes accept both v1 (8-hex) and v2 (32-hex) widths during rollout. A 32-hex
// status line must parse with the full id and all its fields (incl. ble=).
func TestParseWideStatus(t *testing.T) {
	const id = "1fae0dbdfeedfacecafebabe00ff0102"
	e, ok := ParseLine("[status] " + id + " fw=0.12.0 up=42min sf=9 pwr=14 batt=3850mV/60% mob=0 ble=1")
	if !ok || e.Kind != KindStatus || e.ID != up(id) {
		t.Fatalf("wide status: ok=%v kind=%v id=%q", ok, e.Kind, e.ID)
	}
	for k, want := range map[string]int{"sf": 9, "power": 14, "mv": 3850, "pct": 60, "mob": 0, "ble": 1} {
		if got, present := e.Num[k]; !present || got != want {
			t.Fatalf("wide status Num[%q]=%d present=%v want %d", k, got, present, want)
		}
	}
}

// A blocked neighbour drops out of the `nbr` lines, so the topology can't infer it — the
// authoritative source is the [blocked] line. Guard that it round-trips both ids.
func TestBlockedList(t *testing.T) {
	e, ok := ParseLine("[blocked] 8EA09546 1FAE0DBD 0BADC0DE")
	if !ok || e.Kind != KindBlocked || len(e.Blocked) != 3 {
		t.Fatalf("blocked parse: ok=%v kind=%v ids=%v", ok, e.Kind, e.Blocked)
	}
	if e.Blocked[0] != "8EA09546" || e.Blocked[2] != "0BADC0DE" {
		t.Fatalf("blocked ids wrong: %v", e.Blocked)
	}
}
