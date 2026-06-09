#!/usr/bin/env python3
# One role of a Reticulum echo test. The server announces an echo destination and
# auto-proves probes; the client waits for a path (which can only arrive over the
# LoRa mesh interface), then sends a probe packet and reports whether it was
# delivered+proven — i.e. a packet that round-tripped through agnostic-LoRa-Net.
import RNS, sys, time, argparse

APP = "agnlora"; ASPECT = "echo"

def server(configdir):
    RNS.Reticulum(configdir)
    ident = RNS.Identity()
    dest = RNS.Destination(ident, RNS.Destination.IN, RNS.Destination.SINGLE, APP, ASPECT)
    dest.set_proof_strategy(RNS.Destination.PROVE_ALL)
    dest.set_packet_callback(lambda data, packet: RNS.log("echo: probe received, proving"))
    print("DESTHASH " + dest.hash.hex(), flush=True)
    # A few quick announces to establish the path, then back off to reduce channel
    # contention while the client probes.
    for i in range(4):
        dest.announce(); RNS.log("announce sent"); time.sleep(12)
    while True:
        dest.announce(); RNS.log("announce sent"); time.sleep(45)

def client(configdir, desthash_hex):
    RNS.Reticulum(configdir)
    dh = bytes.fromhex(desthash_hex)
    print("waiting for a path to the server (over the mesh)...", flush=True)
    t0 = time.time()
    while not RNS.Transport.has_path(dh):
        if time.time() - t0 > 180:
            print("RESULT no-path", flush=True); sys.exit(1)
        RNS.Transport.request_path(dh); time.sleep(3)
    print(f"path established in {time.time()-t0:.0f}s", flush=True)

    server_identity = RNS.Identity.recall(dh)
    dest = RNS.Destination(server_identity, RNS.Destination.OUT, RNS.Destination.SINGLE, APP, ASPECT)
    # A bare RNS packet isn't auto-retried; over a lossy half-duplex LoRa channel a
    # single probe may not survive, so retry until one round-trips.
    for attempt in range(1, 9):
        state = {"ok": False}
        receipt = RNS.Packet(dest, ("probe-%d" % attempt).encode()).send()
        receipt.set_delivery_callback(lambda r: state.__setitem__("ok", True))
        print(f"probe attempt {attempt} ...", flush=True)
        t0 = time.time()
        while not state["ok"] and time.time() - t0 < 25:
            time.sleep(0.3)
        if state["ok"]:
            print(f"RESULT delivered (attempt {attempt}, {time.time()-t0:.0f}s)", flush=True)
            sys.exit(0)
    print("RESULT timeout", flush=True)
    sys.exit(2)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--role", required=True, choices=["server", "client"])
    ap.add_argument("--config", required=True)
    ap.add_argument("--dest")
    a = ap.parse_args()
    if a.role == "server": server(a.config)
    else: client(a.config, a.dest)
