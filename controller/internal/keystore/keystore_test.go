package keystore

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestMintReloadCounterMonotonic(t *testing.T) {
	dir := t.TempDir()
	ks, err := Mint(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(ks.PubHex()) != 64 {
		t.Fatalf("pubhex len=%d", len(ks.PubHex()))
	}
	c1, _ := ks.Next()
	c2, _ := ks.Next()
	if c2 != c1+1 {
		t.Fatalf("counter not +1: %d -> %d", c1, c2)
	}

	// Reopen: same key, counter continues strictly above the last persisted value.
	ks2, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	if ks2.PubHex() != ks.PubHex() {
		t.Fatal("reloaded key differs")
	}
	c3, _ := ks2.Next()
	if c3 <= c2 {
		t.Fatalf("counter regressed after reload: %d then %d", c2, c3)
	}
}

func TestImportBrowserBackup(t *testing.T) {
	// Simulate the map app's exported backup: PKCS#8 base64 priv + hex pub.
	_, priv, _ := ed25519.GenerateKey(rand.Reader)
	der, err := x509.MarshalPKCS8PrivateKey(priv)
	if err != nil {
		t.Fatal(err)
	}
	pubHex := hex.EncodeToString(priv.Public().(ed25519.PublicKey))
	backup := map[string]any{
		"controllerKey": map[string]string{
			"priv": base64.StdEncoding.EncodeToString(der),
			"pub":  pubHex,
		},
	}
	bj, _ := json.Marshal(backup)
	bpath := filepath.Join(t.TempDir(), "backup.json")
	if err := os.WriteFile(bpath, bj, 0o600); err != nil {
		t.Fatal(err)
	}

	dir := t.TempDir()
	ks, err := ImportBrowserBackup(dir, bpath)
	if err != nil {
		t.Fatal(err)
	}
	if ks.PubHex() != pubHex {
		t.Fatalf("imported pub %s want %s", ks.PubHex(), pubHex)
	}
	// And it persisted: Open recovers the same key.
	ks2, err := Open(dir)
	if err != nil || ks2.PubHex() != pubHex {
		t.Fatalf("reopen after import: %v pub=%s", err, ks2.PubHex())
	}
}

// Export must round-trip through ImportBrowserBackup (and thus the map app, which uses the
// same PKCS#8-base64-priv / hex-pub encoding): export a key, re-import it, same pubkey.
func TestExportRoundTrip(t *testing.T) {
	ks, err := Mint(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	privB64, pubHex, _, err := ks.Export()
	if err != nil {
		t.Fatal(err)
	}
	backup := map[string]any{"controllerKey": map[string]string{"priv": privB64, "pub": pubHex}}
	bj, _ := json.Marshal(backup)
	bpath := filepath.Join(t.TempDir(), "backup.json")
	if err := os.WriteFile(bpath, bj, 0o600); err != nil {
		t.Fatal(err)
	}
	ks2, err := ImportBrowserBackup(t.TempDir(), bpath)
	if err != nil {
		t.Fatalf("re-import exported backup: %v", err)
	}
	if ks2.PubHex() != ks.PubHex() {
		t.Fatalf("pub mismatch after export->import: %s vs %s", ks2.PubHex(), ks.PubHex())
	}
}

func TestOpenMissing(t *testing.T) {
	if _, err := Open(t.TempDir()); !os.IsNotExist(err) {
		t.Fatalf("expected ErrNotExist, got %v", err)
	}
}

// The membership allowlist: approve/revoke/IsAllowed, case-insensitive keys, and a
// persistence round-trip (it must survive Open after a Mint+Approve).
func TestAllowlist(t *testing.T) {
	dir := t.TempDir()
	ks, err := Mint(dir)
	if err != nil {
		t.Fatal(err)
	}
	const pub = "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789"
	if ks.IsAllowed(pub) {
		t.Fatal("fresh store should allow nothing")
	}
	if err := ks.Approve(pub, 1700000000); err != nil {
		t.Fatal(err)
	}
	if !ks.IsAllowed(pub) || !ks.IsAllowed(toLowerHex(pub)) { // case-insensitive
		t.Fatal("approved pub should be allowed regardless of case")
	}
	if al := ks.Allowlist(); al[pub] != 1700000000 {
		t.Fatalf("allowlist snapshot wrong: %v", al)
	}
	// Persistence: reopen the same dir and the approval must still be there.
	ks2, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	if !ks2.IsAllowed(pub) {
		t.Fatal("approval did not persist across reopen")
	}
	// Revoke clears it (and persists).
	if err := ks2.Revoke(pub); err != nil {
		t.Fatal(err)
	}
	if ks2.IsAllowed(pub) {
		t.Fatal("revoked pub should not be allowed")
	}
	if ks3, _ := Open(dir); ks3.IsAllowed(pub) {
		t.Fatal("revoke did not persist")
	}
}

func toLowerHex(s string) string {
	b := []byte(s)
	for i, c := range b {
		if c >= 'A' && c <= 'F' {
			b[i] = c + ('a' - 'A')
		}
	}
	return string(b)
}
