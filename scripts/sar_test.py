#!/usr/bin/env python3
# Drive an end-to-end image transfer between two RAKs over the LoRa mesh and verify
# byte-perfect integrity. Auto-detects the two RAK serial ports, streams the image
# into the sender, triggers the SAR transfer, dumps the reassembled blob from the
# receiver, and compares SHA-256.
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
    out = []; end = time.time() + secs
    while time.time() < end:
        ln = s.readline()
        if ln: out.append(ln.decode(errors='replace').rstrip())
    return out

def wait_for(s, marker, secs, echo=False):
    end = time.time() + secs
    lines = []
    while time.time() < end:
        ln = s.readline()
        if ln:
            t = ln.decode(errors='replace').rstrip(); lines.append(t)
            if echo and ('[SAR]' in t): print("   ", t)
            if marker in t: return True, lines
    return False, lines

def node_id(s):
    for l in drain(s, 6):
        m = re.search(r'node=([0-9A-Fa-f]{8})', l)
        if m: return m.group(1)
    return None

def main():
    img = open(IMG, 'rb').read()
    crc = zlib.crc32(img) & 0xffffffff
    sha = hashlib.sha256(img).hexdigest()
    print(f"original: {len(img)} bytes  crc32={crc:08X}  sha256={sha[:16]}..")

    raks = [t for t in sorted(glob.glob('/dev/ttyACM*')) if vid(t) == '239a']
    print("RAK ports:", raks)
    if len(raks) < 2: print("!! need 2 RAKs"); sys.exit(1)
    TX, RX = op(raks[0]), op(raks[1]); time.sleep(0.4)
    tx_id, rx_id = node_id(TX), node_id(RX)
    print(f"sender   {raks[0]} = {tx_id}")
    print(f"receiver {raks[1]} = {rx_id}")
    if not tx_id or not rx_id: print("!! couldn't read ids"); sys.exit(1)

    # 1) load the image into the sender, 256 bytes (512 hex) per line
    TX.reset_input_buffer()
    TX.write(f"sbegin {len(img)} {crc:08X}\n".encode()); TX.flush(); drain(TX, 0.5)
    for off in range(0, len(img), 256):
        chunk = img[off:off+256]
        TX.write(f"sdata {chunk.hex()}\n".encode()); TX.flush()
        wait_for(TX, "total=", 3)
    print("loaded image into sender")

    # 2) start the transfer
    TX.reset_input_buffer(); RX.reset_input_buffer()
    TX.write(f"xfer {rx_id}\n".encode()); TX.flush()
    print(f"transferring {tx_id} -> {rx_id} ... (watching for completion, up to 180s)")
    t0 = time.time()
    ok, _ = wait_for(RX, "[SAR] complete", 180, echo=True)
    elapsed = time.time() - t0
    if not ok:
        print("!! transfer did not complete in time"); sys.exit(1)
    print(f"receiver reports complete in {elapsed:.0f}s")

    # 3) dump the received blob and reassemble it on the laptop
    RX.reset_input_buffer()
    RX.write(b"dump\n"); RX.flush()
    got_hex = []
    capturing = False; end = time.time() + 20
    while time.time() < end:
        ln = RX.readline()
        if not ln: continue
        t = ln.decode(errors='replace').rstrip()
        if t.startswith('[DUMP]'): capturing = True; print("  ", t); continue
        if t.startswith('[ENDDUMP]'): break
        if capturing and re.fullmatch(r'[0-9A-Fa-f]*', t) and t: got_hex.append(t)
    recv = bytes.fromhex(''.join(got_hex))

    # 4) verify
    rsha = hashlib.sha256(recv).hexdigest()
    print(f"\nreceived: {len(recv)} bytes  crc32={zlib.crc32(recv)&0xffffffff:08X}  sha256={rsha[:16]}..")
    match = (recv == img)
    out = os.path.join(os.path.dirname(__file__), '..', 'test', 'test_recv.png')
    open(out, 'wb').write(recv)
    try: open('/mnt/c/Users/rob/Downloads/test_recv.png', 'wb').write(recv)
    except: pass
    print("\n==================== RESULT ====================")
    print(f"  bytes:  orig={len(img)}  recv={len(recv)}  {'EQUAL' if len(img)==len(recv) else 'DIFFER'}")
    print(f"  sha256: {'MATCH — byte-perfect transfer ✓' if match else 'MISMATCH ✗'}")
    print(f"  saved received -> test/test_recv.png (and Downloads)")
    TX.close(); RX.close()
    sys.exit(0 if match else 2)

if __name__ == '__main__':
    main()
