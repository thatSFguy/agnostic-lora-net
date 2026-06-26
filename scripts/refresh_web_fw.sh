#!/usr/bin/env bash
# Build firmware for all boards and stage UF2 + .dfu.json into web/fw/ so the
# local commissioning hub (web/flash.html) can flash without a public release.
# Run from the repo root:  bash scripts/refresh_web_fw.sh
set -e
PIO=~/.platformio/penv/bin/pio
$PIO run -e wiscore_rak4631 -e xiao_nrf52 -e promicro -e tracker_t1000_e -e heltec_v4 -e xiao_esp32s3
UF2CONV=$(find ~/.platformio/packages -name uf2conv.py | head -1)
mkdir -p web/fw

# nRF52 boards: UF2 (double-tap-reset bootloader) + .dfu.json (in-browser serial DFU).
for b in wiscore_rak4631:agn-rak xiao_nrf52:agn-xiao promicro:agn-promicro tracker_t1000_e:agn-t1000; do
  env=${b%%:*}; name=${b##*:}
  python3 "$UF2CONV" .pio/build/$env/firmware.hex -c -f 0xADA52840 -o web/fw/$name.uf2
  python3 scripts/mk_dfu_json.py .pio/build/$env/firmware.zip web/fw/$name.dfu.json
done

# ESP32-S3 boards (XIAO ESP32-S3, Heltec V4): in-browser flashing via ESP Web Tools.
# Same packaging the CI uses (parts at the standard offsets + manifest).
bash scripts/pkg_esp32_webtools.sh web/fw

# Stamp the version the flasher badge reads (same source as AGN_FW_VERSION / the CI deploy).
V=$(git describe --tags --always --dirty 2>/dev/null || echo dev); V=${V#v}
printf '%s' "$V" > web/fw/version.txt
echo "web/fw/version.txt = $V"

echo "web/fw/ refreshed — open http://localhost:8000/web/ and set firmware source to ./fw/"
echo "note: ESP32-S3 boards now flash in-browser via ESP Web Tools; T1000-E flashes via 'pio run -e tracker_t1000_e -t upload'."
