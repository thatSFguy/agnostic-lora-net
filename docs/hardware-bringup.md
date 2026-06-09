# Hardware bring-up

Validating the firmware on real radios, in order. **Step 1 (this is the do-now):**
two RAK4631s as a 2-node link. **Step 2:** add a third node (XIAO nRF52 / Pro Micro)
for multi-hop forwarding + ARQ.

> ⚠️ **Attach the antenna before powering any node.** Transmitting without an
> antenna can damage the SX1262 PA. Every node here transmits (beacons).

> Scope: this firmware is the **radio + routing** layer. It has **no BLE** yet — the
> phone/BLE-coexistence requirement (Req 1) is validated later, on the MeshCore fork
> (`docs/meshcore-integration.md`). Step 1 proves beacons, link metrics, neighbour/
> route tables, and (with a 3rd node) forwarding + per-hop ACK.

## 0. Environment note (WSL)

This repo builds inside WSL. USB flashing/monitor needs the board's serial port
visible in WSL. On this machine `usbipd-win` is installed and `/dev/ttyACM0` is
already present, so the serial path works. If a freshly-plugged board doesn't show
up in WSL:

```powershell
# in a Windows terminal
usbipd list                       # find the RAK's BUSID
usbipd attach --wsl --busid <X-Y> # forward it into WSL
```

(`usbipd.exe` is at `/mnt/c/Program Files/usbipd-win/usbipd.exe` from WSL.)
The UF2 drag-drop path below avoids USB-serial entirely if you prefer.

## 1. Build

```bash
PIO=~/.platformio/penv/bin/pio
$PIO run -e wiscore_rak4631          # -> .pio/build/wiscore_rak4631/firmware.{hex,zip}
```

Node IDs auto-derive from each chip's FICR, so **two RAKs already get distinct IDs** —
no per-board config needed. (To pin one explicitly:
`PLATFORMIO_BUILD_FLAGS="-DAGN_NODE_ID=0x0000000A" $PIO run -e wiscore_rak4631`.)

## 2. Flash (pick one)

**A. Serial DFU (from WSL).** Put the board in the bootloader (double-tap RST) or let
PlatformIO's 1200-bps touch do it, then:

```bash
$PIO run -e wiscore_rak4631 -t upload --upload-port /dev/ttyACM0
```

**B. UF2 drag-drop (from Windows, no serial needed).** Convert the build to UF2 once,
double-tap RST so the RAK mounts as a USB drive, and copy:

```bash
# nRF52840 UF2 family id = 0xADA52840
UF2=~/.platformio/packages/framework-arduinoadafruitnrf52/tools/uf2conv/uf2conv.py
python3 "$UF2" .pio/build/wiscore_rak4631/firmware.hex -c -f 0xADA52840 \
        -o /mnt/c/Users/<you>/Downloads/agn-rak.uf2
# then in Windows Explorer, drag agn-rak.uf2 onto the RAK's bootloader drive
```

Flash **both** RAK4631s the same way.

## 3. Monitor

Open a serial monitor on each board (separate terminals; the second board may be
`/dev/ttyACM1`):

```bash
$PIO device monitor -e wiscore_rak4631 -p /dev/ttyACM0
```

## 4. What success looks like

Each node prints its banner, beacons every ~10 s, and reports what it hears:

```
=== LoRa Mesh Backbone — Phase 0 link prober ===
fw=0.0.1-phase0  node=1A2B3C4D
PHY: 904.375 MHz BW250 SF11 CR4/5 sync=0x4D
header=17 bytes (link=4 net=13)
radio up, listening...
[TX] beacon seq=0 from 1A2B3C4D  +announce 8B
[RX] beacon  src=00000B17 seq=2 up=23s  rssi=-41.0 dBm  snr=11.5 dB  q=1.00  neighbors=1  routes=2
```

**Pass criteria (2-node):**
- both nodes reach `radio up, listening...` (no `RADIO INIT FAILED`);
- each sees the **other's** `src` with `neighbors=1`;
- `routes=2` (self + peer), `q` plausibly high at close range (~0.8–1.0);
- stable for several minutes — IDs steady, beacons keep flowing.

## 5. Troubleshooting

| Symptom | Likely cause |
|---|---|
| `RADIO INIT FAILED: <n>` | SPI/pins or PHY; `<n>` is a RadioLib error code. Re-seat the WisBlock core; confirm it's a RAK4631 (SX1262). |
| TX prints, but `neighbors=0` forever | The other node isn't transmitting on the same PHY, out of range, or antenna missing. Both must be 904.375 MHz / BW250 / SF11 / sync 0x4D (they are, from one firmware). |
| Resets / USB drops | Power; try a powered hub. (No BLE here, so it isn't BLE timing.) |
| WSL can't see the port | `usbipd attach` (§0) or use UF2 drag-drop (§2B). |

## 6. Step 2 — add a 3rd node for multi-hop + ARQ

Once the pair works, flash the XIAO nRF52 (`xiao_nrf52` env) or Pro Micro
(`promicro` env) as node C and build one node with a data destination so it
originates traffic:

```bash
# Make node A send to node C's ID every 30 s
PLATFORMIO_BUILD_FLAGS="-DAGN_DATA_DEST=0x<C-node-id>" $PIO run -e wiscore_rak4631 -t upload
```

To force a *real* multi-hop (A→B→C) rather than A talking to C directly, separate A
and C physically (or drop TX power) so they can't hear each other but both hear B.
Watch for:
- `[FWD] A->C id=.. via B ttl=..` on node B (it relays);
- `[RX] DATA delivered from A id=..` on node C;
- retransmits if an ACK is missed, and no infinite resends (ARQ gives up after the
  retry limit).

> The XIAO + Wio-SX1262 and Pro Micro use a 1.8 V TCXO and an RXEN RF-switch line
> (the RAK uses neither). Those board configs are wired from MeshCore's definitions
> but **should be confirmed on first flash** — if a node reaches `radio up` but never
> hears/forwards, the RF-switch handling is the first suspect. Report what it prints
> and we'll adjust `board_config.h`.
```
