#!/usr/bin/env python3
# tcp_bridge.py — expose a USB-attached mesh node on the LAN (docs/tcp-bridge.md §4).
#
# Bridges the node's tunnel (HDLC frames + console text over serial) to any number
# of TCP clients:
#
#   your app ──TCP──► tcp_bridge ──serial──► node ──► LoRa mesh
#
# - Node → clients: every byte fans out to ALL connected clients (frames and
#   console lines alike — each client runs its own HDLC/text demux, §4.2).
# - Clients → node: writes are reassembled into ATOMIC units per client (one
#   complete HDLC frame, or one newline-terminated text line) and written to the
#   serial port under a lock — two chatty clients can never interleave bytes
#   mid-frame (the §0.6 writer-lock rule, enforced centrally).
# - The serial side reconnects forever: a node reboot/replug just causes a gap.
#
# Usage:
#   python3 scripts/tcp_bridge.py [--port /dev/ttyACM0] [--listen 0.0.0.0:7878]
#
# Reticulum users: point AgnosticLoraInterface at the bridge host instead of a
# local serial port, or run Path A from the doc (RNS TCPServerInterface).

import argparse, glob, socket, threading, time
import serial

FLAG = 0x7E

def log(msg):
    print(time.strftime("[%H:%M:%S]"), msg, flush=True)

class Bridge:
    def __init__(self, port, baud, send_tunnel_cmd=True):
        self.port, self.baud = port, baud
        self.send_tunnel_cmd = send_tunnel_cmd
        self.ser = None
        self.wlock = threading.Lock()          # one writer at a time, whole units only
        self.clients = set()                   # sockets
        self.clock = threading.Lock()

    # --- serial side -------------------------------------------------------
    def serial_loop(self):
        while True:
            try:
                self.ser = serial.Serial(self.port, self.baud, timeout=0.2)
                log(f"serial up: {self.port}")
                if self.send_tunnel_cmd:
                    with self.wlock:
                        self.ser.write(b"\ntunnel\n")   # harmless if already in tunnel mode
                while True:
                    data = self.ser.read(512)
                    if data:
                        self.fanout(data)
            except (serial.SerialException, OSError) as e:
                log(f"serial down ({e}); retrying in 2 s")
                self.ser = None
                time.sleep(2)

    def write_unit(self, unit):
        """Write one atomic unit (complete frame or text line) to the node."""
        with self.wlock:
            if self.ser:
                try:
                    self.ser.write(unit)
                except (serial.SerialException, OSError):
                    pass                        # serial_loop will reconnect

    # --- TCP side ----------------------------------------------------------
    def fanout(self, data):
        with self.clock:
            dead = []
            for c in self.clients:
                try:
                    c.sendall(data)
                except OSError:
                    dead.append(c)
            for c in dead:
                self.clients.discard(c)

    def client_loop(self, conn, addr):
        log(f"client connected: {addr[0]}:{addr[1]}")
        with self.clock:
            self.clients.add(conn)
        # Reassemble this client's stream into atomic units before writing to the
        # node: bytes between FLAGs are one HDLC frame; outside frames, bytes up
        # to a newline are one console line.
        buf = bytearray()
        in_frame = False
        try:
            while True:
                data = conn.recv(512)
                if not data:
                    break
                for b in data:
                    if in_frame:
                        buf.append(b)
                        if b == FLAG:                       # closing flag
                            self.write_unit(bytes(buf)); buf.clear(); in_frame = False
                    elif b == FLAG:
                        if buf:                              # flush a dangling text part
                            self.write_unit(bytes(buf)); buf.clear()
                        buf.append(b); in_frame = True
                    else:
                        buf.append(b)
                        if b == 0x0A:                        # newline ends a console line
                            self.write_unit(bytes(buf)); buf.clear()
                if len(buf) > 4096:                          # runaway guard
                    buf.clear(); in_frame = False
        except OSError:
            pass
        finally:
            with self.clock:
                self.clients.discard(conn)
            conn.close()
            log(f"client gone: {addr[0]}:{addr[1]}")

    def serve(self, host, tcp_port):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, tcp_port))
        srv.listen(8)
        log(f"listening on {host}:{tcp_port}")
        threading.Thread(target=self.serial_loop, daemon=True).start()
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=self.client_loop, args=(conn, addr), daemon=True).start()

def main():
    ap = argparse.ArgumentParser(description="TCP bridge to a USB-attached mesh node")
    ap.add_argument("--port", default=None, help="serial port (default: first /dev/ttyACM*)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--listen", default="0.0.0.0:7878", help="host:port to listen on")
    ap.add_argument("--no-tunnel-cmd", action="store_true",
                    help="don't send `tunnel` on connect (node already in tunnel mode)")
    args = ap.parse_args()

    port = args.port or next(iter(sorted(glob.glob("/dev/ttyACM*"))), None)
    if not port:
        raise SystemExit("no serial port found — pass --port")
    host, _, tcp_port = args.listen.rpartition(":")

    Bridge(port, args.baud, not args.no_tunnel_cmd).serve(host or "0.0.0.0", int(tcp_port))

if __name__ == "__main__":
    main()
