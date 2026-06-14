package commander

import (
	"crypto/ed25519"
	"encoding/hex"
	"strings"
	"testing"

	"agnostic-lora-net/controller/internal/sign"
)

func decode(t *testing.T, line string) []byte {
	t.Helper()
	f := strings.Fields(line)
	if len(f) != 2 || f[0] != "ctrlsend" {
		t.Fatalf("not a ctrlsend line: %q", line)
	}
	raw, err := hex.DecodeString(f[1])
	if err != nil {
		t.Fatalf("hex: %v", err)
	}
	return raw
}

func mustID(s string) sign.NodeID {
	id, err := sign.ParseNodeID(s)
	if err != nil {
		panic(err)
	}
	return id
}

var (
	idA = mustID("11223344556677889900aabbccddeeff")
	idB = mustID("aaaa0001bbbb0002cccc0003dddd0004")
)

// Every command must round-trip: commander -> ctrlsend hex -> the same verifier the
// firmware runs (sign.VerifyControl) -> the intended fields.
func TestCommandsRoundTrip(t *testing.T) {
	priv := sign.KeyFromSeed(make([]byte, 32))
	pub := priv.Public().(ed25519.PublicKey)

	pwr, err := Power(idA, 12, 5, priv)
	if err != nil {
		t.Fatal(err)
	}
	c, err := sign.VerifyControl(decode(t, pwr), pub, 4)
	if err != nil {
		t.Fatal(err)
	}
	if c.Cmd != sign.CmdPower || c.Target != idA || c.Arg != 12 || c.Counter != 5 {
		t.Fatalf("power decoded wrong: %+v", c)
	}

	cf, err := Confirm(idA, 12, 6, priv)
	if err != nil {
		t.Fatal(err)
	}
	if c, _ := sign.VerifyControl(decode(t, cf), pub, 5); c.Cmd != sign.CmdConfirm || c.Counter != 6 {
		t.Fatalf("confirm decoded wrong: %+v", c)
	}

	ble, err := Ble(idA, true, 7, priv)
	if err != nil {
		t.Fatal(err)
	}
	if c, _ := sign.VerifyControl(decode(t, ble), pub, 6); c.Cmd != sign.CmdBle || c.Target != idA || c.Arg != 1 {
		t.Fatalf("ble decoded wrong: %+v", c)
	}

	blk, err := Block(idB, idA, 30, 9, priv)
	if err != nil {
		t.Fatal(err)
	}
	c, err = sign.VerifyControl(decode(t, blk), pub, 8)
	if err != nil {
		t.Fatal(err)
	}
	if c.Cmd != sign.CmdBlock || c.Target != idB || c.Aux != idA || c.Arg != 30 || c.Counter != 9 {
		t.Fatalf("block decoded wrong: %+v", c)
	}

	unb, err := Unblock(idB, idA, 10, priv)
	if err != nil {
		t.Fatal(err)
	}
	if c, _ := sign.VerifyControl(decode(t, unb), pub, 9); c.Cmd != sign.CmdUnblock || c.Aux != idA {
		t.Fatalf("unblock decoded wrong: %+v", c)
	}
}
