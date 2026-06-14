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
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"agnostic-lora-net/controller/internal/commander"
	"agnostic-lora-net/controller/internal/keystore"
	"agnostic-lora-net/controller/internal/policy"
	"agnostic-lora-net/controller/internal/sign"
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
	fwDir   string // firmware packages served at /fw/ for the Flash tab ("" = disabled)
	mu      sync.Mutex
	events  []policy.Record
	console []string
}

// New builds the server. uiPath persists aliases + map positions ("" = in-memory only).
// fwDir is the directory of firmware packages served at /fw/ ("" disables it).
func New(graph *topo.Graph, ks *keystore.Store, send func(string) error, uiPath, fwDir string) *Server {
	return &Server{graph: graph, ks: ks, send: send, ui: loadUI(uiPath), fwDir: fwDir}
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

// BackupName is the suggested filename for an exported controller backup.
const BackupName = "agnlora-controller-backup.json"

// backupJSON assembles a map-app-compatible controller backup (key + counter + aliases +
// positions). priv = base64 PKCS#8, pub = hex — round-trips through ImportBrowserBackup.
func backupJSON(ks *keystore.Store, aliases map[string]string, positions map[string][]float64) ([]byte, error) {
	if ks == nil {
		return nil, errors.New("no controller key to export")
	}
	priv, pub, ctr, err := ks.Export()
	if err != nil {
		return nil, err
	}
	type ckey struct {
		Priv string `json:"priv"`
		Pub  string `json:"pub"`
	}
	return json.MarshalIndent(struct {
		Warning           string               `json:"_warning"`
		Exported          string               `json:"exported"`
		ControllerKey     ckey                 `json:"controllerKey"`
		ControllerCounter uint32               `json:"controllerCounter"`
		Aliases           map[string]string    `json:"aliases"`
		Positions         map[string][]float64 `json:"positions"`
		Allowlist         map[string]int64     `json:"allowlist,omitempty"`
	}{
		Warning:           "agnostic-lora-net controller backup — contains the network WRITE key. Keep it secret; re-importing seeds a fresh replay counter.",
		Exported:          time.Now().UTC().Format(time.RFC3339),
		ControllerKey:     ckey{Priv: priv, Pub: pub},
		ControllerCounter: ctr,
		Aliases:           aliases,
		Positions:         positions,
		Allowlist:         ks.Allowlist(),
	}, "", "  ")
}

// ExportBackup builds a controller backup from a keystore + a ui.json path — for the CLI
// `-export`, which runs without a live server.
func ExportBackup(ks *keystore.Store, uiPath string) ([]byte, error) {
	al, pos := loadUI(uiPath).snapshot()
	return backupJSON(ks, al, pos)
}

// parseBackupUI pulls aliases + positions out of a backup JSON (absent -> nil -> empty).
func parseBackupUI(b []byte) (map[string]string, map[string][]float64) {
	var bk struct {
		Aliases   map[string]string    `json:"aliases"`
		Positions map[string][]float64 `json:"positions"`
	}
	_ = json.Unmarshal(b, &bk)
	return bk.Aliases, bk.Positions
}

// parseBackupAllowlist pulls the membership allowlist out of a backup JSON (absent -> nil).
func parseBackupAllowlist(b []byte) map[string]int64 {
	var bk struct {
		Allowlist map[string]int64 `json:"allowlist"`
	}
	_ = json.Unmarshal(b, &bk)
	return bk.Allowlist
}

// RestoreToDisk restores a backup (key + aliases + positions) into keydir on a fresh
// install — for the CLI `-restore`, no running server. Returns the restored pubkey.
func RestoreToDisk(keydir, uiPath string, backup []byte) (string, error) {
	ks, err := keystore.RestoreKey(keydir, backup)
	if err != nil {
		return "", err
	}
	al, pos := parseBackupUI(backup)
	loadUI(uiPath).replaceAll(al, pos)
	if acl := parseBackupAllowlist(backup); len(acl) > 0 {
		_ = ks.LoadAllowlist(acl)
	}
	return ks.PubHex(), nil
}

// hexID decodes a 32-hex node id (v2). A malformed id yields the zero NodeID, which the
// commander builders reject (ErrBadTarget) — so a bad id surfaces as an error, never a command.
func hexID(s string) sign.NodeID {
	id, _ := sign.ParseNodeID(s)
	return id
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
	// BLE on the tethered gateway: drive the existing `ble on/off` console command
	// directly — no key, no counter, no mesh round-trip. Works today. (Remote nodes fall
	// through to the signed CTRL_BLE path below, which needs the firmware handler.)
	if c.Action == "ble" && c.Node != "" && c.Node == s.graph.GatewayID() {
		verb := "off"
		if c.On {
			verb = "on"
		}
		return "BLE " + verb + " → gateway (direct console)", s.send("ble " + verb)
	}
	if s.ks == nil {
		return "", errNoKey
	}
	// Membership gate: refuse to command a node that isn't a verified, approved member —
	// BEFORE advancing the replay counter, so a rejected command never burns a counter.
	// Permissive when no ACL is configured (graph.CommandAllowed returns ok). `raw` and the
	// gateway-direct BLE path above are operator-direct and intentionally skip this gate.
	if pub, ok := s.graph.CommandAllowed(c.Node); !ok {
		if pub == "" {
			return "", fmt.Errorf("node %s is not a verified member (no valid signed announce seen yet)", c.Node)
		}
		short := pub
		if len(short) > 12 {
			short = short[:12]
		}
		return "", fmt.Errorf("node %s is verified but not approved — approve pubkey %s… in the Security tab", c.Node, short)
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
	case "ble":
		// Remote node: signed CTRL_BLE (gateway was handled directly above). Needs the
		// firmware CTRL_BLE handler (todo #2 firmware half) to take effect on-device.
		line, err = commander.Ble(hexID(c.Node), c.On, ctr, s.ks.Priv())
	case "retune":
		// Signed CTRL_RETUNE: change the node's PHY over the air (PHY only — power stays under
		// CTRL_POWER). The node validates + acks on the new PHY; recovery is the BLE-rescue flow.
		line, err = commander.Retune(hexID(c.Node), sign.RetuneCfg{
			FreqHz: uint32(c.FreqHz), BwHz: uint32(c.BwHz),
			SF: uint8(c.SF), CR: uint8(c.CR), Sync: uint8(c.Sync), Preamble: uint16(c.Preamble),
		}, ctr, s.ks.Priv())
	default:
		return "", errors.New("unknown action " + c.Action)
	}
	if err != nil {
		return "", err
	}
	return c.Action + " " + c.Node + " queued (ctr=" + strconv.FormatUint(uint64(ctr), 10) + ")", s.send(line)
}

type cmdReq struct {
	Action string `json:"action"` // raw | power | confirm | block | unblock | ble
	Node   string `json:"node"`   // hex id (recipient)
	Victim string `json:"victim"` // hex id (block/unblock)
	Dbm    int    `json:"dbm"`
	Ttl    int    `json:"ttl"`
	On     bool   `json:"on"` // ble: true = enable advertising, false = disable
	// retune: the PHY to apply (PHY only — TX power stays under power/confirm).
	FreqHz   int    `json:"freq_hz"`
	BwHz     int    `json:"bw_hz"`
	SF       int    `json:"sf"`
	CR       int    `json:"cr"`
	Sync     int    `json:"sync"`
	Preamble int    `json:"preamble"`
	Line     string `json:"line"` // raw console line
}

type stateJSON struct {
	Snapshot  topo.Snapshot        `json:"snapshot"`
	Events    []policy.Record      `json:"events"`
	Console   []string             `json:"console"`
	Aliases   map[string]string    `json:"aliases"`
	Positions map[string][]float64 `json:"positions"`
	HasKey    bool                 `json:"has_key"`
	Pub       string               `json:"pub,omitempty"`
	Allowlist map[string]int64     `json:"allowlist,omitempty"` // approved pubkeyHex → approval unix-secs
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
	if s.fwDir != "" {
		// Serve firmware packages (UF2 / .dfu.json) so the Flash tab works without the
		// public GitHub release. http.FileServer cleans paths (no traversal above fwDir).
		mux.Handle("/fw/", http.StripPrefix("/fw/", http.FileServer(http.Dir(s.fwDir))))
	}
	mux.HandleFunc("/api/backup", func(w http.ResponseWriter, r *http.Request) {
		// Download the controller key + nodes (map-app-compatible). Localhost-only by the
		// SECURITY note above — this serves the private WRITE key.
		al, pos := s.ui.snapshot()
		b, err := backupJSON(s.ks, al, pos)
		if err != nil {
			http.Error(w, err.Error(), http.StatusNotFound)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Content-Disposition", `attachment; filename="`+BackupName+`"`)
		_, _ = w.Write(b)
	})
	mux.HandleFunc("/api/restore", func(w http.ResponseWriter, r *http.Request) {
		// Live restore: swap in the backup's key (in-place — engine/REPL/dashboard all see
		// it) and bulk-restore aliases/positions. Localhost-only (it accepts a private key).
		if r.Method != http.MethodPost {
			http.Error(w, "POST only", http.StatusMethodNotAllowed)
			return
		}
		if s.ks == nil {
			writeJSON(w, map[string]any{"ok": false, "msg": "this controller has no key store — restore on a fresh install with the CLI: agnctl -restore backup.json"})
			return
		}
		b, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
		if err != nil {
			writeJSON(w, map[string]any{"ok": false, "msg": "read: " + err.Error()})
			return
		}
		if err := s.ks.Adopt(b); err != nil {
			writeJSON(w, map[string]any{"ok": false, "msg": err.Error()})
			return
		}
		al, pos := parseBackupUI(b)
		s.ui.replaceAll(al, pos)
		acl := parseBackupAllowlist(b)
		_ = s.ks.LoadAllowlist(acl) // restore membership too (nil -> empties it)
		writeJSON(w, map[string]any{"ok": true, "msg": "restored — controller key " + s.ks.PubHex()[:12] + "… + " + strconv.Itoa(len(al)) + " aliases + " + strconv.Itoa(len(acl)) + " approved nodes"})
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
			st.Allowlist = s.ks.Allowlist()
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
	mux.HandleFunc("/api/acl", func(w http.ResponseWriter, r *http.Request) {
		// Membership management: approve/revoke a node pubkey on the allowlist. Localhost-only
		// by the SECURITY note (it mutates the WRITE-key's policy). Approval is keyed on the
		// verified pubkey, not the node id, so spoofing an id can't grant membership.
		if r.Method != http.MethodPost {
			http.Error(w, "POST only", http.StatusMethodNotAllowed)
			return
		}
		if s.ks == nil {
			writeJSON(w, map[string]any{"ok": false, "msg": errNoKey.Error()})
			return
		}
		var a aclReq
		if err := json.NewDecoder(r.Body).Decode(&a); err != nil {
			writeJSON(w, map[string]any{"ok": false, "msg": "bad json: " + err.Error()})
			return
		}
		pub := strings.ToUpper(strings.TrimSpace(a.Pub))
		if len(pub) != 64 {
			writeJSON(w, map[string]any{"ok": false, "msg": "pub must be a 64-hex node pubkey"})
			return
		}
		var err error
		switch a.Action {
		case "approve":
			err = s.ks.Approve(pub, time.Now().Unix())
		case "revoke":
			err = s.ks.Revoke(pub)
		default:
			writeJSON(w, map[string]any{"ok": false, "msg": "unknown acl action " + a.Action})
			return
		}
		if err != nil {
			writeJSON(w, map[string]any{"ok": false, "msg": err.Error()})
			return
		}
		writeJSON(w, map[string]any{"ok": true, "msg": a.Action + " " + pub[:12] + "…"})
	})
	return mux
}

type aclReq struct {
	Action string `json:"action"` // approve | revoke
	Pub    string `json:"pub"`    // node pubkey hex (64 chars)
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}
