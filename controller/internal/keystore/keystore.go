// Package keystore holds the controller's single Ed25519 signing key and its monotonic
// replay counter, persisted to disk. The key is the network's WRITE credential; the
// counter is the replay floor every node enforces (a command is accepted only if its
// counter exceeds the node's stored floor).
//
// Custody (per the operator): **reuse the browser key now** (ImportBrowserBackup), **mint
// fresh for production** (Mint). Either way the counter is seeded from wall-clock seconds
// on first init, so it always exceeds any counter a prior signer (the browser) used —
// avoiding the "controller restarts at 1, node already at 50, every command rejected" trap.
package keystore

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"
)

const stateFile = "controller.json"

type persisted struct {
	SeedHex string `json:"seed_hex"`
	Counter uint32 `json:"counter"`
}

// Store is the in-memory key + counter, backed by <dir>/controller.json.
type Store struct {
	mu      sync.Mutex
	dir     string
	priv    ed25519.PrivateKey
	counter uint32
}

func path(dir string) string { return filepath.Join(dir, stateFile) }

// Open loads an existing keystore. Returns os.ErrNotExist if none is present yet.
func Open(dir string) (*Store, error) {
	b, err := os.ReadFile(path(dir))
	if err != nil {
		return nil, err
	}
	var p persisted
	if err := json.Unmarshal(b, &p); err != nil {
		return nil, fmt.Errorf("keystore: parse %s: %w", path(dir), err)
	}
	seed, err := hex.DecodeString(p.SeedHex)
	if err != nil || len(seed) != ed25519.SeedSize {
		return nil, errors.New("keystore: bad seed in state file")
	}
	return &Store{dir: dir, priv: ed25519.NewKeyFromSeed(seed), counter: p.Counter}, nil
}

// Mint generates a fresh key (production) and persists it with a wall-clock-seeded counter.
func Mint(dir string) (*Store, error) {
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return nil, err
	}
	return newStore(dir, priv)
}

// ImportBrowserBackup reads the map app's backup JSON (or the raw {priv,pub} object) and
// adopts its controller key — "reuse the browser key". The priv field is base64 PKCS#8.
func ImportBrowserBackup(dir, backupPath string) (*Store, error) {
	b, err := os.ReadFile(backupPath)
	if err != nil {
		return nil, err
	}
	// Accept either the full backup ({"controllerKey":{priv,pub}}) or the bare key object.
	var outer struct {
		ControllerKey *struct {
			Priv string `json:"priv"`
			Pub  string `json:"pub"`
		} `json:"controllerKey"`
		Priv string `json:"priv"`
	}
	if err := json.Unmarshal(b, &outer); err != nil {
		return nil, fmt.Errorf("keystore: parse backup: %w", err)
	}
	privB64 := outer.Priv
	if outer.ControllerKey != nil {
		privB64 = outer.ControllerKey.Priv
	}
	if privB64 == "" {
		return nil, errors.New("keystore: no controller key in backup")
	}
	der, err := base64.StdEncoding.DecodeString(privB64)
	if err != nil {
		return nil, fmt.Errorf("keystore: base64 priv: %w", err)
	}
	key, err := x509.ParsePKCS8PrivateKey(der)
	if err != nil {
		return nil, fmt.Errorf("keystore: parse PKCS#8: %w", err)
	}
	priv, ok := key.(ed25519.PrivateKey)
	if !ok {
		return nil, errors.New("keystore: backup key is not Ed25519")
	}
	return newStore(dir, priv)
}

func newStore(dir string, priv ed25519.PrivateKey) (*Store, error) {
	s := &Store{dir: dir, priv: priv, counter: uint32(time.Now().Unix())}
	if err := s.save(); err != nil {
		return nil, err
	}
	return s, nil
}

// Pub is the controller public key — provision it on each node with `ctrlkey <hex>`.
func (s *Store) Pub() ed25519.PublicKey   { return s.priv.Public().(ed25519.PublicKey) }
func (s *Store) PubHex() string           { return hex.EncodeToString(s.Pub()) }
func (s *Store) Priv() ed25519.PrivateKey { return s.priv }

// Next advances the replay counter, persists it, and returns the new value. Persist
// happens BEFORE returning so a crash can never reuse a counter.
func (s *Store) Next() (uint32, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.counter++
	if err := s.save(); err != nil {
		s.counter-- // roll back the in-memory bump if we couldn't persist
		return 0, err
	}
	return s.counter, nil
}

func (s *Store) save() error {
	if err := os.MkdirAll(s.dir, 0o700); err != nil {
		return err
	}
	p := persisted{SeedHex: hex.EncodeToString(s.priv.Seed()), Counter: s.counter}
	b, err := json.MarshalIndent(p, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path(s.dir), b, 0o600)
}
