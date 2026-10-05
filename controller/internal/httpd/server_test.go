package httpd

import (
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"agnostic-lora-net/controller/internal/ingest"
	"agnostic-lora-net/controller/internal/keystore"
	"agnostic-lora-net/controller/internal/policy"
	"agnostic-lora-net/controller/internal/topo"
)

func TestServe(t *testing.T) {
	g := topo.New()
	g.Apply(ingest.Event{Kind: ingest.KindInfoHeader, ID: "GW000001"}, time.Now())

	var sent []string
	s := New(g, nil, func(line string) error { sent = append(sent, line); return nil }, "", "")
	s.Sink(policy.Record{Kind: "decision", Node: "AAAA0001"})

	h := s.Handler()

	// "/" serves the dashboard HTML.
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, httptest.NewRequest("GET", "/", nil))
	if rr.Code != 200 || !strings.Contains(rr.Body.String(), "AGN Controller") {
		t.Fatalf("/ -> %d, body has dashboard: %v", rr.Code, strings.Contains(rr.Body.String(), "AGN Controller"))
	}

	// "/api/state" serves snapshot + events.
	rr2 := httptest.NewRecorder()
	h.ServeHTTP(rr2, httptest.NewRequest("GET", "/api/state", nil))
	if rr2.Code != 200 {
		t.Fatalf("/api/state -> %d", rr2.Code)
	}
	var st stateJSON
	if err := json.Unmarshal(rr2.Body.Bytes(), &st); err != nil {
		t.Fatal(err)
	}
	if st.Snapshot.Gateway != "GW000001" {
		t.Fatalf("gateway=%q", st.Snapshot.Gateway)
	}
	if len(st.Events) != 1 || st.Events[0].Node != "AAAA0001" {
		t.Fatalf("events=%v", st.Events)
	}
}

func TestCmdAPI(t *testing.T) {
	var sent []string
	s := New(topo.New(), nil, func(l string) error { sent = append(sent, l); return nil }, "", "")
	h := s.Handler()

	// raw console line needs no key -> sent through.
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, httptest.NewRequest("POST", "/api/cmd", strings.NewReader(`{"action":"raw","line":"info"}`)))
	var resp map[string]any
	_ = json.Unmarshal(rr.Body.Bytes(), &resp)
	if resp["ok"] != true || len(sent) != 1 || sent[0] != "info" {
		t.Fatalf("raw cmd: ok=%v sent=%v", resp["ok"], sent)
	}

	// block needs a key -> rejected cleanly when there's none.
	rr2 := httptest.NewRecorder()
	h.ServeHTTP(rr2, httptest.NewRequest("POST", "/api/cmd", strings.NewReader(`{"action":"block","node":"AABBCCDD","victim":"11223344"}`)))
	var resp2 map[string]any
	_ = json.Unmarshal(rr2.Body.Bytes(), &resp2)
	if resp2["ok"] != false {
		t.Fatalf("block without key should fail, got %v", resp2)
	}
}

// The membership gate must reject a command to an unapproved node BEFORE advancing the
// replay counter (no counter burn on reject), and accept once the pubkey is approved.
func TestCmdACLGate(t *testing.T) {
	ks, err := keystore.Mint(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	g := topo.New()
	g.SetAllowFunc(ks.IsAllowed) // ACL now configured -> gate is live
	const id = "9828F51B1122334455667788990011AA"
	pub := strings.Repeat("AB", 32) // 64-hex pubkey
	g.Apply(ingest.Event{Kind: ingest.KindIdentity, ID: id, Pub: pub, SigOK: true}, time.Now())

	var sent []string
	s := New(g, ks, func(l string) error { sent = append(sent, l); return nil }, "", "")
	h := s.Handler()
	post := func(body string) map[string]any {
		rr := httptest.NewRecorder()
		h.ServeHTTP(rr, httptest.NewRequest("POST", "/api/cmd", strings.NewReader(body)))
		var resp map[string]any
		_ = json.Unmarshal(rr.Body.Bytes(), &resp)
		return resp
	}
	counter := func() uint32 { _, _, c, _ := ks.Export(); return c }

	// Verified but NOT approved -> rejected, counter unchanged, nothing sent.
	c0 := counter()
	if r := post(`{"action":"power","node":"` + id + `","dbm":14}`); r["ok"] != false {
		t.Fatalf("unapproved node: expected reject, got %v", r)
	}
	if counter() != c0 {
		t.Fatalf("counter advanced on a rejected command: %d -> %d", c0, counter())
	}
	if len(sent) != 0 {
		t.Fatalf("rejected command should send nothing, sent %v", sent)
	}

	// Approve the pubkey via /api/acl, then the same command must go through.
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, httptest.NewRequest("POST", "/api/acl", strings.NewReader(`{"action":"approve","pub":"`+pub+`"}`)))
	var aclResp map[string]any
	_ = json.Unmarshal(rr.Body.Bytes(), &aclResp)
	if aclResp["ok"] != true {
		t.Fatalf("approve failed: %v", aclResp)
	}
	if r := post(`{"action":"power","node":"` + id + `","dbm":14}`); r["ok"] != true {
		t.Fatalf("approved node: expected accept, got %v", r)
	}
	if counter() == c0 {
		t.Fatal("counter should advance after an accepted command")
	}
	if len(sent) != 1 || !strings.HasPrefix(sent[0], "ctrlsend ") {
		t.Fatalf("approved command should send one ctrlsend, sent %v", sent)
	}
}

// A network retune is tracked server-side: the send is recorded, the node's ACK (matched on
// cmd=6 + counter) is latched, and the gateway step is refused until every remote node has
// ACKed the same PHY — unless explicitly forced.
func TestRetuneTrackingAndGatewayGate(t *testing.T) {
	ks, err := keystore.Mint(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	g := topo.New()
	g.Apply(ingest.Event{Kind: ingest.KindInfoHeader, ID: "36C67CE5F7240229FD414B8115A0247E"}, time.Now())
	const id = "FE94E184243CE79DC2C2C622EECFBECB"
	g.Apply(ingest.Event{Kind: ingest.KindIdentity, ID: id, Pub: strings.Repeat("AB", 32), SigOK: true}, time.Now())

	var sent []string
	s := New(g, ks, func(l string) error { sent = append(sent, l); return nil }, "", "")
	h := s.Handler()
	post := func(body string) map[string]any {
		rr := httptest.NewRecorder()
		h.ServeHTTP(rr, httptest.NewRequest("POST", "/api/cmd", strings.NewReader(body)))
		var resp map[string]any
		_ = json.Unmarshal(rr.Body.Bytes(), &resp)
		return resp
	}
	const phy = `"freq_hz":906625000,"bw_hz":62500,"sf":7,"cr":5,"sync":77,"preamble":16`

	// Gateway first, nothing ACKed -> refused, nothing sent.
	if r := post(`{"action":"gwretune",` + phy + `}`); r["ok"] != false {
		t.Fatalf("gateway retune before any ACK: expected refusal, got %v", r)
	}
	if len(sent) != 0 {
		t.Fatalf("refused gateway retune sent %v", sent)
	}

	// Remote retune -> recorded with its counter, no ACK yet.
	if r := post(`{"action":"retune","node":"` + id + `",` + phy + `}`); r["ok"] != true {
		t.Fatalf("remote retune: %v", r)
	}
	rec, ok := s.retuneSnapshot()[id]
	if !ok || rec.Ack != nil || rec.Phy.SF != 7 || rec.Phy.BwHz != 62500 {
		t.Fatalf("retune not recorded as pending: %+v ok=%v", rec, ok)
	}

	// An ACK for another command or a stale counter must not count.
	s.Console(fmt.Sprintf("[ctrl] ack %s cmd=1 applied=11 provisional=0 counter=%d", id, rec.Ctr))
	s.Console(fmt.Sprintf("[ctrl] ack %s cmd=6 applied=1 provisional=0 counter=%d", id, rec.Ctr-1))
	if s.retuneSnapshot()[id].Ack != nil {
		t.Fatal("unrelated ACK was latched as the retune ACK")
	}
	if r := post(`{"action":"gwretune",` + phy + `}`); r["ok"] != false {
		t.Fatalf("gateway retune with node un-ACKed: expected refusal, got %v", r)
	}

	// The real ACK is latched (and survives the console ring scrolling past it).
	s.Console(fmt.Sprintf("[ctrl] ack %s cmd=6 applied=1 provisional=0 counter=%d", id, rec.Ctr))
	for i := 0; i < maxConsole+5; i++ {
		s.Console("[hb] filler")
	}
	if a := s.retuneSnapshot()[id].Ack; a == nil || *a != 1 {
		t.Fatalf("retune ACK not latched: %v", a)
	}

	// A different PHY than the one ACKed is still refused...
	if r := post(`{"action":"gwretune","freq_hz":906625000,"bw_hz":125000,"sf":7,"cr":5,"sync":77,"preamble":16}`); r["ok"] != false {
		t.Fatalf("gateway retune to an un-ACKed PHY: expected refusal, got %v", r)
	}
	// ...the ACKed one goes through, as console rf commands ending in apply.
	sent = nil
	if r := post(`{"action":"gwretune",` + phy + `}`); r["ok"] != true {
		t.Fatalf("gateway retune after ACK: %v", r)
	}
	want := []string{"rf freq 906625000", "rf bw 62.5", "rf sf 7", "rf cr 5", "rf sync 0x4D", "rf preamble 16", "rf apply", "rf show"}
	if strings.Join(sent, "|") != strings.Join(want, "|") {
		t.Fatalf("gateway retune lines:\n got %v\nwant %v", sent, want)
	}

	// Steps are in the feed.
	var n int
	for _, e := range s.events {
		if e.Kind == "retune" {
			n++
		}
	}
	if n != 3 { // SENT, ACK, GATEWAY retuned
		t.Fatalf("expected 3 retune feed events, got %d: %+v", n, s.events)
	}
}

// A gateway that refuses the ctrlsend (old firmware) marks the retune failed, not "awaiting ACK".
func TestRetuneRefusedByGateway(t *testing.T) {
	g := topo.New()
	g.Apply(ingest.Event{Kind: ingest.KindInfoHeader, ID: "36C67CE5F7240229FD414B8115A0247E"}, time.Now())
	s := New(g, nil, func(string) error { return nil }, "", "")
	const id = "A4473FC3984914F27C0E10D43CD7A6A1"
	s.retuneSent(id, phyJSON{FreqHz: 906625000, BwHz: 62500, SF: 7, CR: 5, Sync: 0x4D, Preamble: 16}, 42)
	s.Console("usage: ctrlsend <150 or 158 hex chars>")
	r := s.retuneSnapshot()[id]
	if r.Err == "" || r.Ack != nil {
		t.Fatalf("refused retune not marked failed: %+v", r)
	}
}

func TestGatewayRetuneForce(t *testing.T) {
	g := topo.New()
	g.Apply(ingest.Event{Kind: ingest.KindInfoHeader, ID: "36C67CE5F7240229FD414B8115A0247E"}, time.Now())
	g.Apply(ingest.Event{Kind: ingest.KindIdentity, ID: "A4473FC3984914F27C0E10D43CD7A6A1", Pub: strings.Repeat("CD", 32), SigOK: true}, time.Now())
	var sent []string
	s := New(g, nil, func(l string) error { sent = append(sent, l); return nil }, "", "")
	if _, err := s.issue(cmdReq{Action: "gwretune", FreqHz: 906625000, BwHz: 250000, SF: 9, CR: 5, Sync: 0x4D, Preamble: 16, Force: true}); err != nil {
		t.Fatalf("forced gateway retune: %v", err)
	}
	if len(sent) != 8 || sent[1] != "rf bw 250" {
		t.Fatalf("forced gateway retune sent %v", sent)
	}
}
