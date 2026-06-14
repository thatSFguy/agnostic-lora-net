// Command agnctl is the Phase 4 controller (Go, laptop-first). It reads a tethered
// node's console stream (or replays a saved log), builds the live topology, passively
// records RF-utilisation/chattiness data, and — with a controller key — signs and pushes
// POWER/BLOCK commands into the mesh. See docs/phase4-controller-plan.md.
//
//	# live: collect + capture + issue signed commands (interactive, on stdin):
//	agnctl -port /dev/ttyACM0 -csv capture.csv
//	# reuse the browser controller key from a map-app backup (one time):
//	agnctl -port /dev/ttyACM0 -import-backup agnlora-controller-backup.json
//	# production: mint a fresh controller key:
//	agnctl -port /dev/ttyACM0 -mint
//	# replay a saved console log (no hardware, no commands):
//	agnctl -file session.log
//
// Interactive commands (serial mode): power <id> <dbm> | confirm <id> <dbm> |
// block <recip> <victim> [ttlMin] | unblock <recip> <victim> | pub | help
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"net/http"
	"path/filepath"

	"agnostic-lora-net/controller/internal/capture"
	"agnostic-lora-net/controller/internal/commander"
	"agnostic-lora-net/controller/internal/httpd"
	"agnostic-lora-net/controller/internal/ingest"
	"agnostic-lora-net/controller/internal/keystore"
	"agnostic-lora-net/controller/internal/policy"
	"agnostic-lora-net/controller/internal/topo"
)

func main() {
	var (
		port      = flag.String("port", "", "serial device of the tethered node (e.g. /dev/ttyACM0)")
		file      = flag.String("file", "", "replay console lines from a file ('-' = stdin) instead of serial")
		baud      = flag.Int("baud", 115200, "serial baud (USB-CDC usually ignores this)")
		csvPath   = flag.String("csv", "capture.csv", "passive-capture CSV output path")
		poll      = flag.Bool("poll", true, "on serial: send `trace on` then poll info/nbrdump")
		summary   = flag.Duration("summary", 10*time.Second, "how often to print a chattiness summary")
		snapshot  = flag.String("snapshot", "", "on exit, write the final topology JSON here")
		keydir    = flag.String("keydir", "state", "directory holding the controller key + replay counter")
		importBak = flag.String("import-backup", "", "adopt the controller key from a map-app backup JSON (reuse the browser key)")
		mint      = flag.Bool("mint", false, "mint a fresh controller key (production) instead of loading/importing")

		optimize  = flag.Bool("optimize", false, "run the autonomous RF power-optimisation loop")
		apply     = flag.Bool("apply", false, "with -optimize: actually send commands (default is dry-run, log only)")
		polLog    = flag.String("policy-log", "policy.jsonl", "policy decision audit trail (JSONL)")
		polEvery  = flag.Duration("policy-interval", 15*time.Second, "optimisation cycle interval")
		marginLo  = flag.Float64("margin-low", 6, "raise power below this SNR margin (dB)")
		marginHi  = flag.Float64("margin-high", 12, "lower power above this SNR margin (dB)")
		maxStep   = flag.Int("max-step", 3, "max dBm power change per cycle")
		heartbeat = flag.Duration("heartbeat", 2*time.Hour, "re-assert held nodes this often (keeps their flash-default watchdog fresh; must be < the node's 6h window)")
		httpAddr  = flag.String("http", "", "serve the live dashboard on this address (e.g. :8080)")
		fwDir     = flag.String("fwdir", "../web/fw", "directory of firmware packages to serve at /fw/ for the dashboard Flash tab")
	)
	flag.Parse()

	if *port == "" && *file == "" {
		fmt.Fprintln(os.Stderr, "need -port <dev> or -file <log|->. see -h")
		os.Exit(2)
	}

	src, serial, err := openSource(*port, *file, *baud)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open source: %v\n", err)
		os.Exit(1)
	}
	defer src.Close()

	logger, err := capture.NewLogger(*csvPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open capture csv: %v\n", err)
		os.Exit(1)
	}
	defer logger.Close()

	graph := topo.New()

	// Resolve the controller key (commands need it; pure collection/replay doesn't).
	ks, err := resolveKey(*keydir, *importBak, *mint, serial)
	if err != nil {
		fmt.Fprintf(os.Stderr, "controller key: %v\n", err)
		os.Exit(1)
	}
	if ks != nil {
		fmt.Fprintf(os.Stderr, "controller key %s — provision nodes with:  ctrlkey %s\n", ks.PubHex(), ks.PubHex())
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if serial && *poll {
		go pollLoop(ctx, src, graph)
	}
	if serial && ks != nil {
		go commandREPL(ctx, src, ks)
	}

	// Live web dashboard (read-only): topology + the streaming decision feed.
	var dash *httpd.Server
	if *httpAddr != "" {
		dash = httpd.New(graph, ks, src.Send, filepath.Join(*keydir, "ui.json"), *fwDir)
		go func() {
			if err := http.ListenAndServe(*httpAddr, dash.Handler()); err != nil {
				fmt.Fprintf(os.Stderr, "http: %v\n", err)
			}
		}()
		fmt.Fprintf(os.Stderr, "dashboard: http://localhost%s\n", *httpAddr)
	}

	var eng *policy.Engine
	if *optimize {
		if *apply && !serial {
			fmt.Fprintln(os.Stderr, "-apply needs a tethered node (-port); -optimize alone previews on a replay (dry-run)")
			os.Exit(2)
		}
		if *apply && ks == nil {
			fmt.Fprintln(os.Stderr, "-apply needs a controller key (use -mint or -import-backup)")
			os.Exit(2)
		}
		plog, err := policy.NewLogger(*polLog)
		if err != nil {
			fmt.Fprintf(os.Stderr, "policy log: %v\n", err)
			os.Exit(1)
		}
		defer plog.Close()
		if dash != nil {
			plog.Subscribe(dash.Sink) // stream decisions to the web feed
		}
		cfg := policy.DefaultConfig()
		cfg.MarginLow, cfg.MarginHigh, cfg.MaxStep = *marginLo, *marginHi, int8(*maxStep)
		eng = policy.NewEngine(cfg, plog, ks, src.Send, *apply, 3*(*polEvery), *heartbeat)
		mode := "dry-run (log only)"
		if *apply {
			mode = "APPLY"
		}
		fmt.Fprintf(os.Stderr, "optimiser: %s, band [%.0f,%.0f] dB, ≤%d dB/cycle every %s → %s\n",
			mode, *marginLo, *marginHi, *maxStep, *polEvery, *polLog)
		go func() {
			t := time.NewTicker(*polEvery)
			defer t.Stop()
			for {
				select {
				case <-ctx.Done():
					return
				case <-t.C:
					eng.Tick(graph.Snapshot(), time.Now())
				}
			}
		}()
	}

	srcName := *file
	if serial {
		srcName = *port
	}
	fmt.Fprintf(os.Stderr, "agnctl: reading %s → capture %s (Ctrl-C to stop)\n", srcName, *csvPath)

	lines := src.Lines()
	sumTick := time.NewTicker(*summary)
	defer sumTick.Stop()

loop:
	for {
		select {
		case <-ctx.Done():
			break loop
		case <-sumTick.C:
			fmt.Fprintln(os.Stderr, logger.Summary())
		case line, ok := <-lines:
			if !ok {
				break loop // stream ended (e.g. end of replay file)
			}
			now := time.Now()
			e, _ := ingest.ParseLine(line)
			graph.Apply(e, now)
			logger.Log(e, now)
			if dash != nil {
				dash.Console(line) // raw console line -> dashboard console pane
			}
			if eng != nil && e.Kind == ingest.KindCtrlAck {
				eng.NoteAck(now, e.ID, e.Num["applied"], e.Num["provisional"])
			}
		}
	}

	if eng != nil {
		eng.Tick(graph.Snapshot(), time.Now()) // final evaluation — drives the no-hardware replay preview
	}
	fmt.Fprintln(os.Stderr, "\n"+logger.Summary())
	if *snapshot != "" {
		if err := writeSnapshot(*snapshot, graph.Snapshot()); err != nil {
			fmt.Fprintf(os.Stderr, "snapshot: %v\n", err)
		} else {
			fmt.Fprintf(os.Stderr, "wrote topology snapshot → %s\n", *snapshot)
		}
	}
}

// resolveKey loads/imports/mints the controller key. Returns nil (no key) only for a
// read-only replay with no explicit key request.
func resolveKey(keydir, importBak string, mint, serial bool) (*keystore.Store, error) {
	switch {
	case importBak != "":
		return keystore.ImportBrowserBackup(keydir, importBak)
	case mint:
		return keystore.Mint(keydir)
	default:
		ks, err := keystore.Open(keydir)
		if err == nil {
			return ks, nil
		}
		if !os.IsNotExist(err) {
			return nil, err
		}
		if serial {
			ks, err := keystore.Mint(keydir)
			if err != nil {
				return nil, err
			}
			fmt.Fprintln(os.Stderr, "no keystore found — minted a fresh controller key (use -import-backup to reuse the browser key)")
			return ks, nil
		}
		return nil, nil // replay mode: commands not available, that's fine
	}
}

func openSource(port, file string, baud int) (ingest.Source, bool, error) {
	if file != "" {
		if file == "-" {
			return ingest.NewReaderSource(os.Stdin), false, nil
		}
		f, err := os.Open(file)
		if err != nil {
			return nil, false, err
		}
		return ingest.NewReaderSource(f), false, nil
	}
	// Resilient: reconnects across node reboots / USB re-enumeration instead of exiting,
	// and starts even if the gateway isn't attached yet — so the dashboard (incl. the
	// Configure/Flash tabs, which don't use the gateway) stays up across node blips.
	s := ingest.NewReconnectingSerial(port, baud, func(m string) { fmt.Fprintln(os.Stderr, m) })
	return s, true, nil
}

func pollLoop(ctx context.Context, src ingest.Source, graph *topo.Graph) {
	time.Sleep(400 * time.Millisecond)
	_ = src.Send("trace on")
	t := time.NewTicker(3 * time.Second)
	defer t.Stop()
	tick, rr := 0, 0
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			_ = src.Send("info")
			if tick%3 == 0 {
				_ = src.Send("nbrdump")
			}
			// Round-robin one remote telemetry query per tick. The reply (a routed `[status] N`
			// + its `nbr …` table) feeds the mesh-wide topology the optimiser needs to tune
			// nodes that can't reach the gateway directly. One per tick keeps airtime modest;
			// each known node is polled every (3s × node-count).
			if ids := graph.ManagedIDs(); len(ids) > 0 {
				_ = src.Send("status " + ids[rr%len(ids)])
				rr++
			}
			tick++
		}
	}
}

// commandREPL reads controller commands from stdin and pushes signed control messages to
// the tethered node. Acks come back through the normal console stream (KindCtrlAck).
func commandREPL(ctx context.Context, src ingest.Source, ks *keystore.Store) {
	fmt.Fprintln(os.Stderr, "commands: power <id> <dbm> | confirm <id> <dbm> | block <recip> <victim> [ttlMin] | unblock <recip> <victim> | pub | help")
	sc := bufio.NewScanner(os.Stdin)
	for sc.Scan() {
		if ctx.Err() != nil {
			return
		}
		handleCommand(strings.TrimSpace(sc.Text()), src, ks)
	}
}

func parseID(s string) (uint32, error) {
	v, err := strconv.ParseUint(strings.TrimPrefix(s, "0x"), 16, 32)
	return uint32(v), err
}

func handleCommand(line string, src ingest.Source, ks *keystore.Store) {
	f := strings.Fields(line)
	if len(f) == 0 {
		return
	}
	warn := func(s string) { fmt.Fprintf(os.Stderr, "  ! %s\n", s) }
	send := func(consoleLine string, err error) {
		if err != nil {
			warn(err.Error())
			return
		}
		if err := src.Send(consoleLine); err != nil {
			warn("send: " + err.Error())
			return
		}
		fmt.Fprintln(os.Stderr, "  → command pushed (watch for [ctrl] ack)")
	}

	switch f[0] {
	case "pub":
		fmt.Fprintf(os.Stderr, "  controller pub = %s\n", ks.PubHex())
	case "help":
		fmt.Fprintln(os.Stderr, "  power <id> <dbm> | confirm <id> <dbm> | block <recip> <victim> [ttlMin] | unblock <recip> <victim> | pub")
	case "power", "confirm":
		if len(f) != 3 {
			warn("usage: " + f[0] + " <hexid> <dbm>")
			return
		}
		id, e1 := parseID(f[1])
		dbm, e2 := strconv.Atoi(f[2])
		if e1 != nil || e2 != nil {
			warn("bad args")
			return
		}
		ctr, err := ks.Next()
		if err != nil {
			warn(err.Error())
			return
		}
		if f[0] == "power" {
			send(commander.Power(id, int8(dbm), ctr, ks.Priv()))
		} else {
			send(commander.Confirm(id, int8(dbm), ctr, ks.Priv()))
		}
	case "block", "unblock":
		if len(f) < 3 {
			warn("usage: " + f[0] + " <recipientHex> <victimHex>" + map[string]string{"block": " [ttlMin]"}[f[0]])
			return
		}
		recip, e1 := parseID(f[1])
		victim, e2 := parseID(f[2])
		if e1 != nil || e2 != nil {
			warn("bad args")
			return
		}
		ctr, err := ks.Next()
		if err != nil {
			warn(err.Error())
			return
		}
		if f[0] == "block" {
			ttl := 0
			if len(f) >= 4 {
				ttl, _ = strconv.Atoi(f[3])
			}
			send(commander.Block(recip, victim, int8(ttl), ctr, ks.Priv()))
		} else {
			send(commander.Unblock(recip, victim, ctr, ks.Priv()))
		}
	default:
		warn("unknown command " + f[0] + " (try help)")
	}
}

func writeSnapshot(path string, s topo.Snapshot) error {
	s.At = time.Now().UnixMilli()
	b, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, b, 0o644)
}
