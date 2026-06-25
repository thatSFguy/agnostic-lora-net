# A Reticulum interface that tunnels RNS packets over the agnostic-LoRa-Net mesh.
#
# Reticulum sees this as a point-to-point link to one peer node; underneath, the
# LoRa backbone routes the opaque RNS packet to that peer (multi-hop, ARQ, the lot).
# This is the "apps ride on top as opaque payload" model — NOT
# RNode emulation (which would bypass our routing).
#
# Wire to a node running tunnel mode over USB serial. Tunnel frames are HDLC carrying
# the typed envelope [u8 addr_type][u8 addr_len][addr…][payload]:
#   outbound: [LOCATOR][len][peer_node][rns_packet]  -> node routes it to that node
#   inbound:  [LOCATOR][len][src_node ][rns_packet]  <- node delivered it from the mesh
#
# Addressing the peer (two ways):
#   * STATIC      — config `peer = 9828F51B` pins the far node id (the original mode).
#   * BY IDENTITY — config `identity`/`peer_identity` (opaque hex). The interface
#     `register`s its own identity and `resolve`s the peer's via the node's distributed
#     locator directory, so the far node id is *discovered* (and tracks mobility) rather
#     than hardcoded. This is Phase 4.
#
# Config (in the Reticulum config file):
#   [[Mesh Link]]
#     type = AgnosticLoraInterface
#     interface_enabled = yes
#     port = /dev/ttyACM0
#     identity = A1A1A1A1A1A1A1A1     # this side's opaque id (hex) — registered
#     peer_identity = B2B2B2B2B2B2B2B2 # far side's opaque id (hex) — resolved to a node
#     # peer = 9828F51B               # …or pin the node id directly instead
#     speed = 115200
import time, threading, struct, os
import serial
import RNS
from RNS.Interfaces.Interface import Interface

_AGN_DEBUG = os.environ.get("AGN_IFACE_DEBUG")   # set to enable /tmp/agn_<port>.log tracing


class HDLC:
    FLAG = 0x7E; ESC = 0x7D; ESC_MASK = 0x20
    @staticmethod
    def frame(data):
        out = bytearray([HDLC.FLAG])
        for b in data:
            if b == HDLC.FLAG or b == HDLC.ESC:
                out += bytes([HDLC.ESC, b ^ HDLC.ESC_MASK])
            else:
                out.append(b)
        out.append(HDLC.FLAG)
        return bytes(out)


class AgnosticLoraInterface(Interface):
    DEFAULT_IFAC_SIZE = 8

    # Typed, length-prefixed tunnel envelope (matches the node firmware):
    #   [u8 addr_type][u8 addr_len][addr bytes…][payload…]
    ADDR_LOCATOR  = 0x01      # addr = node-id locator (4 B today, 16 B once the id widens)
    ADDR_IDENTITY = 0x02      # reserved (resolve-and-forward) — not used yet

    def __init__(self, owner, configuration):
        super().__init__()
        c = Interface.get_config_obj(configuration)

        self.owner   = owner
        self.name    = c["name"]
        self.port    = c["port"]
        self.speed   = int(c["speed"]) if "speed" in c else 115200
        # Static node id (optional) or identity-based addressing via the directory.
        # v2: node id is a 16-byte value (32 hex). Store as raw bytes, natural order — the
        # wire locator is these bytes verbatim (NOT a little-endian int; that was the v1 bug).
        self.peer          = bytes.fromhex(c["peer"]) if "peer" in c else None
        self.identity      = c["identity"]      if "identity" in c else None
        self.peer_identity = c["peer_identity"] if "peer_identity" in c else None
        self.bitrate = 800                            # LoRa-class link (~0.8 kbit/s)
        self.HW_MTU  = 500
        self.IN = True; self.OUT = True
        self.online  = False
        self.serial  = None
        self._wlock  = threading.Lock()               # serial is shared (RNS + directory thread)
        self._pending = []                            # outgoing buffered until peer resolves
        self._plock   = threading.Lock()

        self._connect()

    def _connect(self):
        RNS.log(f"Opening {self.port} for {self}", RNS.LOG_VERBOSE)
        self.serial = serial.Serial(self.port, self.speed, timeout=0.2)
        self.serial.dtr = True; self.serial.rts = True
        time.sleep(0.3)
        self.serial.reset_input_buffer()
        self._write(b"tunnel\n")                       # put the node into tunnel mode
        time.sleep(0.3)
        threading.Thread(target=self._read_loop, daemon=True).start()
        self.online = True
        self._dbg(f"connect id={self.identity} peer_id={self.peer_identity} peer={self.peer}")
        if self.identity or self.peer_identity:        # identity-addressed: drive the directory
            threading.Thread(target=self._directory_loop, daemon=True).start()
        who = self.peer.hex().upper() if self.peer is not None else f"identity {self.peer_identity}"
        RNS.log(f"{self} online (peer {who})", RNS.LOG_VERBOSE)

    # All serial writes go through one lock so the directory thread's text commands and
    # RNS's HDLC frames never interleave mid-write on the shared port.
    def _write(self, raw):
        with self._wlock:
            self.serial.write(raw); self.serial.flush()

    def _dbg(self, msg):
        if not _AGN_DEBUG: return
        try:
            with open(f"/tmp/agn_{os.path.basename(self.port)}.log", "a") as f:
                f.write(f"{time.time():.1f} {msg}\n")
        except Exception: pass

    def _cmd(self, line):
        try: self._write((line + "\n").encode()); self._dbg(f"cmd> {line}")
        except Exception: pass

    # Register our identity once, then keep resolving the peer's (retry until found,
    # then a slow refresh so a peer that moves nodes is picked up). `resolve` is a cheap
    # local cache hit on the node once the peer's REGISTER has propagated.
    def _directory_loop(self):
        if self.identity:
            self._cmd(f"register {self.identity}")
        while self.online:
            if self.peer_identity:
                self._cmd(f"resolve {self.peer_identity}")
            time.sleep(5 if self.peer is None else 30)

    def process_outgoing(self, data):
        if not self.online:
            return
        if self.peer is None:
            # Peer not resolved yet — BUFFER (bounded) instead of dropping, so the early
            # RNS announce / path-request isn't lost (losing it = no path established).
            with self._plock:
                if len(self._pending) < 64:
                    self._pending.append(data); self._dbg(f"QUEUE ({len(data)}B) n={len(self._pending)}")
            return
        self._send_frame(data)

    def _send_frame(self, data):
        loc = self.peer                                # 16 raw bytes, natural order (= bytes.fromhex(id))
        payload = bytes([self.ADDR_LOCATOR, len(loc)]) + loc + data  # [type][len=16][locator][rns packet]
        self._write(HDLC.frame(payload))
        self.txb += len(data)

    def _read_loop(self):
        in_frame = False; esc = False; buf = bytearray(); text = bytearray()
        try:
            while True:
                b = self.serial.read(1)
                if len(b) == 0:
                    continue
                byte = b[0]
                if byte == HDLC.FLAG:
                    if in_frame:
                        in_frame = False; self._handle_frame(buf)
                    else:
                        in_frame = True; esc = False; buf = bytearray()
                    text = bytearray()                 # a frame boundary is never part of a line
                    continue
                if in_frame:
                    if len(buf) < self.HW_MTU + 8:
                        if byte == HDLC.ESC: esc = True
                        else: buf.append(byte ^ HDLC.ESC_MASK if esc else byte); esc = False
                    continue
                # out-of-frame: the node's text console (e.g. `loc <id> <node>` answers)
                if byte == 0x0A:
                    self._handle_text(text.decode("latin1", "ignore")); text = bytearray()
                elif byte != 0x0D and len(text) < 200:
                    text.append(byte)
        except Exception as e:
            self.online = False
            RNS.log(f"{self} read error: {e}", RNS.LOG_ERROR)

    def _handle_frame(self, buf):
        # [addr_type][addr_len][addr][payload]; strip the envelope, hand the RNS packet up.
        if len(buf) >= 2:
            addr_type, addr_len = buf[0], buf[1]
            if addr_type == self.ADDR_LOCATOR and len(buf) >= 2 + addr_len:
                data = bytes(buf[2 + addr_len:])
                self.rxb += len(data)
                src = buf[2:2 + addr_len].hex().upper()   # 16-byte id, natural order (not LE int)
                self._rxn = getattr(self, "_rxn", 0) + 1
                if self._rxn <= 8 or self._rxn % 25 == 0:
                    self._dbg(f"INBOUND #{self._rxn} ({len(data)}B) from {src}")
                self.owner.inbound(data, self)

    def _handle_text(self, line):
        # `loc <IDHEX> <NODE-32hex>` — a resolve answer from the node's directory (v2: both
        # full uppercase hex). The node id is parsed as 16 raw bytes, NOT an int.
        parts = line.split()
        if len(parts) == 3 and parts[0] == "loc" and self.peer_identity \
                and parts[1].upper() == self.peer_identity.upper():
            try:
                node = bytes.fromhex(parts[2])
            except ValueError:
                return
            if node != self.peer:
                self.peer = node
                self._dbg(f"RESOLVED {self.peer_identity} -> {node.hex().upper()}")
                RNS.log(f"{self} resolved {self.peer_identity} -> {node.hex().upper()}", RNS.LOG_NOTICE)
                with self._plock:
                    pend, self._pending = self._pending, []
                if pend: self._dbg(f"flush {len(pend)} buffered")
                for d in pend: self._send_frame(d)   # send what we buffered while unresolved
        elif _AGN_DEBUG and parts[:1] == ["loc"]:
            self._dbg(f"loc line (no match): {line!r} want={self.peer_identity}")

    def __str__(self):
        return f"AgnosticLoraInterface[{self.name}]"

# Reticulum looks for this name when loading a custom interface:
interface_class = AgnosticLoraInterface
