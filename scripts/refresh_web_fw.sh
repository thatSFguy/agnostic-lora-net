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

# ESP32-S3 boards (Heltec V4, XIAO ESP32-S3): a single application image each. ESP32 does NOT use
# the nRF52 UF2/serial-DFU path — flash over USB serial with esptool (`pio run -e <env> -t upload`)
# or ESP Web Tools. We stage the app .bin so it can be published alongside the others; in-browser
# ESP flashing is not wired into the hub (the Flash tab shows the CLI command for these).
cp .pio/build/heltec_v4/firmware.bin   web/fw/agn-heltec-v4.bin
cp .pio/build/xiao_esp32s3/firmware.bin web/fw/agn-xiao-s3.bin

echo "web/fw/ refreshed — open http://localhost:8000/web/ and set firmware source to ./fw/"
echo "note: ESP32 boards (Heltec V4, XIAO ESP32-S3) flash via 'pio run -e <env> -t upload', not the hub's nRF DFU."
