# Identity vs. locator — where addresses live in agnostic-LoRa-Net

A design boundary that's load-bearing for both the firmware and any app riding on the
mesh (e.g. a Reticulum mobile app). Short version: **the mesh routes on *locators*
(node ids); apps address on *identities* (e.g. RNS destination hashes); the two stay
separate.** This note says why, and folds the "node id = 16-byte node-pubkey hash"
decision into the §5 plan.

---

## 1. Two different questions: *where* vs. *who*

| | Locator | Identity |
|---|---|---|
| Answers | *where* — which physical node to hand the packet to | *who* — which app endpoint/user |
| Owned by | the **mesh** (our backbone) | the **app** (RNS, or any app) |
| Value | a **node id** | e.g. an **RNS destination hash** (16 B / 32 hex) |
| Routed by | our per-direction link-quality DV | RNS Transport (announces, hop counts), above the wire |
| Stability | tied to a physical node (mostly fixed) | follows the *user/app*, moves between nodes |

This is the same split the internet makes between an **IP address** (locator — where on
the network) and **DNS names / keys** (identity — who). Keeping them separate is what
makes mobility, multi-homing, and scaling tractable; fusing them is what makes routing
hard.

---

## 2. The decision

**(A) Node ids stay locators, and become a 16-byte hash of the *node's own* public key.**
- 128-bit, **RNS-shaped format** (same width as an RNS hash) but a **distinct
  namespace** — it identifies a *node*, not an app/user. Self-certifying (a node can
  prove it owns its id via signature) and collision-free in practice (2¹²⁸).
- This is the long-term form of the id; today it's a 32-bit `FICR.DEVICEID` XOR-fold
  placeholder (see [[node-id-identity]]). Widening + signing lands with the signed
  control plane (the Tier-1 identity work), not before.

**(B) The mesh does NOT route on RNS/app identity hashes.** A node never "inherits" the
attached phone's RNS hash to route on it. The app's RNS hash is carried **end-to-end as
opaque payload**; the mesh only needs a *locator* to deliver it to.

**(C) Apps address by their own identity end-to-end.** A Reticulum app uses RNS
destination hashes for contacts/conversations; RNS Transport handles discovery + mobility
+ crypto. Node ids never appear in the app UI. The single per-app config — the **uplink
locator** (which node is my entry point) — is auto-fillable from the `AgnLoRa-<id>` BLE
name. See [`tcp-bridge.md`](tcp-bridge.md) / [`remote-config.md`](remote-config.md).

---

## 3. Why not fuse them (route the mesh on the phone's RNS hash)?

It's tempting — "one address everywhere, no node-id field" — but it breaks the design:

1. **It makes the backbone RNS-specific.** The core premise is an *app-agnostic*
   transport that moves opaque addressed packets like IP moves IP. If mesh routing keys
   on RNS destination hashes, the backbone *is* Reticulum, and a non-RNS app — or a
   second RNS aspect on the same phone — has no address.
2. **A phone is not one hash.** An RNS identity has *many* destination hashes (one per app
   aspect). "Inherit the phone's hash" has no single answer, and it breaks the
   1 node : 1 address assumption the DV relies on.
3. **Mobility churns the routing core.** If node L adopts user U's hash and U walks to
   node M, the DV must withdraw/re-announce that hash and re-converge every time someone
   moves. That turns a stable-topology locator network into a mobile-identity router —
   the hard problem IP deliberately avoids.
4. **It reimplements RNS Transport on an MCU.** RNS already routes by destination hash
   (announces propagate hashes with hop counts; transport nodes learn next-hop per hash;
   mobility handled). Duplicating that in our DV is redundant — on an 8-bit-class budget.
5. **It dissolves the seam our novelty lives in.** Our value-add is **per-direction
   link-quality** routing *between physical nodes*. That optimization belongs at the
   locator layer (which RF path between nodes); identity routing rides on top. Fuse them
   and there's no clean home for the RF-quality logic.

---

## 4. How the layers compose (the intended stack)

```
app identity      RNS destination hash (16 B)         "who"  — never seen by the mesh
   │  (opaque RNS packet as payload; RNS Transport routes by hash, above the wire)
mesh locator      node id (16 B node-pubkey hash)     "where" — our per-direction DV
   │  (link-local 1-byte aliases per hop; RF-quality routing between nodes)
radio             SX1262 PHY
```

- The app hands the mesh an **opaque payload** + a **destination locator** (one node id).
- The mesh delivers it (multi-hop, ARQ, per-direction paths) to that node.
- At that node, the app/RNS layer takes over and routes by **identity** as far as needed.
- Per-hop addressing uses **1-byte link-local aliases**, so widening the end-to-end id to
  16 B costs airtime only on the network header + announces, not on every hop. (Tension
  with §5's "keep the header low-double-digit" goal is real and accepted: the id width is
  the price of self-certifying, collision-free identity.)

---

## 5. Consequences for the app dev

- Use **RNS hashes** for everything the user sees (contacts, conversations). The node id
  is plumbing.
- Configure **one uplink locator** (the node id of your RNS entry/gateway), defaulted
  from the connected node's `AgnLoRa-<id>` BLE name, overridable by type/scan. Don't
  hardcode it; don't surface it as identity.
- Don't treat a node id as proof of *who* a peer is — that's the identity layer's job
  (RNS gives it cryptographically today; node-level signed identity arrives with §5).

---

## 6. Status

- **Now:** mesh routes on 32-bit FICR-fold node ids (locators); apps address by RNS hash
  as opaque payload; one uplink-locator field. Layers already separate.
- **§5 / Tier-1 identity work:** node id → 16-byte node-pubkey hash, self-certifying, with
  a signed control plane. RNS-shaped format, separate namespace. No app rewrite — the
  uplink field just carries a longer value.
