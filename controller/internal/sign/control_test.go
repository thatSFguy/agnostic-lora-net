package sign

import (
	"bytes"
	"crypto/ed25519"
	"encoding/hex"
	"testing"
)

// interopSeed matches test/test_control + test/test_ctrl_interop on-device:
// seed[i] = i*7+1. Same seed -> same keypair -> (RFC 8032) same signatures.
func interopSeed() []byte {
	s := make([]byte, 32)
	for i := range s {
		s[i] = byte(i*7 + 1)
	}
	return s
}

func pubOf(priv ed25519.PrivateKey) ed25519.PublicKey {
	return priv.Public().(ed25519.PublicKey)
}

// v2 test ids: full 16-byte (32-hex) self-certifying NodeIds.
var (
	idTarget = mustID("9828f51b1122334455667788990011aa")
	idVictim = mustID("1fae0dbdfeedfacecafebabe00ff0102")
)

func mustID(s string) NodeID {
	id, err := ParseNodeID(s)
	if err != nil {
		panic(err)
	}
	return id
}

func TestRoundTrip(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	msg, err := BuildControl(CmdPower, idTarget, 10, 42, priv)
	if err != nil {
		t.Fatal(err)
	}
	if len(msg) != MsgBytes {
		t.Fatalf("len=%d want %d", len(msg), MsgBytes)
	}
	c, err := VerifyControl(msg, pubOf(priv), 41)
	if err != nil {
		t.Fatal(err)
	}
	if c.Cmd != CmdPower || c.Target != idTarget || c.Arg != 10 || c.Counter != 42 {
		t.Fatalf("decoded wrong: %+v", c)
	}
}

func TestReplayRejected(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	pub := pubOf(priv)
	msg, _ := BuildControl(CmdPower, idTarget, 22, 100, priv)
	if _, err := VerifyControl(msg, pub, 100); err != ErrReplay { // floor == counter
		t.Fatalf("floor==counter: got %v want ErrReplay", err)
	}
	if _, err := VerifyControl(msg, pub, 500); err != ErrReplay { // floor > counter
		t.Fatalf("floor>counter: got %v want ErrReplay", err)
	}
	if _, err := VerifyControl(msg, pub, 99); err != nil { // floor < counter -> ok
		t.Fatalf("floor<counter: got %v want nil", err)
	}
}

func TestTamperRejected(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	msg, _ := BuildControl(CmdPower, idTarget, -3, 7, priv)
	msg[2] ^= 0x01 // flip a target byte (signed region) -> signature must fail
	if _, err := VerifyControl(msg, pubOf(priv), 0); err != ErrBadSig {
		t.Fatalf("tamper: got %v want ErrBadSig", err)
	}
}

// The 23-byte v2 unsigned header is pure layout (no crypto) — assert it exactly so a wire
// regression is caught here, not just on-device. ver|cmd|target(16)|arg|counter(LE).
func TestHeaderLayout(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	msg, _ := BuildControl(CmdPower, idTarget, 10, 42, priv)
	if msg[0] != CtrlVer || msg[1] != CmdPower {
		t.Fatalf("ver/cmd = %02x %02x", msg[0], msg[1])
	}
	if !bytes.Equal(msg[2:2+IDBytes], idTarget[:]) {
		t.Fatalf("target bytes = %s", hex.EncodeToString(msg[2:2+IDBytes]))
	}
	if int8(msg[2+IDBytes]) != 10 {
		t.Fatalf("arg = %d", int8(msg[2+IDBytes]))
	}
	if got := hex.EncodeToString(msg[3+IDBytes : unsignedBytes]); got != "2a000000" { // LE(42)
		t.Fatalf("counter LE = %s want 2a000000", got)
	}
}

func TestBlockRoundTrip(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	pub := pubOf(priv)
	// recipient (target) blocks victim (aux), TTL 30 min, counter 7.
	msg, err := BuildBlock(CmdBlock, idTarget, idVictim, 30, 7, priv)
	if err != nil {
		t.Fatal(err)
	}
	if len(msg) != BlkBytes {
		t.Fatalf("len=%d want %d", len(msg), BlkBytes)
	}
	c, err := VerifyControl(msg, pub, 0)
	if err != nil {
		t.Fatal(err)
	}
	if c.Cmd != CmdBlock || c.Target != idTarget || c.Aux != idVictim || c.Arg != 30 || c.Counter != 7 {
		t.Fatalf("decoded wrong: %+v", c)
	}
	// Tampering a victim byte must break the signature.
	msg[3+IDBytes] ^= 0x01
	if _, err := VerifyControl(msg, pub, 0); err != ErrBadSig {
		t.Fatalf("tampered victim: got %v want ErrBadSig", err)
	}
}

func TestBleRoundTrip(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	pub := pubOf(priv)
	for _, tc := range []struct {
		on  bool
		arg int8
	}{{true, 1}, {false, 0}} {
		msg, err := BuildBle(idTarget, tc.on, 9, priv)
		if err != nil {
			t.Fatal(err)
		}
		if len(msg) != MsgBytes { // BLE uses the short POWER/CONFIRM layout
			t.Fatalf("on=%v len=%d want %d", tc.on, len(msg), MsgBytes)
		}
		c, err := VerifyControl(msg, pub, 8)
		if err != nil {
			t.Fatal(err)
		}
		if c.Cmd != CmdBle || c.Target != idTarget || c.Arg != tc.arg || c.Counter != 9 {
			t.Fatalf("on=%v decoded wrong: %+v", tc.on, c)
		}
	}
	if _, err := BuildBle(NodeID{}, true, 1, priv); err != ErrBadTarget {
		t.Fatalf("zero target: got %v want ErrBadTarget", err)
	}
}

func TestRetuneRoundTrip(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	pub := pubOf(priv)
	cfg := RetuneCfg{FreqHz: 906625000, BwHz: 250000, SF: 9, CR: 5, Sync: 0x4D, Preamble: 16}
	msg, err := BuildRetune(idTarget, cfg, 11, priv)
	if err != nil {
		t.Fatal(err)
	}
	if len(msg) != RtnBytes {
		t.Fatalf("len=%d want %d", len(msg), RtnBytes)
	}
	c, err := VerifyControl(msg, pub, 10)
	if err != nil {
		t.Fatal(err)
	}
	if c.Cmd != CmdRetune || c.Target != idTarget || c.Counter != 11 {
		t.Fatalf("retune decoded wrong: %+v", c)
	}
	if got := DecodeRetuneCfg(c.Cfg); got != cfg {
		t.Fatalf("PHY blob round-trip: %+v want %+v", got, cfg)
	}
	// Tampering a cfg byte must break the signature.
	msg[2+IDBytes] ^= 0x01
	if _, err := VerifyControl(msg, pub, 10); err != ErrBadSig {
		t.Fatalf("tampered cfg: got %v want ErrBadSig", err)
	}
}

func TestParseNodeID(t *testing.T) {
	if _, err := ParseNodeID("9828f51b"); err == nil { // 8 hex = v1 width, rejected as a 16-byte id
		t.Fatal("8-hex id should fail ParseNodeID")
	}
	id, err := ParseNodeID("0X9828F51B1122334455667788990011AA") // 0x + uppercase tolerated
	if err != nil || id != idTarget {
		t.Fatalf("parse upper/0x: id=%v err=%v", id, err)
	}
	if got := idTarget.Hex(); got != "9828f51b1122334455667788990011aa" {
		t.Fatalf("Hex()=%s", got)
	}
}

// TestEmitVector prints the v2 POWER + BLOCK vectors (message + pubkey) to paste into the
// on-device interop test. Run: go test ./internal/sign -run EmitVector -v
func TestEmitVector(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	pw, _ := BuildControl(CmdPower, idTarget, 10, 42, priv)
	blk, _ := BuildBlock(CmdBlock, idTarget, idVictim, 30, 7, priv)
	t.Logf("GO_MSG  (%d) = %s", len(pw), hex.EncodeToString(pw))
	t.Logf("GO_BLK  (%d) = %s", len(blk), hex.EncodeToString(blk))
	t.Logf("GO_PUB  (%d) = %s", len(pubOf(priv)), hex.EncodeToString(pubOf(priv)))
}
