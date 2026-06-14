// Package commander turns a control intent + a counter + the signing key into the exact
// console line the tethered node expects: "ctrlsend <hex>". The node decodes the hex,
// checks the target, and floods it into the mesh as a signed PKT_CONTROL (see the
// firmware `ctrlsend` handler). These are pure functions — the caller owns the keystore
// (for Next()/Priv()) and the transport (Source.Send).
package commander

import (
	"crypto/ed25519"
	"encoding/hex"

	"agnostic-lora-net/controller/internal/sign"
)

func line(msg []byte) string { return "ctrlsend " + hex.EncodeToString(msg) }

// Power lowers/raises the target node's TX power to dbm. Decreases auto-revert on-device
// in 60 s unless a Confirm follows.
func Power(target sign.NodeID, dbm int8, counter uint32, priv ed25519.PrivateKey) (string, error) {
	m, err := sign.BuildControl(sign.CmdPower, target, dbm, counter, priv)
	if err != nil {
		return "", err
	}
	return line(m), nil
}

// Confirm disarms the revert for a provisional power change of `dbm` on the target.
func Confirm(target sign.NodeID, dbm int8, counter uint32, priv ed25519.PrivateKey) (string, error) {
	m, err := sign.BuildControl(sign.CmdConfirm, target, dbm, counter, priv)
	if err != nil {
		return "", err
	}
	return line(m), nil
}

// Block tells `recipient` to drop its link to `victim` for ttlMin minutes (0 = node
// default). The node auto-expires the block unless renewed.
func Block(recipient, victim sign.NodeID, ttlMin int8, counter uint32, priv ed25519.PrivateKey) (string, error) {
	m, err := sign.BuildBlock(sign.CmdBlock, recipient, victim, ttlMin, counter, priv)
	if err != nil {
		return "", err
	}
	return line(m), nil
}

// Ble enables (on=true) or disables BLE/BT advertising on the target node via a signed
// CTRL_BLE command. Remote nodes need firmware CTRL_BLE support to honor it; for the
// tethered gateway the server short-circuits to a direct `ble on/off` console line instead.
func Ble(target sign.NodeID, on bool, counter uint32, priv ed25519.PrivateKey) (string, error) {
	m, err := sign.BuildBle(target, on, counter, priv)
	if err != nil {
		return "", err
	}
	return line(m), nil
}

// Unblock clears a block of `victim` on `recipient`.
func Unblock(recipient, victim sign.NodeID, counter uint32, priv ed25519.PrivateKey) (string, error) {
	m, err := sign.BuildBlock(sign.CmdUnblock, recipient, victim, 0, counter, priv)
	if err != nil {
		return "", err
	}
	return line(m), nil
}
