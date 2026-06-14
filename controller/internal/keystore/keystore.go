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
	"strings"
	"sync"
	"time"
)

const stateFile = "controller.json"

type persisted struct {
	SeedHex   string           `json:"seed_hex"`
	Counter   uint32           `json:"counter"`
	Allowlist map[string]int64 `json:"allowlist,omitempty"` // pubkeyHex(upper) → approved unix-secs
}

// Store is the in-memory key + counter + membership allowlist, backed by
// <dir>/controller.json. The allowlist gates which nodes the controller will manage: a
// node is "allowed" only if its verified pubkey appears here (see self-certifying-identity
// plan §6). It is controller policy, persisted with the key and carried in backups.
type Store struct {
	mu      sync.Mutex
	dir     string
	priv    ed25519.PrivateKey
	counter uint32
	allow   map[string]int64
}

// normPub canonicalises a pubkey hex for allowlist keys: upper-case, trimmed. The firmware
// emits uppercase pubkeys (loc_id_hex %02X) and Export() uppercases too, so this keeps the
// allowlist keyed consistently regardless of caller case.
func normPub(pubHex string) string { return strings.ToUpper(strings.TrimSpace(pubHex)) }

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
	allow := p.Allowlist
	if allow == nil {
		allow = map[string]int64{}
	}
	return &Store{dir: dir, priv: ed25519.NewKeyFromSeed(seed), counter: p.Counter, allow: allow}, nil
}

// Mint generates a fresh key (production) and persists it with a wall-clock-seeded counter.
func Mint(dir string) (*Store, error) {
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return nil, err
	}
	return newStore(dir, priv)
}

// ParseBackupPriv extracts the Ed25519 private key from a map-app / controller backup JSON
// (the full {"controllerKey":{priv,pub}} or the bare {priv,pub}). priv is base64 PKCS#8.
func ParseBackupPriv(b []byte) (ed25519.PrivateKey, error) {
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
	return priv, nil
}

// ImportBrowserBackup reads a backup JSON file and adopts its controller key, persisting a
// fresh store in dir. Used at startup (-import-backup).
func ImportBrowserBackup(dir, backupPath string) (*Store, error) {
	b, err := os.ReadFile(backupPath)
	if err != nil {
		return nil, err
	}
	priv, err := ParseBackupPriv(b)
	if err != nil {
		return nil, err
	}
	return newStore(dir, priv)
}

// RestoreKey adopts the key from backup bytes into a fresh store at dir (for the CLI
// `-restore`, which has the bytes in hand).
func RestoreKey(dir string, backup []byte) (*Store, error) {
	priv, err := ParseBackupPriv(backup)
	if err != nil {
		return nil, err
	}
	return newStore(dir, priv)
}

// Adopt swaps in the key from backup bytes IN PLACE (every holder of this *Store sees it),
// re-seeding the replay counter from wall-clock so it clears each node's stored floor.
// For a live restore on a running controller.
func (s *Store) Adopt(backup []byte) error {
	priv, err := ParseBackupPriv(backup)
	if err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.priv = priv
	s.counter = uint32(time.Now().Unix())
	return s.save()
}

func newStore(dir string, priv ed25519.PrivateKey) (*Store, error) {
	s := &Store{dir: dir, priv: priv, counter: uint32(time.Now().Unix()), allow: map[string]int64{}}
	if err := s.save(); err != nil {
		return nil, err
	}
	return s, nil
}

// Approve adds (or refreshes) a pubkey on the membership allowlist with the given approval
// time (unix secs; pass it in so the store stays testable). Persists immediately.
func (s *Store) Approve(pubHex string, atUnix int64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.allow == nil {
		s.allow = map[string]int64{}
	}
	s.allow[normPub(pubHex)] = atUnix
	return s.save()
}

// Revoke removes a pubkey from the allowlist. Persists immediately. No error if absent.
func (s *Store) Revoke(pubHex string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.allow, normPub(pubHex))
	return s.save()
}

// IsAllowed reports whether a pubkey is on the membership allowlist.
func (s *Store) IsAllowed(pubHex string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, ok := s.allow[normPub(pubHex)]
	return ok
}

// Allowlist returns a copy of the allowlist (pubkeyHex → approved unix-secs) for the UI/CLI.
func (s *Store) Allowlist() map[string]int64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make(map[string]int64, len(s.allow))
	for k, v := range s.allow {
		out[k] = v
	}
	return out
}

// LoadAllowlist replaces the allowlist wholesale (a backup restore) and persists. Keys are
// re-normalised so a backup written by any case round-trips cleanly.
func (s *Store) LoadAllowlist(al map[string]int64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.allow = make(map[string]int64, len(al))
	for k, v := range al {
		s.allow[normPub(k)] = v
	}
	return s.save()
}

// Pub is the controller public key — provision it on each node with `ctrlkey <hex>`.
// Pub/PubHex/Priv lock so a live Adopt() can't tear the key field mid-read.
func (s *Store) Pub() ed25519.PublicKey { s.mu.Lock(); defer s.mu.Unlock(); return s.priv.Public().(ed25519.PublicKey) }
func (s *Store) PubHex() string         { return hex.EncodeToString(s.Pub()) }
func (s *Store) Priv() ed25519.PrivateKey { s.mu.Lock(); defer s.mu.Unlock(); return s.priv }

// Export returns the key + replay counter in the same encoding the map app's backup uses
// (priv = base64 PKCS#8, pub = uppercase hex), so the result round-trips through
// ImportBrowserBackup and the browser. Counter is informational — an import re-seeds it.
func (s *Store) Export() (privB64, pubHex string, counter uint32, err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	der, err := x509.MarshalPKCS8PrivateKey(s.priv)
	if err != nil {
		return "", "", 0, err
	}
	pub := s.priv.Public().(ed25519.PublicKey) // compute directly — s.Pub() would re-lock s.mu (deadlock)
	return base64.StdEncoding.EncodeToString(der), strings.ToUpper(hex.EncodeToString(pub)), s.counter, nil
}

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
	p := persisted{SeedHex: hex.EncodeToString(s.priv.Seed()), Counter: s.counter, Allowlist: s.allow}
	b, err := json.MarshalIndent(p, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path(s.dir), b, 0o600)
}
