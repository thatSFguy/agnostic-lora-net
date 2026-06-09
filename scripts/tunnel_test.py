#!/usr/bin/env python3
# Validate the firmware's tunnel framing WITHOUT Reticulum: put both RAKs in tunnel
# mode, push an HDLC [dst][payload] frame into one, and confirm the other emits an
# HDLC [src][payload] frame with the same bytes. Isolates firmware from RNS config.
import serial, glob, os, re, sys, time, struct

FLAG=0x7E; ESC=0x7D; MASK=0x20
def hdlc(data):
    out=bytearray([FLAG])
    for b in data:
        if b in (FLAG,ESC): out += bytes([ESC, b^MASK])
        else: out.append(b)
    out.append(FLAG); return bytes(out)
def vid(t):
    p=os.path.realpath(f'/sys/class/tty/{os.path.basename(t)}/device')
    while p and p!='/' and not os.path.exists(f'{p}/idVendor'): p=os.path.dirname(p)
    try: return open(f'{p}/idVendor').read().strip()
    except: return ''

def read_frames(s, secs):
    """Yield decoded HDLC frames seen on s for `secs`."""
    end=time.time()+secs; inf=False; esc=False; buf=bytearray(); out=[]
    while time.time()<end:
        b=s.read(1)
        if not b: continue
        c=b[0]
        if inf and c==FLAG:
            inf=False
            if buf: out.append(bytes(buf))
        elif c==FLAG: inf=True; esc=False; buf=bytearray()
        elif inf:
            if c==ESC: esc=True
            else: buf.append(c^MASK if esc else c); esc=False
    return out

def main():
    raks=[t for t in sorted(glob.glob('/dev/ttyACM*')) if vid(t)=='239a']
    if len(raks)<2: print("!! need 2 RAKs"); sys.exit(1)
    A=serial.Serial(raks[0],115200,timeout=0.2); A.dtr=True
    B=serial.Serial(raks[1],115200,timeout=0.2); B.dtr=True
    time.sleep(0.4)
    # read node ids from heartbeat (console mode, before tunnel)
    def nid(s):
        end=time.time()+6
        while time.time()<end:
            ln=s.readline()
            if ln:
                m=re.search(rb'node=([0-9A-Fa-f]{8})', ln)
                if m: return int(m.group(1),16)
        return None
    aid,bid=nid(A),nid(B)
    print(f"A {raks[0]} = {aid:08X}\nB {raks[1]} = {bid:08X}")

    # enable tunnel mode on both
    for s in (A,B): s.write(b"tunnel\n"); s.flush()
    time.sleep(0.5); A.reset_input_buffer(); B.reset_input_buffer()

    def trial(src, sid, dst, did, msg):
        print(f"\n=== {sid:08X} --tunnel--> {did:08X} : {msg} ===")
        src.write(hdlc(struct.pack('<I',did)+msg)); src.flush()
        for f in read_frames(dst, 10):
            if len(f)>=4:
                fsrc=struct.unpack('<I',f[:4])[0]; payload=f[4:]
                if payload==msg:
                    print(f"   [dst] got frame from {fsrc:08X}: {payload!r}  ✓"); return True
        print("   NOT received"); return False

    ab=trial(A,aid,B,bid,b"hello-tunnel-AB")
    ba=trial(B,bid,A,aid,b"hello-tunnel-BA")
    print(f"\nA->B: {'PASS' if ab else 'FAIL'}\nB->A: {'PASS' if ba else 'FAIL'}")
    A.close(); B.close()
    sys.exit(0 if (ab and ba) else 2)

if __name__=='__main__': main()
