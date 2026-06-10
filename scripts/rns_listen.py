#!/usr/bin/env python3
# Catch-all RNS announce listener bound to ONE node — for confirming an external app's
# announce arrives over the mesh and fires received_announce (any aspect). Receive-only:
# the interface needs no peer/identity, it just delivers inbound to RNS.
import RNS, time, os, shutil, argparse
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
IFACE = os.path.join(ROOT, "reticulum", "interfaces", "AgnosticLoraInterface.py")

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--port", required=True); a = ap.parse_args()
    cfgdir = os.path.join(ROOT, "reticulum", "inst_listen")
    os.makedirs(os.path.join(cfgdir, "interfaces"), exist_ok=True)
    shutil.copy(IFACE, os.path.join(cfgdir, "interfaces", "AgnosticLoraInterface.py"))
    with open(os.path.join(cfgdir, "config"), "w") as f:
        f.write(f"""[reticulum]
  enable_transport = True
  share_instance = No
[logging]
  loglevel = 3
[interfaces]
  [[Mesh Link]]
    type = AgnosticLoraInterface
    interface_enabled = yes
    port = {a.port}
    speed = 115200
""")
    RNS.Reticulum(cfgdir)
    class Handler:
        aspect_filter = None   # catch EVERY announce, any aspect (incl. lxmf.delivery)
        def received_announce(self, destination_hash, announced_identity, app_data):
            print("RECV-ANNOUNCE " + destination_hash.hex(), flush=True)
    RNS.Transport.register_announce_handler(Handler())
    print(f"listening for ANY announce on {a.port} ...", flush=True)
    while True: time.sleep(1)

if __name__ == "__main__":
    main()
