#!/usr/bin/env python3
# Validate RNS ANNOUNCE PROPAGATION over agnostic-LoRa-Net — the thing a real app
# (Sideband-style) relies on to discover peers, and the exact thing the mobile app is
# struggling with ("announcements not reaching the other app").
#
# Unlike rns_demo.py (which uses Transport.request_path — the client actively pulls a
# path because it already knows the hash), THIS test is purely PASSIVE on the receive
# side: instance B announces a destination, and instance A must have its
# `received_announce()` handler fire — no request_path, no prior knowledge. That handler
# firing is how an app populates contacts. If it works here, the mesh+firmware+interface
# baseline propagates announces correctly and any app-side failure is app-side.
#
# Two roles, orchestrated on this one host over the 2 RAKs (no LAN path).
import RNS, sys, time, os, glob, re, subprocess, shutil, serial

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IFACE_SRC = os.path.join(ROOT, "reticulum", "interfaces", "AgnosticLoraInterface.py")
PYTHON = os.path.join(ROOT, ".venv-rns", "bin", "python")
APP, ASPECT = "agnlora", "disco"

def vid(t):
    p = os.path.realpath(f'/sys/class/tty/{os.path.basename(t)}/device')
    while p and p != '/' and not os.path.exists(f'{p}/idVendor'): p = os.path.dirname(p)
    try: return open(f'{p}/idVendor').read().strip()
    except: return ''

def node_id(port):
    s = serial.Serial(port, 115200, timeout=0.2); s.dtr = True; time.sleep(0.4)
    end = time.time() + 6
    while time.time() < end:
        m = re.search(rb'node=([0-9A-Fa-f]{8})', s.readline() or b'')
        if m: s.close(); return m.group(1).decode().upper()
    s.close(); return None

def write_config(cfgdir, port, identity, peer_identity):
    os.makedirs(os.path.join(cfgdir, "interfaces"), exist_ok=True)
    shutil.copy(IFACE_SRC, os.path.join(cfgdir, "interfaces", "AgnosticLoraInterface.py"))
    with open(os.path.join(cfgdir, "config"), "w") as f:
        f.write(f"""[reticulum]
  enable_transport = True
  share_instance = No
  panic_on_interface_error = No
[logging]
  loglevel = 3
[interfaces]
  [[Mesh Link]]
    type = AgnosticLoraInterface
    interface_enabled = yes
    port = {port}
    identity = {identity}
    peer_identity = {peer_identity}
    speed = 115200
""")

# --- roles -----------------------------------------------------------------------
def announcer(configdir):
    RNS.Reticulum(configdir)
    ident = RNS.Identity()
    dest = RNS.Destination(ident, RNS.Destination.IN, RNS.Destination.SINGLE, APP, ASPECT)
    print("DESTHASH " + dest.hash.hex(), flush=True)
    while True:
        dest.announce(); RNS.log("announce sent"); time.sleep(12)

def listener(configdir):
    RNS.Reticulum(configdir)
    class Handler:
        aspect_filter = f"{APP}.{ASPECT}"
        def received_announce(self, destination_hash, announced_identity, app_data):
            print("ANNOUNCE-RECEIVED " + destination_hash.hex(), flush=True)
    RNS.Transport.register_announce_handler(Handler())
    print("listening for announces (passive — no request_path)...", flush=True)
    while True: time.sleep(1)

# --- orchestrator ----------------------------------------------------------------
def main():
    raks = [t for t in sorted(glob.glob('/dev/ttyACM*')) if vid(t) == '239a']
    if len(raks) < 2: print("!! need 2 RAKs"); sys.exit(1)
    pa, pb = raks[0], raks[1]
    ida, idb = node_id(pa), node_id(pb)
    print(f"RAK A {pa} = {ida} (listener)\nRAK B {pb} = {idb} (announcer)")
    A_ID, B_ID = "A1A1A1A1A1A1A1A1", "B2B2B2B2B2B2B2B2"
    cfg_a = os.path.join(ROOT, "reticulum", "inst_a")
    cfg_b = os.path.join(ROOT, "reticulum", "inst_b")
    write_config(cfg_a, pa, A_ID, B_ID)   # listener
    write_config(cfg_b, pb, B_ID, A_ID)   # announcer

    print("\nstarting announcer (B)...")
    ann = subprocess.Popen([PYTHON, __file__, "--role", "announcer", "--config", cfg_b],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    desthash = None; t0 = time.time()
    while time.time() - t0 < 30:
        line = ann.stdout.readline()
        if line and line.startswith("DESTHASH"):
            desthash = line.split()[1]; print("announcer destination:", desthash); break
    if not desthash:
        print("!! announcer didn't start"); ann.terminate(); sys.exit(1)

    print("starting listener (A) — waiting for B's announce to arrive PASSIVELY...\n")
    lis = subprocess.Popen([PYTHON, __file__, "--role", "listener", "--config", cfg_a],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    got = None; t0 = time.time()
    while time.time() - t0 < 240:
        line = lis.stdout.readline()
        if not line:
            if lis.poll() is not None: break
            continue
        print("  [listener]", line.rstrip())
        if line.startswith("ANNOUNCE-RECEIVED"):
            got = line.split()[1]; break

    for p in (ann, lis):
        try: p.terminate()
        except: pass
    ok = (got == desthash)
    print("\n==================== RESULT ====================")
    if ok:
        print(f"  ANNOUNCE PROPAGATED ✓  listener received B's destination {got} over the mesh")
    else:
        print(f"  FAILED: listener got {got!r}, expected {desthash} (announce did NOT propagate)")
    sys.exit(0 if ok else 2)

if __name__ == "__main__":
    if "--role" in sys.argv:
        import argparse
        ap = argparse.ArgumentParser()
        ap.add_argument("--role", choices=["announcer", "listener"])
        ap.add_argument("--config", required=True)
        a = ap.parse_args()
        (announcer if a.role == "announcer" else listener)(a.config)
    else:
        main()
