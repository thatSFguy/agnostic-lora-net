// Package sign is the Go side of the Tier-1 signed control plane — the byte-exact
// mirror of lib/mesh/control.cpp. The controller (this Go program) is the sole signer;
// nodes only verify. Because monocypher's crypto_ed25519_* is RFC 8032 (SHA-512) and its
// 64-byte secret key is seed‖pub — identical to Go's ed25519.PrivateKey — a message built
// here verifies on-device byte-for-byte. test/test_ctrl_interop proves exactly that.
//
// v2 (CtrlVer=2): node ids are the 16-byte self-certifying NodeId (= blake2b(pubkey)[0:16]),
// carried raw (no endianness, matching nid_write). This is a hard wire break from v1's
// 4-byte ids — a v1 controller cannot talk to v2 nodes.
//
// Wire layout (matches control.cpp):
//
//	POWER/CONFIRM/BLE unsigned[23] = ver(1) | cmd(1) | target(16) | arg(i8) | counter(u32 LE)
//	BLOCK/UNBLOCK     unsigned[39] = ver(1) | cmd(1) | target(16) | ttl(i8) | victim(16) | counter(u32 LE)
//	msg = unsigned ‖ Ed25519_sig[64]  over  "AGN-CTRL-1" ‖ unsigned
package sign

import (
	"crypto/ed25519"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
)

const (
	CtrlVer    = 2
	CmdPower   = 1 // CTRL_POWER
	CmdConfirm = 2 // CTRL_CONFIRM
	CmdBlock   = 3 // CTRL_BLOCK
	CmdUnblock = 4 // CTRL_UNBLOCK
	CmdBle     = 5 // CTRL_BLE — arg: 1 = enable BLE/BT advertising, 0 = disable

	IDBytes            = 16                                         // a NodeId on the wire
	unsignedBytes      = 1 + 1 + IDBytes + 1 + 4                    // 23: ver|cmd|target|arg|counter
	unsignedBytesBlock = 1 + 1 + IDBytes + 1 + IDBytes + 4          // 39: + victim before counter
	MsgBytes           = unsignedBytes + ed25519.SignatureSize      // 87
	BlkBytes           = unsignedBytesBlock + ed25519.SignatureSize // 103
)

// NodeID is the 16-byte self-certifying node id, carried raw on the wire (nid_write =
// memcpy, no endianness). Its hex form is 32 lowercase chars, MSB-first — the same string
// the firmware's nid_hex prints and the controller normalises (upper-cased) elsewhere.
type NodeID [IDBytes]byte

// IsZero reports the all-zero id (the wire "none" sentinel rejected by builders).
func (n NodeID) IsZero() bool {
	for _, b := range n {
		if b != 0 {
			return false
		}
	}
	return true
}

// Hex returns the 32-char lowercase hex form (matches firmware nid_hex).
func (n NodeID) Hex() string { return hex.EncodeToString(n[:]) }

// ParseNodeID decodes a 32-hex-char id (optionally 0x-prefixed, any case) into a NodeID.
func ParseNodeID(s string) (NodeID, error) {
	s = strings.TrimPrefix(s, "0x")
	s = strings.TrimPrefix(s, "0X")
	b, err := hex.DecodeString(s)
	if err != nil {
		return NodeID{}, fmt.Errorf("sign: bad node id %q: %w", s, err)
	}
	if len(b) != IDBytes {
		return NodeID{}, fmt.Errorf("sign: node id must be %d hex chars, got %d", IDBytes*2, len(s))
	}
	var id NodeID
	copy(id[:], b)
	return id, nil
}

// domain tag — a control signature can never be confused with any other signed thing.
var domain = []byte("AGN-CTRL-1")

var (
	ErrBadCmd    = errors.New("sign: cmd must be POWER, CONFIRM or BLE")
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

// unsignedPart builds the POWER/CONFIRM/BLE 23-byte header (target|arg|counter).
func unsignedPart(cmd uint8, target NodeID, arg int8, counter uint32) []byte {
	b := make([]byte, unsignedBytes)
	b[0] = CtrlVer
	b[1] = cmd
	copy(b[2:2+IDBytes], target[:])
	b[2+IDBytes] = byte(arg)
	putU32LE(b[3+IDBytes:], counter)
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
	return cmd == CmdPower || cmd == CmdConfirm || cmd == CmdBlock || cmd == CmdUnblock || cmd == CmdBle
}

// Command is a decoded control command.
type Command struct {
	Cmd     uint8
	Target  NodeID // recipient that applies the command
	Arg     int8   // POWER: dBm. BLOCK: TTL minutes. BLE: 1/0.
	Aux     NodeID // BLOCK/UNBLOCK: victim id (else zero)
	Counter uint32
}

// BuildControl produces an 87-byte signed POWER/CONFIRM command, byte-identical to the
// firmware's ctrl_build for the same key and fields.
func BuildControl(cmd uint8, target NodeID, arg int8, counter uint32, priv ed25519.PrivateKey) ([]byte, error) {
	if cmd != CmdPower && cmd != CmdConfirm {
		return nil, ErrBadCmd
	}
	if target.IsZero() {
		return nil, ErrBadTarget
	}
	u := unsignedPart(cmd, target, arg, counter)
	sig := ed25519.Sign(priv, signedView(u))
	return append(u, sig...), nil
}

// BuildBle enables (on=true) or disables BLE/BT advertising on the target node. Same
// 23-byte POWER-style header (CTRL_BLE accepted by ctrl_build on-device), arg = 1/0.
func BuildBle(target NodeID, on bool, counter uint32, priv ed25519.PrivateKey) ([]byte, error) {
	if target.IsZero() {
		return nil, ErrBadTarget
	}
	var arg int8
	if on {
		arg = 1
	}
	u := unsignedPart(CmdBle, target, arg, counter)
	sig := ed25519.Sign(priv, signedView(u))
	return append(u, sig...), nil
}

// BuildBlock produces a 103-byte signed BLOCK/UNBLOCK command, byte-identical to the
// firmware's ctrl_build_block. `target` is the recipient that applies it; `victim` is the
// link to drop; `ttlMin` (BLOCK) is the node-enforced auto-expiry (0 = firmware default).
func BuildBlock(cmd uint8, target, victim NodeID, ttlMin int8, counter uint32, priv ed25519.PrivateKey) ([]byte, error) {
	if cmd != CmdBlock && cmd != CmdUnblock {
		return nil, ErrBadCmd
	}
	if target.IsZero() || victim.IsZero() {
		return nil, ErrBadTarget
	}
	b := make([]byte, unsignedBytesBlock)
	b[0] = CtrlVer
	b[1] = cmd
	copy(b[2:2+IDBytes], target[:])
	b[2+IDBytes] = byte(ttlMin)
	copy(b[3+IDBytes:3+2*IDBytes], victim[:])
	putU32LE(b[3+2*IDBytes:], counter)
	sig := ed25519.Sign(priv, signedView(b))
	return append(b, sig...), nil
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
	c := Command{Cmd: cmd, Arg: int8(msg[2+IDBytes]), Counter: counter}
	copy(c.Target[:], msg[2:2+IDBytes])
	if cmd == CmdBlock || cmd == CmdUnblock {
		copy(c.Aux[:], msg[3+IDBytes:3+2*IDBytes])
	}
	return c, nil
}
