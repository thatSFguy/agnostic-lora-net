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

	s := New(g)
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
