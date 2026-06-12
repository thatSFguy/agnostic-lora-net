// nrf-dfu.js — Adafruit/Nordic nRF52 serial DFU over Web Serial.
//
// A faithful port of adafruit-nrfutil's dfu_transport_serial (the protocol
// `pio run -t upload` speaks): SLIP/HCI framed reliable packets, START → INIT →
// DATA* → STOP, ack-paced. Consumes the self-contained .dfu.json this repo's
// CI emits (base64 firmware + hex init packet) — no in-browser ZIP needed.
//
// The board must be in its UF2/serial bootloader first. For boards running our
// app, a 1200-baud "touch" reboots them into the bootloader; if that doesn't
// surface a port (or for a bricked board), the caller falls back to UF2.
'use strict';

const DFU = (() => {
  const START = 3, INIT = 1, DATA = 4, STOP = 5;     // packet types
  const HCI_TYPE = 14, DIP = 1, RP = 1;              // integrity-check + reliable
  const FEND = 0xC0, FESC = 0xDB, TFEND = 0xDC, TFESC = 0xDD;

  function crc16(bytes, crc = 0xFFFF) {
    for (const b of bytes) {
      crc = ((crc >> 8) & 0x00FF) | ((crc << 8) & 0xFF00);
      crc ^= b;
      crc ^= (crc & 0x00FF) >> 4;
      crc ^= (crc << 8) << 4;
      crc ^= ((crc & 0x00FF) << 4) << 1;
      crc &= 0xFFFF;
    }
    return crc & 0xFFFF;
  }
  const u32 = v => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255];
  const u16 = v => [v & 255, (v >>> 8) & 255];

  // 4-byte SLIP/HCI header (adafruit-nrfutil slip_parts_to_four_bytes).
  function hciHeader(seq, len) {
    const a0 = seq | (((seq + 1) % 8) << 3) | (DIP << 6) | (RP << 7);
    const a1 = HCI_TYPE | ((len & 0x000F) << 4);
    const a2 = (len & 0x0FF0) >> 4;
    const a3 = ((~(a0 + a1 + a2)) + 1) & 0xFF;        // header checksum
    return [a0, a1, a2, a3];
  }
  function slipEscape(bytes) {                        // inner esc (not the framing)
    const out = [];
    for (const b of bytes) {
      if (b === FEND) out.push(FESC, TFEND);
      else if (b === FESC) out.push(FESC, TFESC);
      else out.push(b);
    }
    return out;
  }
  // One reliable HCI packet: FEND | header | data | crc16 | FEND (esc'd inside).
  function hciPacket(seq, data) {
    const body = hciHeader(seq, data.length).concat(data);
    const c = crc16(body);
    body.push(c & 0xFF, (c >> 8) & 0xFF);
    return new Uint8Array([FEND, ...slipEscape(body), FEND]);
  }

  const sleep = ms => new Promise(r => setTimeout(r, ms));

  // 1200-baud touch: open/close to kick the app into its bootloader.
  async function touch(port) {
    try { await port.open({ baudRate: 1200 }); await sleep(120); await port.close(); }
    catch (e) { /* may already be in bootloader */ }
    await sleep(1600);
  }

  // Stream a whole DFU update. `dfu` = parsed .dfu.json. `log`/`prog` callbacks.
  async function flash(port, dfu, { log = () => {}, prog = () => {} } = {}) {
    const fw = Uint8Array.from(atob(dfu.fw_bin_b64), c => c.charCodeAt(0));
    const init = dfu.init_dat_hex.match(/../g).map(h => parseInt(h, 16));
    const appSize = dfu.app_size;

    await port.open({ baudRate: 115200 });
    const writer = port.writable.getWriter();
    const reader = port.readable.getReader();
    let seq = 0;
    const next = () => (seq = (seq + 1) % 8);

    // Drain one ack (frame bounded by two FENDs) or time out — matches the
    // lenient python transport, which paces on the ack but doesn't hard-verify.
    async function ack(timeoutMs = 2500) {
      const deadline = Date.now() + timeoutMs;
      let fends = 0;
      while (Date.now() < deadline) {
        const { value, done } = await Promise.race([
          reader.read(),
          sleep(timeoutMs).then(() => ({ value: null, done: false })),
        ]);
        if (done || !value) break;
        for (const b of value) if (b === FEND && ++fends >= 2) return true;
      }
      return false;
    }
    async function send(type, payload = []) {
      const pkt = hciPacket(next(), u32(type).concat(payload));
      await writer.write(pkt);
      await ack();
    }

    try {
      log('START (app ' + appSize + ' B)');
      // START: mode=APP(4), image sizes sd=0 bl=0 app=appSize
      await send(START, u32(4).concat(u32(0)).concat(u32(0)).concat(u32(appSize)));
      // erase wait: max(0.5, (size/4096 + 1) * 89.7ms)
      await sleep(Math.max(500, ((appSize >> 12) + 1) * 90));

      log('INIT packet');
      await send(INIT, init.concat(u16(0)));          // + 2-byte padding

      log('firmware…');
      const CHUNK = 512;
      for (let off = 0, n = 0; off < fw.length; off += CHUNK, n++) {
        await send(DATA, Array.from(fw.subarray(off, off + CHUNK)));
        if ((n & 7) === 0) { await sleep(5); prog(Math.round(100 * off / fw.length)); }
      }
      prog(100);
      await send(STOP);
      log('activating — node will reboot');
    } finally {
      try { reader.releaseLock(); } catch (e) {}
      try { writer.releaseLock(); } catch (e) {}
      try { await port.close(); } catch (e) {}
    }
    await sleep(1500);                                 // activate + reset
    return true;
  }

  return { flash, touch, crc16, hciPacket };
})();
