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

# ESP32-S3 boards (XIAO ESP32-S3, Heltec V4): flashed in-browser via ESP Web Tools, which needs
# the parts at their standard Arduino-ESP32-S3 offsets + a manifest. Mirror the CI packaging.
BOOT_APP0=$(find ~/.platformio/packages -name boot_app0.bin | head -1)
MANIFEST='{"name":"agnostic-LoRa-Net %s","new_install_prompt_erase":true,"builds":[{"chipFamily":"ESP32-S3","parts":[{"path":"%s-bootloader.bin","offset":0},{"path":"%s-partitions.bin","offset":32768},{"path":"%s-bootapp0.bin","offset":57344},{"path":"%s.bin","offset":65536}]}]}\n'
for b in xiao_esp32s3:agn-xiao-s3 heltec_v4:agn-heltec-v4; do
  env=${b%%:*}; name=${b##*:}
  cp ".pio/build/$env/bootloader.bin" web/fw/$name-bootloader.bin
  cp ".pio/build/$env/partitions.bin" web/fw/$name-partitions.bin
  cp "$BOOT_APP0"                      web/fw/$name-bootapp0.bin
  cp ".pio/build/$env/firmware.bin"    web/fw/$name.bin
  printf "$MANIFEST" "$name" "$name" "$name" "$name" "$name" > web/fw/$name.manifest.json
done

echo "web/fw/ refreshed — open http://localhost:8000/web/ and set firmware source to ./fw/"
echo "note: ESP32-S3 boards now flash in-browser via ESP Web Tools; T1000-E flashes via 'pio run -e tracker_t1000_e -t upload'."
