// Package sign is the Go side of the Tier-1 signed control plane — the byte-exact
// mirror of lib/mesh/control.cpp. The controller (this Go program) is the sole signer;
// nodes only verify. Because monocypher's crypto_ed25519_* is RFC 8032 (SHA-512) and its
// 64-byte secret key is seed‖pub — identical to Go's ed25519.PrivateKey — a message built
// here verifies on-device byte-for-byte. test/test_ctrl_interop proves exactly that.
//
// Wire layout (matches control.cpp):
//
//	unsigned[11] = ver(1) | cmd(1) | target(u32 LE) | arg(i8) | counter(u32 LE)
//	msg[75]      = unsigned[11] ‖ Ed25519_sig[64]  over  "AGN-CTRL-1" ‖ unsigned[11]
package sign

import (
	"crypto/ed25519"
	"errors"
)

const (
	CtrlVer    = 1
	CmdPower   = 1 // CTRL_POWER
	CmdConfirm = 2 // CTRL_CONFIRM
	CmdBlock   = 3 // CTRL_BLOCK
	CmdUnblock = 4 // CTRL_UNBLOCK

	unsignedBytes      = 11                                         // POWER/CONFIRM header
	unsignedBytesBlock = 15                                         // BLOCK/UNBLOCK header (+ victim)
	MsgBytes           = unsignedBytes + ed25519.SignatureSize      // 75
	BlkBytes           = unsignedBytesBlock + ed25519.SignatureSize // 79
)

// domain tag — a control signature can never be confused with any other signed thing.
var domain = []byte("AGN-CTRL-1")

var (
	ErrBadCmd    = errors.New("sign: cmd must be POWER or CONFIRM")
	ErrBadTarget = errors.New("sign: target must be non-zero")
	ErrMalformed = errors.New("sign: malformed control message")
	ErrBadSig    = errors.New("sign: signature check failed")
	ErrReplay    = errors.New("sign: counter not above replay floor")
)

func putU32LE(b []byte, v uint32) {
	b[0], b[1], b[2], b[3] = byte(v), byte(v>>8), byte(v>>16), byte(v>>24)
}

func u32LE(b []byte) uint32 {
	return uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24
}

func unsignedPart(cmd uint8, target uint32, arg int8, counter uint32) []byte {
	b := make([]byte, unsignedBytes)
	b[0] = CtrlVer
	b[1] = cmd
	putU32LE(b[2:], target)
	b[6] = byte(arg)
	putU32LE(b[7:], counter)
	return b
}

// signedView = domain ‖ unsigned — the exact bytes signed (mirrors signed_view()).
func signedView(unsigned []byte) []byte {
	v := make([]byte, 0, len(domain)+len(unsigned))
	v = append(v, domain...)
	v = append(v, unsigned...)
	return v
}

// KeyFromSeed derives the controller keypair from a 32-byte seed — the same derivation
// monocypher's crypto_ed25519_key_pair performs on-device for an identical seed.
func KeyFromSeed(seed []byte) ed25519.PrivateKey {
	return ed25519.NewKeyFromSeed(seed)
}

func unsignedLen(cmd uint8) int {
	if cmd == CmdBlock || cmd == CmdUnblock {
		return unsignedBytesBlock
	}
	return unsignedBytes
}

func knownCmd(cmd uint8) bool {
	return cmd == CmdPower || cmd == CmdConfirm || cmd == CmdBlock || cmd == CmdUnblock
}

// Command is a decoded control command.
type Command struct {
	Cmd     uint8
	Target  uint32 // recipient that applies the command
	Arg     int8   // POWER: dBm. BLOCK: TTL minutes.
	Aux     uint32 // BLOCK/UNBLOCK: victim id (else 0)
	Counter uint32
}

// BuildControl produces a 75-byte signed control command, byte-identical to the
// firmware's ctrl_build for the same key and fields.
func BuildControl(cmd uint8, target uint32, arg int8, counter uint32, priv ed25519.PrivateKey) ([]byte, error) {
	if cmd != CmdPower && cmd != CmdConfirm {
		return nil, ErrBadCmd
	}
	if target == 0 {
		return nil, ErrBadTarget
	}
	u := unsignedPart(cmd, target, arg, counter)
	sig := ed25519.Sign(priv, signedView(u))
	out := make([]byte, 0, MsgBytes)
	out = append(out, u...)
	out = append(out, sig...)
	return out, nil
}

// BuildBlock produces a 79-byte signed BLOCK/UNBLOCK command, byte-identical to the
// firmware's ctrl_build_block. `target` is the recipient that applies it; `victim` is the
// link to drop; `ttlMin` (BLOCK) is the node-enforced auto-expiry (0 = firmware default).
func BuildBlock(cmd uint8, target, victim uint32, ttlMin int8, counter uint32, priv ed25519.PrivateKey) ([]byte, error) {
	if cmd != CmdBlock && cmd != CmdUnblock {
		return nil, ErrBadCmd
	}
	if target == 0 || victim == 0 {
		return nil, ErrBadTarget
	}
	b := make([]byte, unsignedBytesBlock)
	b[0] = CtrlVer
	b[1] = cmd
	putU32LE(b[2:], target)
	b[6] = byte(ttlMin)
	putU32LE(b[7:], victim)
	putU32LE(b[11:], counter)
	sig := ed25519.Sign(priv, signedView(b))
	out := make([]byte, 0, BlkBytes)
	out = append(out, b...)
	out = append(out, sig...)
	return out, nil
}

// VerifyControl mirrors ctrl_verify for controller-side self-checks and tests: signature
// first, then the replay floor (accepts only counter > minCounter).
func VerifyControl(msg []byte, pub ed25519.PublicKey, minCounter uint32) (Command, error) {
	if len(msg) < MsgBytes || msg[0] != CtrlVer {
		return Command{}, ErrMalformed
	}
	cmd := msg[1]
	if !knownCmd(cmd) {
		return Command{}, ErrMalformed
	}
	ulen := unsignedLen(cmd)
	if len(msg) < ulen+ed25519.SignatureSize { // BLOCK carries its longer header
		return Command{}, ErrMalformed
	}
	if !ed25519.Verify(pub, signedView(msg[:ulen]), msg[ulen:ulen+ed25519.SignatureSize]) {
		return Command{}, ErrBadSig
	}
	counter := u32LE(msg[ulen-4:])
	if counter <= minCounter {
		return Command{}, ErrReplay
	}
	c := Command{Cmd: cmd, Target: u32LE(msg[2:]), Arg: int8(msg[6]), Counter: counter}
	if cmd == CmdBlock || cmd == CmdUnblock {
		c.Aux = u32LE(msg[7:])
	}
	return c, nil
}
