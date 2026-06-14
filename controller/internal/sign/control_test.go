package sign

import (
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

func TestRoundTrip(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	msg, err := BuildControl(CmdPower, 0x9828F51B, 10, 42, priv)
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
	if c.Cmd != CmdPower || c.Target != 0x9828F51B || c.Arg != 10 || c.Counter != 42 {
		t.Fatalf("decoded wrong: %+v", c)
	}
}

func TestReplayRejected(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	pub := pubOf(priv)
	msg, _ := BuildControl(CmdPower, 0xD97EEC3A, 22, 100, priv)
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
	msg, _ := BuildControl(CmdPower, 0x11223344, -3, 7, priv)
	msg[6] ^= 0x01 // flip the arg byte -> signature must fail
	if _, err := VerifyControl(msg, pubOf(priv), 0); err != ErrBadSig {
		t.Fatalf("tamper: got %v want ErrBadSig", err)
	}
}

// The 11-byte unsigned header is pure layout (no crypto) — assert it exactly so a wire
// regression is caught here, not just on-device. LE(0x9828F51B)=1b f5 28 98; arg 10=0a;
// LE(42)=2a 00 00 00.
func TestHeaderLayout(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	msg, _ := BuildControl(CmdPower, 0x9828F51B, 10, 42, priv)
	const wantHdr = "01011bf528980a2a000000"
	if got := hex.EncodeToString(msg[:11]); got != wantHdr {
		t.Fatalf("header=%s want %s", got, wantHdr)
	}
}

func TestBlockRoundTrip(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	pub := pubOf(priv)
	// recipient (target) blocks victim (aux), TTL 30 min, counter 7.
	msg, err := BuildBlock(CmdBlock, 0x9828F51B, 0x1FAE0DBD, 30, 7, priv)
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
	if c.Cmd != CmdBlock || c.Target != 0x9828F51B || c.Aux != 0x1FAE0DBD || c.Arg != 30 || c.Counter != 7 {
		t.Fatalf("decoded wrong: %+v", c)
	}
	// Tampering the victim id must break the signature.
	msg[7] ^= 0x01
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
		msg, err := BuildBle(0x7D9BB406, tc.on, 9, priv)
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
		if c.Cmd != CmdBle || c.Target != 0x7D9BB406 || c.Arg != tc.arg || c.Counter != 9 {
			t.Fatalf("on=%v decoded wrong: %+v", tc.on, c)
		}
	}
	if _, err := BuildBle(0, true, 1, priv); err != ErrBadTarget {
		t.Fatalf("zero target: got %v want ErrBadTarget", err)
	}
}

// TestEmitVector prints the POWER + BLOCK vectors (message + pubkey) to paste into the
// on-device interop test. Run: go test ./internal/sign -run EmitVector -v
func TestEmitVector(t *testing.T) {
	priv := KeyFromSeed(interopSeed())
	pw, _ := BuildControl(CmdPower, 0x9828F51B, 10, 42, priv)
	blk, _ := BuildBlock(CmdBlock, 0x9828F51B, 0x1FAE0DBD, 30, 7, priv)
	t.Logf("GO_MSG  (%d) = %s", len(pw), hex.EncodeToString(pw))
	t.Logf("GO_BLK  (%d) = %s", len(blk), hex.EncodeToString(blk))
	t.Logf("GO_PUB  (%d) = %s", len(pubOf(priv)), hex.EncodeToString(pubOf(priv)))
}
