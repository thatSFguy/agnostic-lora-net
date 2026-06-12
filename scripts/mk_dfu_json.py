#!/usr/bin/env python3
# mk_dfu_json.py — turn a PlatformIO/adafruit-nrfutil DFU package (firmware.zip)
# into a single self-contained JSON the browser flasher streams. Avoids in-browser
# ZIP/DEFLATE: the web app fetches one file and runs the serial DFU protocol.
#
#   python3 scripts/mk_dfu_json.py <firmware.zip> <out.dfu.json>
import sys, json, base64, zipfile

def main(zip_path, out_path):
    z = zipfile.ZipFile(zip_path)
    man = json.loads(z.read('manifest.json'))
    app = man['manifest']['application']
    binb = z.read(app['bin_file'])
    datb = z.read(app['dat_file'])
    obj = {
        'fw_bin_b64': base64.b64encode(binb).decode(),
        'init_dat_hex': datb.hex(),
        'app_size': len(binb),
        'dfu_version': man['manifest'].get('dfu_version'),
    }
    with open(out_path, 'w') as f:
        json.dump(obj, f)
    print(f'{out_path}: app={len(binb)}B init={len(datb)}B')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
