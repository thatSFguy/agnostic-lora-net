// Package httpd serves the controller's live dashboard: the current topology plus the
// streaming policy decision/event feed. It's a read-only window the operator opens in a
// browser to watch optimisation happen — no controls (commands stay on the trusted CLI).
package httpd

import (
	_ "embed"
	"encoding/json"
	"net/http"
	"sync"

	"agnostic-lora-net/controller/internal/policy"
	"agnostic-lora-net/controller/internal/topo"
)

//go:embed index.html
var indexHTML []byte

const maxEvents = 300

// Server holds the live graph and a ring buffer of recent policy records.
type Server struct {
	graph  *topo.Graph
	mu     sync.Mutex
	events []policy.Record
}

func New(graph *topo.Graph) *Server { return &Server{graph: graph} }

// Sink is the policy.Logger subscriber: it appends each record to the ring buffer.
func (s *Server) Sink(r policy.Record) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.events = append(s.events, r)
	if len(s.events) > maxEvents {
		s.events = s.events[len(s.events)-maxEvents:]
	}
}

type stateJSON struct {
	Snapshot topo.Snapshot   `json:"snapshot"`
	Events   []policy.Record `json:"events"`
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		_, _ = w.Write(indexHTML)
	})
	mux.HandleFunc("/api/state", func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		ev := make([]policy.Record, len(s.events))
		copy(ev, s.events)
		s.mu.Unlock()
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(stateJSON{Snapshot: s.graph.Snapshot(), Events: ev})
	})
	return mux
}
