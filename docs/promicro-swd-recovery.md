# Pro Micro nRF52840 — SWD bootloader recovery

Recovering a Pro Micro / nice!nano clone whose **bootloader** is dead, i.e. the board
no longer enumerates as a UF2 drive or a COM port.

## Symptom that sends you here

Windows Device Manager / `usbipd list` shows:

```
BUSID 1-1   VID:PID 0000:0002
USB\VID_0000&PID_0002\...
Status : Error
"Unknown USB Device (Device Descriptor Request Failed)"
```

No `NICENANO` drive, no COM port. `VID_0000&PID_0002` is Windows' placeholder for
"a device attached and I could not read one descriptor from it".

Read it as: the nRF52840's USBD peripheral **was** enabled and asserted the D+ pullup,
then the CPU hung before answering `GET_DESCRIPTOR`. The chip is alive; the bootloader
starts and dies. Common causes:

- Bootloader built expecting an external **32.768 kHz crystal (LFXO)** that this clone
  does not populate → blocks forever in clock init, right around USB bring-up.
- Partially written / interrupted bootloader region.
- Bootloader/SoftDevice version mismatch against what the MBR expects.

## Why USB cannot save you

The nRF52840 has **no ROM DFU**. Unlike STM32 or ESP32 there is no factory fallback in
silicon — the bootloader in flash *is* the only USB path. Once it does not enumerate,
SWD is the only way back in.

## Rule out the free causes first

1. **Double-tap RST** fast (<500 ms apart). Most Superminis have no button — short the
   RST pad to GND twice. A `NICENANO` drive appearing means you are done.
2. **Different cable, direct port, no hub.** A marginal cable can produce genuine
   descriptor failures.
3. Unplug ~30 s to drain the rails, replug, watch for an LED flicker — that
   distinguishes a boot loop from a hard hang.

## Choosing a probe

The recovery may require a **CTRL-AP ERASEALL** (AP #1) to clear `APPROTECT`. This rules
out some probes. From `tool-openocd/openocd/scripts/target/nrf52.cfg`:

> A high level adapter (like a ST-Link) you are currently using cannot access
> the CTRL-AP so 'nrf52_recover' command will not work.

| Probe | CTRL-AP? | Notes |
|---|---|---|
| **Raspberry Pi Pico** + `debugprobe.uf2` (~$4) | yes | CMSIS-DAP, full multi-AP. Best value; the probe itself is drag-drop flashable, so no chicken-and-egg. |
| **J-Link EDU Mini** (~$20) | yes | Nordic's home turf, zero drama. `tool-jlink` already bundled. |
| ST-Link V2 clone (~$3) | **no** (in HLA mode) | Only works via `stlink-dap.cfg` with recent ST-Link firmware; clones vary and many cannot be updated. **Avoid for this job.** |
| XIAO ESP32-S3 → `blackmagic-espidf` | yes | Zero cost if you already own one, but see the ESP32-S3-over-WSL flashing pain in `docs/` notes. |

## Tooling — already installed

No installs needed; PlatformIO ships both:

- `~/.platformio/packages/tool-openocd` — OpenOCD 0.12.0, with `target/nrf52.cfg`
  (`nrf52_recover`) and `interface/` configs for `cmsis-dap`, `jlink`, `stlink*`,
  `sysfsgpio-raspberrypi`
- `~/.platformio/packages/tool-jlink` — full J-Link suite incl. `JLinkExe`, JFlash

## Wiring

Four wires: **SWDIO, SWCLK, GND, 3V3**.

On the Pro Micro nRF52840 Supermini, SWDIO/SWCLK are usually two small pads on the
**underside** (sometimes silkscreened `D`/`C`), with GND and 3V3 available on the
castellated edge. **Pad location varies by clone revision** — photograph the underside
and cross-check against <https://github.com/joaquimorg/PromicroMeshtastic> (the board
`url` in `boards/promicro_nrf52840.json`) before soldering.

Power the target from USB, not from the probe's 3V3, and connect the probe's 3V3 pin as
a **reference only** — or power from the probe with USB unplugged. Do not drive both.

## Recovery — OpenOCD (Pico debugprobe / CMSIS-DAP)

Attach the probe to WSL first: `usbipd list` then `usbipd attach --wsl --busid <id>`.

```bash
OCD=~/.platformio/packages/tool-openocd
$OCD/bin/openocd -s $OCD/openocd/scripts \
  -f interface/cmsis-dap.cfg -c "transport select swd" \
  -f target/nrf52.cfg
```

Expect `Info : nRF52840-xxAA(build code: ...) 1024kB Flash, 256kB RAM`. If instead you
see `Error: Cannot access nRF52 CTRL-AP` the wiring is wrong; if you see the AP-lock
message, APPROTECT is set. Then in a second terminal (`telnet localhost 4444`):

```
nrf52_recover                 # mass erase + unlock via CTRL-AP. ONLY if locked.
halt
program /path/to/nice_nano_bootloader-0.9.2_s140_6.1.1.hex verify
reset
```

## Recovery — J-Link

```bash
~/.platformio/packages/tool-jlink/JLinkExe -device nRF52840_xxAA -if SWD -speed 4000
```
```
connect
erase                          # or: exec EnableEraseAllFlashBanks + erase
loadfile /path/to/nice_nano_bootloader-0.9.2_s140_6.1.1.hex
r
g
```

## Which bootloader hex

Use the **stock nice!nano bootloader**, not the OTAFix one:

- `nice_nano_bootloader-0.9.2_s140_6.1.1.hex` from
  <https://github.com/adafruit/Adafruit_nRF52_Bootloader/releases>

Take the `.hex` (full image: MBR + SoftDevice + bootloader), **not** the `.uf2` — the
UF2 needs a working bootloader to accept it, which is the thing you do not have.

This matches `boards/promicro_nrf52840.json`: SoftDevice `s140` `6.1.1` (fwid `0x00B6`),
bootloader `settings_addr 0xFF000`. The board's UF2 drive label is `NICENANO`.

**Do not reflash the OTAFix bootloader.** ALN firmware never uses BLE OTA DFU — the only
DFU path in the codebase is `DFU_MAGIC_SERIAL_ONLY_RESET` (serial DFU) at
`src/main.cpp:2376`, and no `BLEDfu` service is ever registered. OTAFix fixes a
capability this project does not call.

## Back to a node

Once the `NICENANO` drive returns:

```bash
# from Windows Explorer, drag onto the NICENANO drive:
web/fw/agn-promicro.uf2
```

Or serial DFU: `pio run -e promicro -t upload`.

Then confirm on the console: node ID, `nbrs=`, and per `docs/hardware-bringup.md` treat
the RXEN / POWER_EN RF-switch wiring as the prime suspect if it comes up but hears
nothing.
