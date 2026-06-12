#!/usr/bin/env bash
# Build firmware for all boards and stage UF2 + .dfu.json into web/fw/ so the
# local commissioning hub (web/index.html) can flash without a public release.
# Run from the repo root:  bash scripts/refresh_web_fw.sh
set -e
PIO=~/.platformio/penv/bin/pio
$PIO run -e wiscore_rak4631 -e xiao_nrf52 -e promicro
UF2CONV=$(find ~/.platformio/packages -name uf2conv.py | head -1)
mkdir -p web/fw
for b in wiscore_rak4631:agn-rak xiao_nrf52:agn-xiao promicro:agn-promicro; do
  env=${b%%:*}; name=${b##*:}
  python3 "$UF2CONV" .pio/build/$env/firmware.hex -c -f 0xADA52840 -o web/fw/$name.uf2
  python3 scripts/mk_dfu_json.py .pio/build/$env/firmware.zip web/fw/$name.dfu.json
done
echo "web/fw/ refreshed — open http://localhost:8000/web/ and set firmware source to ./fw/"
