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

func TestOpenMissing(t *testing.T) {
	if _, err := Open(t.TempDir()); !os.IsNotExist(err) {
		t.Fatalf("expected ErrNotExist, got %v", err)
	}
}
