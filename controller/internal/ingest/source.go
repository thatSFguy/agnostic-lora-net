package ingest

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strconv"
	"sync"
	"time"
)

// Source yields the node's console lines and (optionally) lets us send commands back.
// Two implementations: ReaderSource (file/stdin replay — read-only) and SerialSource
// (a tethered node over USB — bidirectional).
type Source interface {
	Lines() <-chan string  // closed when the stream ends
	Send(cmd string) error // appends '\n'; ErrReadOnly for replay sources
	Close() error
}

var ErrReadOnly = errors.New("ingest: source is read-only")

// ReaderSource streams lines from any io.Reader (a saved console log, or stdin). Perfect
// for replaying a capture or for tests — no hardware required.
type ReaderSource struct {
	rc io.ReadCloser
	ch chan string
}

func NewReaderSource(rc io.ReadCloser) *ReaderSource {
	s := &ReaderSource{rc: rc, ch: make(chan string, 256)}
	go func() {
		defer close(s.ch)
		sc := bufio.NewScanner(rc)
		sc.Buffer(make([]byte, 0, 4096), 1<<20)
		for sc.Scan() {
			s.ch <- sc.Text()
		}
	}()
	return s
}

func (s *ReaderSource) Lines() <-chan string { return s.ch }
func (s *ReaderSource) Send(string) error    { return ErrReadOnly }
func (s *ReaderSource) Close() error         { return s.rc.Close() }

// SerialSource talks to a tethered node over a USB-CDC tty (Linux/WSL/RPi). The CDC ACM
// link usually ignores the baud rate, but we still put the tty in raw mode best-effort.
type SerialSource struct {
	f   *os.File
	ch  chan string
	wmu sync.Mutex // serialise concurrent Send() (poll loop + command REPL)
}

func OpenSerial(dev string, baud int) (*SerialSource, error) {
	// Best-effort raw mode; ignore failure (USB-CDC ignores baud, and the device may
	// already be raw). We don't hard-depend on stty.
	_ = exec.Command("stty", "-F", dev, "raw", "-echo", strconv.Itoa(baud)).Run()

	f, err := os.OpenFile(dev, os.O_RDWR, 0)
	if err != nil {
		return nil, err
	}
	s := &SerialSource{f: f, ch: make(chan string, 256)}
	go func() {
		defer close(s.ch)
		sc := bufio.NewScanner(f)
		sc.Buffer(make([]byte, 0, 4096), 1<<20)
		for sc.Scan() {
			s.ch <- sc.Text()
		}
	}()
	return s, nil
}

func (s *SerialSource) Lines() <-chan string { return s.ch }

func (s *SerialSource) Send(cmd string) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.f.WriteString(cmd + "\n")
	return err
}

func (s *SerialSource) Close() error { return s.f.Close() }

// ReconnectingSerial wraps SerialSource so a tethered node dropping off the USB bus (reboot,
// re-enumeration — routine on WSL) doesn't end the program. Its Lines() channel stays open
// for the lifetime of the controller; it reopens the port with backoff and keeps relaying.
// Send() targets the live port, or errors while disconnected. This keeps the dashboard (and
// the Configure/Flash tabs, which don't even use the gateway) alive across node blips.
type ReconnectingSerial struct {
	dev  string
	baud int
	ch   chan string
	logf func(string)
	stop chan struct{}

	mu     sync.Mutex
	cur    *SerialSource
	closed bool
}

// NewReconnectingSerial returns immediately and connects in the background — so the
// controller starts (and serves) even if the gateway isn't attached yet. logf (may be nil)
// receives connect/disconnect/retry notices.
func NewReconnectingSerial(dev string, baud int, logf func(string)) *ReconnectingSerial {
	r := &ReconnectingSerial{dev: dev, baud: baud, ch: make(chan string, 256), logf: logf, stop: make(chan struct{})}
	go r.run()
	return r
}

func (r *ReconnectingSerial) log(format string, a ...any) {
	if r.logf != nil {
		r.logf(fmt.Sprintf(format, a...))
	}
}

func (r *ReconnectingSerial) run() {
	const minBackoff, maxBackoff = 500 * time.Millisecond, 5 * time.Second
	backoff := minBackoff
	for {
		select {
		case <-r.stop:
			return
		default:
		}
		s, err := OpenSerial(r.dev, r.baud)
		if err != nil {
			r.log("serial %s: open failed (%v) — retrying in %s", r.dev, err, backoff)
			select {
			case <-r.stop:
				return
			case <-time.After(backoff):
			}
			if backoff < maxBackoff {
				backoff *= 2
			}
			continue
		}
		backoff = minBackoff
		r.mu.Lock()
		r.cur = s
		r.mu.Unlock()
		r.log("serial %s: connected", r.dev)
		// Relay until this port EOFs (node rebooted / unplugged) or we're told to stop.
		for line := range s.Lines() {
			select {
			case r.ch <- line:
			case <-r.stop:
				s.Close()
				return
			}
		}
		r.mu.Lock()
		r.cur = nil
		r.mu.Unlock()
		s.Close()
		r.log("serial %s: disconnected — reconnecting", r.dev)
		select {
		case <-r.stop:
			return
		case <-time.After(backoff):
		}
	}
}

func (r *ReconnectingSerial) Lines() <-chan string { return r.ch } // stays open across reconnects

func (r *ReconnectingSerial) Send(cmd string) error {
	r.mu.Lock()
	s := r.cur
	r.mu.Unlock()
	if s == nil {
		return errors.New("ingest: serial not connected")
	}
	return s.Send(cmd)
}

func (r *ReconnectingSerial) Close() error {
	r.mu.Lock()
	if r.closed {
		r.mu.Unlock()
		return nil
	}
	r.closed = true
	close(r.stop)
	s := r.cur
	r.cur = nil
	r.mu.Unlock()
	if s != nil {
		return s.Close()
	}
	return nil
}
