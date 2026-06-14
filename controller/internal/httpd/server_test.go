package httpd

import (
	"encoding/json"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"agnostic-lora-net/controller/internal/ingest"
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
