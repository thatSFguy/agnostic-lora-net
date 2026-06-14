// Package httpd serves the controller's dashboard: live topology, the streaming policy
// decision feed, and an interactive control surface (block/power/console) that issues
// signed commands through the controller's own key + serial link. The browser is a thin
// client — no Web Serial, no key in the browser, no fight over the USB port.
//
// SECURITY: the write API (/api/cmd) wields the controller key. Serve it only on a trusted
// address (localhost / the bench LAN), never the open internet.
package httpd

import (
	_ "embed"
	"encoding/json"
	"errors"
	"net/http"
	"strconv"
	"sync"

	"agnostic-lora-net/controller/internal/commander"
	"agnostic-lora-net/controller/internal/keystore"
	"agnostic-lora-net/controller/internal/policy"
	"agnostic-lora-net/controller/internal/topo"
)

//go:embed index.html
var indexHTML []byte

// nrf-dfu.js is vendored from web/nrf-dfu.js so the consolidated dashboard can flash a
// locally-attached node over Web Serial. Keep it in sync with web/nrf-dfu.js.
//
//go:embed nrf-dfu.js
var dfuJS []byte

const maxEvents = 300

var errNoKey = errors.New("no controller key — start with -import-backup or -mint")

// Server holds the live graph, a ring buffer of recent policy records, and (for the write
// API) the controller key + the serial send func.
const maxConsole = 200

type Server struct {
	graph   *topo.Graph
	ks      *keystore.Store
	send    func(string) error
	ui      *uiState
	mu      sync.Mutex
	events  []policy.Record
	console []string
}

// New builds the server. uiPath persists aliases + map positions ("" = in-memory only).
func New(graph *topo.Graph, ks *keystore.Store, send func(string) error, uiPath string) *Server {
	return &Server{graph: graph, ks: ks, send: send, ui: loadUI(uiPath)}
}

// Console records a raw node console line for the dashboard's console pane.
func (s *Server) Console(line string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.console = append(s.console, line)
	if len(s.console) > maxConsole {
		s.console = s.console[len(s.console)-maxConsole:]
	}
}

// Sink is the policy.Logger subscriber: it appends each record to the ring buffer.
func (s *Server) Sink(r policy.Record) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.events = append(s.events, r)
	if len(s.events) > maxEvents {
		s.events = s.events[len(s.events)-maxEvents:]
	}
}

func hexID(s string) uint32 {
	v, _ := strconv.ParseUint(s, 16, 32)
	return uint32(v)
}

// issue signs (where needed) and sends one command. `raw` console lines need only the
// serial link; block/power need the controller key.
func (s *Server) issue(c cmdReq) (string, error) {
	if c.Action == "raw" {
		if c.Line == "" {
			return "", errors.New("empty console line")
		}
		return "sent: " + c.Line, s.send(c.Line)
	}
	if s.ks == nil {
		return "", errNoKey
	}
	ctr, err := s.ks.Next()
	if err != nil {
		return "", err
	}
	var line string
	switch c.Action {
	case "power":
		line, err = commander.Power(hexID(c.Node), int8(c.Dbm), ctr, s.ks.Priv())
	case "confirm":
		line, err = commander.Confirm(hexID(c.Node), int8(c.Dbm), ctr, s.ks.Priv())
	case "block":
		line, err = commander.Block(hexID(c.Node), hexID(c.Victim), int8(c.Ttl), ctr, s.ks.Priv())
	case "unblock":
		line, err = commander.Unblock(hexID(c.Node), hexID(c.Victim), ctr, s.ks.Priv())
	default:
		return "", errors.New("unknown action " + c.Action)
	}
	if err != nil {
		return "", err
	}
	return c.Action + " " + c.Node + " queued (ctr=" + strconv.FormatUint(uint64(ctr), 10) + ")", s.send(line)
}

type cmdReq struct {
	Action string `json:"action"` // raw | power | confirm | block | unblock
	Node   string `json:"node"`   // hex id (recipient)
	Victim string `json:"victim"` // hex id (block/unblock)
	Dbm    int    `json:"dbm"`
	Ttl    int    `json:"ttl"`
	Line   string `json:"line"` // raw console line
}

type stateJSON struct {
	Snapshot  topo.Snapshot        `json:"snapshot"`
	Events    []policy.Record      `json:"events"`
	Console   []string             `json:"console"`
	Aliases   map[string]string    `json:"aliases"`
	Positions map[string][]float64 `json:"positions"`
	HasKey    bool                 `json:"has_key"`
	Pub       string               `json:"pub,omitempty"`
}

type uiReq struct {
	Type string  `json:"type"` // alias | pos
	ID   string  `json:"id"`
	Name string  `json:"name"`
	X    float64 `json:"x"`
	Y    float64 `json:"y"`
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
	mux.HandleFunc("/nrf-dfu.js", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/javascript; charset=utf-8")
		_, _ = w.Write(dfuJS)
	})
	mux.HandleFunc("/api/state", func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		ev := make([]policy.Record, len(s.events))
		copy(ev, s.events)
		con := make([]string, len(s.console))
		copy(con, s.console)
		s.mu.Unlock()
		al, pos := s.ui.snapshot()
		st := stateJSON{Snapshot: s.graph.Snapshot(), Events: ev, Console: con,
			Aliases: al, Positions: pos, HasKey: s.ks != nil}
		if s.ks != nil {
			st.Pub = s.ks.PubHex()
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(st)
	})
	mux.HandleFunc("/api/ui", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST only", http.StatusMethodNotAllowed)
			return
		}
		var u uiReq
		if err := json.NewDecoder(r.Body).Decode(&u); err != nil || u.ID == "" {
			writeJSON(w, map[string]any{"ok": false, "msg": "bad request"})
			return
		}
		switch u.Type {
		case "alias":
			s.ui.setAlias(u.ID, u.Name)
		case "pos":
			s.ui.setPos(u.ID, u.X, u.Y)
		default:
			writeJSON(w, map[string]any{"ok": false, "msg": "unknown ui type"})
			return
		}
		writeJSON(w, map[string]any{"ok": true})
	})
	mux.HandleFunc("/api/cmd", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST only", http.StatusMethodNotAllowed)
			return
		}
		var c cmdReq
		if err := json.NewDecoder(r.Body).Decode(&c); err != nil {
			writeJSON(w, map[string]any{"ok": false, "msg": "bad json: " + err.Error()})
			return
		}
		msg, err := s.issue(c)
		if err != nil {
			writeJSON(w, map[string]any{"ok": false, "msg": err.Error()})
			return
		}
		writeJSON(w, map[string]any{"ok": true, "msg": msg})
	})
	return mux
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}
