#!/usr/bin/env python3
# 3-node multi-hop image transfer: RAK_A --block--> can't reach RAK_C directly, so it
# routes through the XIAO repeater. Uses the runtime `block` console command (no
# reflash) to force the relay, waits for the route to re-converge via the repeater,
# then runs the SAR transfer and verifies byte-perfect integrity.
import serial, glob, os, re, sys, time, zlib, hashlib

IMG = os.path.join(os.path.dirname(__file__), '..', 'test', 'test_orig.png')

def vid(t):
    p = os.path.realpath(f'/sys/class/tty/{os.path.basename(t)}/device')
    while p and p != '/' and not os.path.exists(f'{p}/idVendor'):
        p = os.path.dirname(p)
    try: return open(f'{p}/idVendor').read().strip()
    except: return ''
def op(t):
    s = serial.Serial(t, 115200, timeout=0.2); s.dtr = True; s.rts = True; return s
def drain(s, secs):
    out=[]; end=time.time()+secs
    while time.time()<end:
        ln=s.readline()
        if ln: out.append(ln.decode(errors='replace').rstrip())
    return out
def node_id(s):
    for l in drain(s,6):
        m=re.search(r'node=([0-9A-Fa-f]{8})',l)
        if m: return m.group(1)
    return None
def wait_for(s, marker, secs, echo=False):
    end=time.time()+secs
    while time.time()<end:
        ln=s.readline()
        if ln:
            t=ln.decode(errors='replace').rstrip()
            if echo and '[SAR]' in t: print("   ",t)
            if marker in t: return True
    return False

def route_via(s, dst):
    """Query `info` and return the next-hop for dst (or None)."""
    s.reset_input_buffer(); s.write(b"info\n"); s.flush()
    for l in drain(s, 3):
        m = re.search(rf'route dst={dst} via=([0-9A-Fa-f]{{8}})', l)
        if m: return m.group(1)
    return None

def main():
    img=open(IMG,'rb').read(); crc=zlib.crc32(img)&0xffffffff; sha=hashlib.sha256(img).hexdigest()
    print(f"original: {len(img)} bytes  crc32={crc:08X}  sha256={sha[:16]}..")
    raks=[t for t in sorted(glob.glob('/dev/ttyACM*')) if vid(t)=='239a']
    if len(raks)<2: print("!! need 2 RAKs on the laptop"); sys.exit(1)
    A,C=op(raks[0]),op(raks[1]); time.sleep(0.4)
    aid,cid=node_id(A),node_id(C)
    print(f"sender A {raks[0]} = {aid}\nreceiver C {raks[1]} = {cid}")
    if not aid or not cid: print("!! ids?"); sys.exit(1)

    # 1) Force the relay: A blocks the direct link to C.
    print(f"\nblocking direct link {aid} -X-> {cid} (forcing relay)...")
    A.reset_input_buffer(); A.write(f"block {cid}\n".encode()); A.flush(); drain(A,1)

    # 2) Wait for A's route to C to re-converge through the repeater (next hop != C).
    print("waiting for route to C to re-converge via the repeater...")
    via=None; t0=time.time()
    while time.time()-t0 < 90:
        via=route_via(A, cid)
        if via and via.upper()!=cid.upper():
            print(f"  A now reaches {cid} via {via}  (the repeater)  after {time.time()-t0:.0f}s")
            break
        time.sleep(3)
    else:
        print("!! route did not move to a relay — is the XIAO powered + in range of both?")
        A.write(f"unblock {cid}\n".encode()); sys.exit(1)

    # 3) Load + transfer.
    A.reset_input_buffer()
    A.write(f"sbegin {len(img)} {crc:08X}\n".encode()); A.flush(); drain(A,0.5)
    for off in range(0,len(img),256):
        A.write(f"sdata {img[off:off+256].hex()}\n".encode()); A.flush(); wait_for(A,"total=",3)
    print("loaded image into sender; starting multi-hop transfer (up to 300s)...")
    A.reset_input_buffer(); C.reset_input_buffer()
    A.write(f"xfer {cid}\n".encode()); A.flush()
    ok=wait_for(C,"[SAR] complete",300,echo=True)
    if not ok:
        print("!! did not complete"); A.write(f"unblock {cid}\n".encode()); sys.exit(1)

    # 4) Dump + verify.
    C.reset_input_buffer(); C.write(b"dump\n"); C.flush()
    hexs=[]; cap=False; end=time.time()+25
    while time.time()<end:
        ln=C.readline()
        if not ln: continue
        t=ln.decode(errors='replace').rstrip()
        if t.startswith('[DUMP]'): cap=True; print("  ",t); continue
        if t.startswith('[ENDDUMP]'): break
        if cap and re.fullmatch(r'[0-9A-Fa-f]+',t): hexs.append(t)
    recv=bytes.fromhex(''.join(hexs))
    open(os.path.join(os.path.dirname(__file__),'..','test','test_recv_mh.png'),'wb').write(recv)
    try: open('/mnt/c/Users/rob/Downloads/test_recv_mh.png','wb').write(recv)
    except: pass

    A.write(f"unblock {cid}\n".encode())   # restore the mesh
    print("\n==================== RESULT (multi-hop) ====================")
    print(f"  path:   {aid} -> {via} -> {cid}")
    print(f"  bytes:  orig={len(img)} recv={len(recv)}  {'EQUAL' if len(img)==len(recv) else 'DIFFER'}")
    print(f"  sha256: {'MATCH — byte-perfect over 2 hops ✓' if recv==img else 'MISMATCH ✗'}")
    A.close(); C.close()
    sys.exit(0 if recv==img else 2)

if __name__=='__main__': main()
