#!/usr/bin/env python3
# Orchestrate a Reticulum-over-agnostic-LoRa-Net proof on this one laptop:
# two fully isolated RNS instances, each bound to one RAK, connected ONLY by the
# LoRa mesh (no LAN/loopback interface). Runs the echo: a packet from instance A
# must traverse the mesh to instance B and have its proof come back.
import serial, glob, os, re, sys, time, subprocess, shutil, signal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IFACE_SRC = os.path.join(ROOT, "reticulum", "interfaces", "AgnosticLoraInterface.py")
PYTHON = os.path.join(ROOT, ".venv-rns", "bin", "python")

def vid(t):
    p = os.path.realpath(f'/sys/class/tty/{os.path.basename(t)}/device')
    while p and p != '/' and not os.path.exists(f'{p}/idVendor'): p = os.path.dirname(p)
    try: return open(f'{p}/idVendor').read().strip()
    except: return ''

def node_id(port):
    s = serial.Serial(port, 115200, timeout=0.2); s.dtr = True; time.sleep(0.4)
    end = time.time() + 6
    while time.time() < end:
        ln = s.readline()
        m = re.search(rb'node=([0-9A-Fa-f]{8})', ln or b'')
        if m: s.close(); return m.group(1).decode().upper()
    s.close(); return None

def write_config(cfgdir, port, identity, peer_identity):
    os.makedirs(os.path.join(cfgdir, "interfaces"), exist_ok=True)
    shutil.copy(IFACE_SRC, os.path.join(cfgdir, "interfaces", "AgnosticLoraInterface.py"))
    with open(os.path.join(cfgdir, "config"), "w") as f:
        # Identity-addressed: the interface REGISTERs `identity` and RESOLVEs
        # `peer_identity` to a node id via the distributed directory — the far node id is
        # discovered, not hardcoded (Phase 4 of docs/distributed-lookup-plan.md).
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

def main():
    raks = [t for t in sorted(glob.glob('/dev/ttyACM*')) if vid(t) == '239a']
    if len(raks) < 2: print("!! need 2 RAKs"); sys.exit(1)
    pa, pb = raks[0], raks[1]
    ida, idb = node_id(pa), node_id(pb)
    print(f"RAK A {pa} = {ida}\nRAK B {pb} = {idb}")
    if not ida or not idb: print("!! node ids?"); sys.exit(1)

    cfg_a = os.path.join(ROOT, "reticulum", "inst_a")   # client side, talks to peer B
    cfg_b = os.path.join(ROOT, "reticulum", "inst_b")   # server side, talks to peer A
    # Opaque identities (NOT node ids) — each side registers its own and resolves the
    # other's to a node id via the directory. Proves identity-addressed routing.
    A_ID, B_ID = "A1A1A1A1A1A1A1A1", "B2B2B2B2B2B2B2B2"
    write_config(cfg_a, pa, A_ID, B_ID)   # A registers A_ID, resolves B_ID -> B's node
    write_config(cfg_b, pb, B_ID, A_ID)   # B registers B_ID, resolves A_ID -> A's node
    print(f"identities: A={A_ID} (at {ida})  B={B_ID} (at {idb})")

    print("\nstarting RNS server (instance B)...")
    srv = subprocess.Popen([PYTHON, os.path.join(HERE, "rns_echo.py"), "--role", "server", "--config", cfg_b],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    desthash = None
    t0 = time.time()
    while time.time() - t0 < 30:
        line = srv.stdout.readline()
        if not line: continue
        if line.startswith("DESTHASH"):
            desthash = line.split()[1]; print("server echo destination:", desthash); break
    if not desthash:
        print("!! server didn't start"); srv.terminate(); sys.exit(1)

    # give the server a moment to send its first announce into the mesh
    time.sleep(3)
    print("starting RNS client (instance A) — probing the server through the mesh...\n")
    cli = subprocess.Popen([PYTHON, os.path.join(HERE, "rns_echo.py"), "--role", "client",
                            "--config", cfg_a, "--dest", desthash],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    result = None
    t0 = time.time()
    while time.time() - t0 < 320:
        line = cli.stdout.readline()
        if not line:
            if cli.poll() is not None: break
            continue
        print("  [client]", line.rstrip())
        if line.startswith("RESULT"): result = line.split()[1]; break

    srv.send_signal(signal.SIGINT); cli.send_signal(signal.SIGINT)
    time.sleep(0.5)
    for p in (srv, cli):
        try: p.kill()
        except: pass
    print("\n==================== RESULT ====================")
    print(f"  Reticulum echo over agnostic-LoRa-Net: {'DELIVERED ✓ (packet round-tripped through the mesh)' if result=='delivered' else 'FAILED: '+str(result)}")
    sys.exit(0 if result == "delivered" else 2)

if __name__ == "__main__":
    main()
