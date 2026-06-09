# Connecting apps to agnostic-LoRa-Net — the TCP bridge & tunnel protocol

This guide is for **app developers**. It explains how to get an application — a
Reticulum app like Sideband, or your own custom app — sending and receiving data
across an agnostic-LoRa-Net mesh, by way of a **TCP bridge**.

The backbone is *app-agnostic*: it moves opaque, addressed payloads between nodes and
never inspects them. Your app supplies its own format and encryption; the mesh just
delivers bytes from node A to node B (multi-hop, with link-layer retransmission). This
doc gives you exactly what you need to plug in.

---

## 1. The big picture

A node exposes a **tunnel**: a byte pipe (over USB serial *or* BLE) that carries
framed `[destination-node][payload]` messages into the mesh, and hands back
`[source-node][payload]` messages that arrive for it. A **TCP bridge** is a small host
program that connects to a node's tunnel and re-exposes it over TCP, so apps on the
network (including a phone over Wi-Fi) can reach the mesh.

```
   your app  ──TCP (Wi-Fi/LAN)──►  bridge host  ──USB serial or BLE──►  LoRa node ──► mesh ──► …
  (phone/PC)                      (RPi / PC / Termux)                  (RAK4631, XIAO, …)
```

There are **two integration paths**, depending on what your app speaks:

| Your app | Path | Bridge type |
|---|---|---|
| **Reticulum** (Sideband, MeshChat, custom RNS) | A | An RNS instance that routes between a `TCPServerInterface` and the mesh |
| **Anything else** (your own protocol) | B | A transparent TCP ⇄ tunnel byte relay; your app speaks the framing below |

Path A needs **no protocol code in your app** — Reticulum handles everything. Path B
gives you a raw, language-agnostic pipe.

> **Reality check — bandwidth.** This is LoRa: ~0.7–1 kbit/s at the default PHY
> (SF11/BW250). It's built for small, addressed messages (chat, telemetry, control),
> not bulk transfer. A 2 KB payload takes ~50 s. Design accordingly.

---

## 2. The tunnel protocol (used by everything below)

### 2.1 Transports

A node's tunnel runs over either:

- **USB serial** — 115200 8N1. Send the text line `tunnel\n` once to switch the node's
  console into binary tunnel mode. (The node keeps emitting human-readable log lines;
  see framing below — they're harmless.)
- **BLE** — the Nordic UART Service (NUS). Service `6e400001-…`, write char
  `6e400002-…`, notify char `6e400003-…`. Tunnel mode is automatic once connected. BLE
  is **PIN-paired** — see §5.

The framing is identical over both.

### 2.2 Framing: HDLC

Messages are delimited with HDLC (the same framing Reticulum's `PipeInterface` uses):

- Frame boundary byte: `FLAG = 0x7E`
- Escape byte: `ESC = 0x7D`, escape mask `0x20`
- To encode a frame: emit `FLAG`, then each payload byte (replacing `0x7E`→`0x7D 0x5E`
  and `0x7D`→`0x7D 0x5D`), then `FLAG`.
- To decode: sync on `FLAG`; collect until the next `FLAG`; un-escape. **Bytes that
  appear outside a frame are ignored** — this is why the node's log lines (plain ASCII,
  never containing `0x7E`/`0x7D`) don't corrupt the stream.

### 2.3 Frame contents

Inside each HDLC frame is a **typed, length-prefixed address** followed by the opaque
payload:

```
frame body := [ u8 addr_type ][ u8 addr_len ][ addr bytes … ][ payload bytes … ]

  addr_type 0x01 = LOCATOR   node id (the mesh address). addr_len = 4 today; it becomes
                             16 when node ids widen to a pub-key hash (see
                             docs/identity-vs-locator.md §6) — same frame, longer addr.
  addr_type 0x02 = IDENTITY  reserved — send to an app identity and let the node resolve
                             it to a locator. NOT live yet; do not emit.

outbound (host → node):  addr = dst_node_id  → mesh delivers to that node
inbound  (node → host):  addr = src_node_id  ← arrived from that node
```

> **Why typed + length-prefixed:** the address width can grow (4→16 B) without a
> flag-day, and identity-addressed delivery can be added as a *type*, not a redesign.
> Always read `addr_len` rather than assuming 4 bytes.

- **Node ids** are the mesh addresses (4 bytes today). Each node prints its own id in its
  serial banner/heartbeat (`node=9828F51B`) and advertises BLE as `AgnLoRa-<id>`. You
  address a peer by its id; discovering ids is your app's concern (announce, directory,
  config).
- **Payload size:** keep it under ~178 bytes to ride one LoRa frame. The node will
  **automatically fragment & reassemble** larger payloads (up to 8 KB) using its internal
  SAR layer with a CRC + missing-fragment retransmit — transparent to you, just slower.

### 2.4 Reliability

The mesh does **per-hop** ARQ (each LoRa hop is acked + retried). It does **not**
provide end-to-end delivery guarantees for a single raw packet — a packet lost beyond
the hop-retry limit is dropped. So:

- If you use **Reticulum** (Path A), you get RNS's end-to-end reliability for free.
- If you build a **custom** app (Path B), add your own acknowledgment/retry for anything
  that must arrive.

---

## 3. Path A — Reticulum apps (Sideband, etc.)

Here the bridge is a **Reticulum instance** with two interfaces: one to the mesh (the
`AgnosticLoraInterface` in this repo) and a `TCPServerInterface` your app connects to.
RNS routes between them. **Your app needs zero custom code** — it's just another
Reticulum node reachable over TCP.

```
 Sideband ──TCPClientInterface──► bridge RNS ──AgnosticLoraInterface──► node ──► mesh
```

### 3.1 Bridge host setup

1. Install Reticulum: `pip install rns` (Python 3.8+, e.g. on an RPi).
2. Copy [`reticulum/interfaces/AgnosticLoraInterface.py`](../reticulum/interfaces/AgnosticLoraInterface.py)
   into your Reticulum config dir at `~/.reticulum/interfaces/`.
3. Configure `~/.reticulum/config`:

```ini
[reticulum]
  enable_transport = True
  share_instance   = Yes

[interfaces]
  # The link into the LoRa mesh (USB serial to a node in tunnel mode)
  [[Mesh Link]]
    type              = AgnosticLoraInterface
    interface_enabled = yes
    port              = /dev/ttyACM0
    peer              = 9828F51B        # the mesh node id this interface delivers to
    speed             = 115200

  # The TCP door your app connects to
  [[TCP Server]]
    type              = TCPServerInterface
    interface_enabled = yes
    listen_ip         = 0.0.0.0
    listen_port       = 4242
```

4. Run `rnsd` (or any RNS app) on the bridge host. The mesh interface comes online and
   announces propagate.

### 3.2 App setup (Sideband / any RNS app)

Add a **TCP Client** interface pointing at the bridge:

```ini
[[Mesh Bridge]]
  type              = TCPClientInterface
  interface_enabled = yes
  target_host       = 192.168.1.50      # the bridge host's IP
  target_port       = 4242
```

In Sideband this is *Settings → Connectivity → add a TCP interface*. Once connected,
Sideband's announces/LXMF messages route over TCP to the bridge and out across the LoRa
mesh to whatever's on the other side (another bridge+node+app, or any RNS destination
reachable through the mesh).

This is the proven path: [`scripts/rns_demo.py`](../scripts/rns_demo.py) runs two RNS
instances over the mesh (no LAN path) and round-trips a cryptographically-proven echo —
adding the `TCPServerInterface` simply lets external apps in.

> One mesh node ⇄ one `AgnosticLoraInterface` (point-to-point to a configured `peer`).
> For a hub that reaches several mesh nodes, run one interface per peer, or let RNS
> Transport route across them.

---

## 4. Path B — custom apps (any language)

If your app isn't Reticulum, the bridge is a **transparent byte relay**: it copies bytes
between a TCP socket and the node's tunnel, in both directions. Because HDLC frames are
self-delimiting, the relay doesn't even parse them — your app speaks the §2 framing over
the TCP socket exactly as if it were wired to the node's serial port.

```
 your app ──TCP──► [ relay: copy bytes both ways ] ──serial/BLE──► node ──► mesh
```

### 4.1 Reference relay (Python, single client)

```python
#!/usr/bin/env python3
# Transparent TCP <-> node-tunnel relay. App speaks HDLC [type][len][dst][payload] over TCP (§2.3/§4.2).
import socket, threading, serial

NODE_PORT, NODE_BAUD = "/dev/ttyACM0", 115200
LISTEN = ("0.0.0.0", 7878)

ser = serial.Serial(NODE_PORT, NODE_BAUD, timeout=0.1)
ser.dtr = True
ser.write(b"tunnel\n")            # enable USB tunnel mode (harmless if already on / over BLE)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(LISTEN); srv.listen(1)
print("relay listening on", LISTEN)

while True:
    conn, addr = srv.accept(); print("client", addr)
    alive = [True]
    def n2t():
        while alive[0]:
            d = ser.read(256)
            if d:
                try: conn.sendall(d)
                except OSError: break
    t = threading.Thread(target=n2t, daemon=True); t.start()
    try:
        while True:
            d = conn.recv(256)
            if not d: break
            ser.write(d)
    except OSError: pass
    alive[0] = False; conn.close()
```

(Production: handle multiple clients, reconnect the serial, and fan-out/back-pressure
as needed. For BLE instead of serial, swap the `serial` calls for a BLE-NUS client such
as `bleak`, writing to char `6e400002-…` and subscribing to `6e400003-…`.)

### 4.2 App side — encode/decode the framing

Pseudocode your app implements on the TCP socket:

```
LOCATOR = 0x01

SEND(dst_id, payload):                        # payload ≤ ~178 B for one hop; node SARs larger
    loc   = u32_le(dst_id)
    frame = byte(LOCATOR) + byte(len(loc)) + loc + payload
    socket.write( hdlc_encode(frame) )

ON RECEIVE (run an HDLC decoder over the incoming TCP byte stream):
    for frame in hdlc_decode(stream):         # decoder ignores non-frame bytes (node logs)
        if len(frame) < 2: continue
        addr_type, addr_len = frame[0], frame[1]
        if addr_type != LOCATOR or len(frame) < 2 + addr_len: continue
        src_id  = le_uint(frame[2 : 2+addr_len])     # read addr_len bytes — DON'T assume 4
        payload = frame[2+addr_len :]
        deliver(src_id, payload)              # your app's message

hdlc_encode(data):  FLAG + escape(data) + FLAG          # FLAG=0x7E, ESC=0x7D, mask 0x20
hdlc_decode:        sync on FLAG; collect to next FLAG; un-escape; discard out-of-frame bytes
```

That's the whole contract. Anything you put in `payload` is delivered verbatim to the
app on the destination node — bring your own encryption.

---

## 5. Operational notes

- **Node prep.** For USB the relay/interface sends `tunnel\n` to enter tunnel mode. For
  BLE, the node must have BLE enabled and be **paired** first: connect over USB and run
  `ble on`, read the 6-digit PIN (`blepin`, or it's in the heartbeat / `web/manage.html`),
  then pair your phone/host. See the main `README.md` BLE section.
- **Addressing & discovery.** Node ids are fixed per chip (printed at boot / in
  `AgnLoRa-<id>`). How an app learns peer ids is up to it — static config, an announce,
  or a directory. (Reticulum handles this itself via announces.)
- **Multiple nodes / multi-hop.** The mesh routes between nodes for you; the bridge only
  needs a link to *one* node. A payload addressed to a distant node is forwarded
  hop-by-hop automatically.
- **Throughput.** Keep messages small and infrequent. The node fragments larger payloads
  (≤8 KB) transparently but each fragment is ~2 s of airtime.
- **Security.** The backbone does not encrypt payloads — apps must (Reticulum does by
  default). BLE access to a node is PIN-gated; the LoRa side is open by design
  (app-layer crypto is the boundary).

---

## 6. Protocol summary (cheat sheet)

```
Transport:   USB serial 115200 8N1 (send "tunnel\n" once) | BLE NUS 6e400001/2/3 (paired)
Framing:     HDLC   FLAG=0x7E  ESC=0x7D  ESC_MASK=0x20
Frame body:  [u8 addr_type][u8 addr_len][addr LE][payload]   (addr_type 0x01=LOCATOR; 0x02=IDENTITY reserved)
             out → addr = dst node id      in ← addr = src node id   (read addr_len; 4 B now, 16 B later)
Node id:     mesh address (4 B today, hex e.g. 9828F51B); BLE name AgnLoRa-<id>
Payload:     ≤ ~178 B = 1 hop frame; up to 8 KB auto-fragmented (SAR + CRC + retransmit)
Reliability: per-hop ARQ only — add end-to-end ACK yourself, or use Reticulum (Path A)
TCP bridge:  Path A = RNS (TCPServerInterface + AgnosticLoraInterface) | Path B = byte relay
```
