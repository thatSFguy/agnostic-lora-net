#!/usr/bin/env bash
# Package the ESP32-S3 firmware (XIAO ESP32-S3, Heltec V4) as ESP Web Tools parts + a
# manifest, so the in-browser flasher (web/flash.html) can flash them over Web Serial.
# Parts go at the standard Arduino-ESP32-S3 flash offsets:
#   bootloader 0x0 (0), partition table 0x8000 (32768), boot_app0 0xe000 (57344), app 0x10000 (65536).
# Run AFTER `pio run -e xiao_esp32s3 -e heltec_v4`.
# Usage: bash scripts/pkg_esp32_webtools.sh [outdir]   (default outdir = current dir)
set -e
OUT=${1:-.}
mkdir -p "$OUT"
BOOT_APP0=$(find ~/.platformio/packages -name boot_app0.bin | head -1)
echo "boot_app0: $BOOT_APP0"
for b in xiao_esp32s3:agn-xiao-s3 heltec_v4:agn-heltec-v4; do
  env=${b%%:*}; name=${b##*:}
  cp ".pio/build/$env/bootloader.bin" "$OUT/$name-bootloader.bin"
  cp ".pio/build/$env/partitions.bin" "$OUT/$name-partitions.bin"
  cp "$BOOT_APP0"                      "$OUT/$name-bootapp0.bin"
  cp ".pio/build/$env/firmware.bin"    "$OUT/$name.bin"
  cat > "$OUT/$name.manifest.json" <<EOF
{"name":"agnostic-LoRa-Net $name","new_install_prompt_erase":true,"builds":[{"chipFamily":"ESP32-S3","parts":[{"path":"$name-bootloader.bin","offset":0},{"path":"$name-partitions.bin","offset":32768},{"path":"$name-bootapp0.bin","offset":57344},{"path":"$name.bin","offset":65536}]}]}
EOF
done
ls -la "$OUT"/agn-xiao-s3* "$OUT"/agn-heltec-v4*
