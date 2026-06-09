# A Reticulum interface that tunnels RNS packets over the agnostic-LoRa-Net mesh.
#
# Reticulum sees this as a point-to-point link to one peer node; underneath, the
# LoRa backbone routes the opaque RNS packet to that peer (multi-hop, ARQ, the lot).
# This is the "apps ride on top as opaque payload" model from Agent.md §1 — NOT
# RNode emulation (which would bypass our routing).
#
# Wire to a node running tunnel mode over USB serial. Frames are HDLC (matching
# RNS's PipeInterface) carrying [u32 node_id LE][payload]:
#   outbound: [peer_dst][rns_packet]   -> node sends it into the mesh toward peer
#   inbound:  [src][rns_packet]        <- node delivered it from the mesh
#
# Config (in the Reticulum config file):
#   [[Mesh Link]]
#     type = AgnosticLoraInterface
#     interface_enabled = yes
#     port = /dev/ttyACM0
#     peer = 9828F51B          # destination node id (hex) on the mesh
#     speed = 115200
import time, threading, struct
import serial
import RNS
from RNS.Interfaces.Interface import Interface


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
        self.peer    = int(c["peer"], 16)             # mesh node id of the far end
        self.bitrate = 800                            # LoRa-class link (~0.8 kbit/s)
        self.HW_MTU  = 500
        self.IN = True; self.OUT = True
        self.online  = False
        self.serial  = None

        self._connect()

    def _connect(self):
        RNS.log(f"Opening {self.port} for {self}", RNS.LOG_VERBOSE)
        self.serial = serial.Serial(self.port, self.speed, timeout=0.2)
        self.serial.dtr = True; self.serial.rts = True
        time.sleep(0.3)
        self.serial.reset_input_buffer()
        self.serial.write(b"tunnel\n"); self.serial.flush()   # put the node into tunnel mode
        time.sleep(0.3)
        t = threading.Thread(target=self._read_loop, daemon=True); t.start()
        self.online = True
        RNS.log(f"{self} online (peer {self.peer:08X})", RNS.LOG_VERBOSE)

    def process_outgoing(self, data):
        if not self.online:
            return
        loc = struct.pack("<I", self.peer)                          # locator (node id, LE)
        payload = bytes([self.ADDR_LOCATOR, len(loc)]) + loc + data  # [type][len][locator][rns packet]
        self.serial.write(HDLC.frame(payload)); self.serial.flush()
        self.txb += len(data)

    def _read_loop(self):
        in_frame = False; esc = False; buf = bytearray()
        try:
            while True:
                b = self.serial.read(1)
                if len(b) == 0:
                    continue
                byte = b[0]
                if in_frame and byte == HDLC.FLAG:
                    in_frame = False
                    # [addr_type][addr_len][addr][payload]; strip the envelope, hand the
                    # RNS packet up. (We don't need the src locator for RNS, but caching
                    # it here is the free reverse-path binding — see distributed-lookup-plan.)
                    if len(buf) >= 2:
                        addr_type, addr_len = buf[0], buf[1]
                        if addr_type == self.ADDR_LOCATOR and len(buf) >= 2 + addr_len:
                            data = bytes(buf[2 + addr_len:])
                            self.rxb += len(data)
                            self.owner.inbound(data, self)
                elif byte == HDLC.FLAG:
                    in_frame = True; esc = False; buf = bytearray()
                elif in_frame and len(buf) < self.HW_MTU + 8:
                    if byte == HDLC.ESC:
                        esc = True
                    else:
                        buf.append(byte ^ HDLC.ESC_MASK if esc else byte); esc = False
        except Exception as e:
            self.online = False
            RNS.log(f"{self} read error: {e}", RNS.LOG_ERROR)

    def __str__(self):
        return f"AgnosticLoraInterface[{self.name}]"

# Reticulum looks for this name when loading a custom interface:
interface_class = AgnosticLoraInterface
