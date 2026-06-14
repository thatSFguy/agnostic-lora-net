package httpd

import (
	"encoding/json"
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
