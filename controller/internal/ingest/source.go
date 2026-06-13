package ingest

import (
	"bufio"
	"errors"
	"io"
	"os"
	"os/exec"
	"strconv"
	"sync"
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
