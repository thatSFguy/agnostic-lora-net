// hub.js — logic for the commissioning hub (flash.html).
// Two halves: (1) in-browser nRF52 serial-DFU flashing wizard, and (2) the Provision &
// configure step — one node connection (USB/BLE console) that does controller-key
// provisioning, BLE PIN, node name, radio PHY, battery calibration, mobility and a raw
// console. (Folded in from the old standalone node manager so there's one page, one
// connection, no duplication.)
'use strict';
const $ = id => document.getElementById(id);
// Firmware is served same-origin from ./fw/ (the Pages deploy + refresh_web_fw.sh stage it
// there; the agnctl dashboard serves /fw/ itself). The release URL is an optional fallback
// (UF2 download works; serial-DFU fetch may be blocked cross-origin).
const RELEASE_BASE = 'https://github.com/thatSFguy/agnostic-lora-net/releases/latest/download/';
const BOARD_FILE = { rak:'agn-rak', xiao:'agn-xiao', promicro:'agn-promicro' };
// ESP32 boards don't use the nRF52 serial-DFU path below — they flash with esptool /
// ESP Web Tools. The hub can't drive that, so selecting one shows guidance instead.
const ESP32_BOARD_FILE = { 'heltec-v4':'agn-heltec-v4' };
const NUS='6e400001-b5a3-f393-e0a9-e50e24dcca9e', NUS_RX='6e400002-b5a3-f393-e0a9-e50e24dcca9e', NUS_TX='6e400003-b5a3-f393-e0a9-e50e24dcca9e';

function log(m) { const el=$('log'); el.textContent=(el.textContent+m+'\n').split('\n').slice(-200).join('\n'); el.scrollTop=el.scrollHeight; }
function fwBaseUrl() { return ($('fwBase').value.trim() || './fw/'); }

// ---------- shared controller key (same localStorage key as map.html) ----------
let ctrlKeys = null;
(async function ctrlInit() {
  try {
    const saved = JSON.parse(localStorage.getItem('agn-ctrl-key') || 'null');
    if (saved) {
      const priv = await crypto.subtle.importKey('pkcs8',
        Uint8Array.from(atob(saved.priv), c=>c.charCodeAt(0)), {name:'Ed25519'}, true, ['sign']);
      ctrlKeys = { priv, pubHex: saved.pub };
    } else {
      const kp = await crypto.subtle.generateKey({name:'Ed25519'}, true, ['sign','verify']);
      const priv = btoa(String.fromCharCode(...new Uint8Array(await crypto.subtle.exportKey('pkcs8', kp.privateKey))));
      const pub = [...new Uint8Array(await crypto.subtle.exportKey('raw', kp.publicKey))]
                  .map(b=>b.toString(16).padStart(2,'0')).join('').toUpperCase();
      localStorage.setItem('agn-ctrl-key', JSON.stringify({priv, pub}));
      ctrlKeys = { priv: kp.privateKey, pubHex: pub };
    }
    $('pubShort').textContent = ctrlKeys.pubHex.slice(0,8)+'…'+ctrlKeys.pubHex.slice(-8);
  } catch (e) { $('pubShort').textContent = '(controller crypto unavailable)'; }
})();

// ---------- step flow ----------
function setStep(n) {
  for (let i=1;i<=4;i++) {
    $('st-'+i).classList.toggle('on', i===n);
    $('st-'+i).classList.toggle('done', i<n);
    $('step'+i).classList.toggle('hide', i!==n);
  }
  window.scrollTo(0,0);
}

// ---------- flashing ----------
async function doFlash() {
  const board = $('board').value;
  $('flashBoard').textContent = $('board').selectedOptions[0].text;

  // ESP32 boards (Heltec V4): no in-browser flashing here — point at the CLI route.
  if (board in ESP32_BOARD_FILE) {
    setStep(2); $('uf2Fallback').classList.add('hide'); $('prog').style.width='0';
    $('flashState').textContent='ESP32 — flash over USB serial'; $('flashState').className='pill';
    log('Heltec V4 is an ESP32-S3 board; the nRF52 serial-DFU path does not apply.');
    log('Flash it from the repo with:  pio run -e heltec_v4 -t upload');
    log('(app image also staged at '+fwBaseUrl()+ESP32_BOARD_FILE[board]+'.bin for esptool / ESP Web Tools)');
    return;
  }

  setStep(2); $('uf2Fallback').classList.add('hide'); $('prog').style.width='0';
  $('dlUf2').onclick = () => window.open(fwBaseUrl()+BOARD_FILE[board]+'.uf2', '_blank');

  if (!('serial' in navigator)) { flashFail('Web Serial not supported — use Chrome/Edge, or the UF2 fallback.'); return; }
  let dfu;
  try {
    $('flashState').textContent='fetching firmware…';
    const r = await fetch(fwBaseUrl()+BOARD_FILE[board]+'.dfu.json');
    if (!r.ok) throw new Error('HTTP '+r.status);
    dfu = await r.json();
    log('firmware: '+dfu.app_size+' B');
  } catch (e) { flashFail('Could not fetch firmware ('+e+').'); return; }

  let port;
  try {
    port = await navigator.serial.requestPort();
  } catch (e) { setStep(1); return; }   // user cancelled the picker

  try {
    if (!$('isBoot').checked) { $('flashState').textContent='rebooting to bootloader…'; log('1200-baud touch'); await DFU.touch(port); }
    $('flashState').textContent='flashing…';
    await DFU.flash(port, dfu, { log, prog: p => { $('prog').style.width=p+'%'; $('flashState').textContent='flashing… '+p+'%'; } });
    $('flashState').textContent='flashed ✓'; $('flashState').className='pill ok';
    log('done — node rebooting');
    setTimeout(afterFlash, 1500);
  } catch (e) {
    log('DFU error: '+e);
    flashFail('Auto-flash failed — use the UF2 fallback below.');
  }
}
function flashFail(msg) {
  $('flashState').textContent = msg; $('flashState').className='pill bad';
  $('uf2Fallback').classList.remove('hide');
}
function afterFlash() { setStep(3); }
function markReady() {
  $('readyNode').textContent = provNodeId || 'node';
  setStep(4);
}
function resetWizard() { provDisconnect(); setStep(1); }

// ---------- a reusable node connection (USB serial or BLE NUS), text console ----------
function makeConn() {
  let port=null, dev=null, writer=null, onText=()=>{}, buf='', inFrame=false;
  function feed(bytes) {
    for (const b of bytes) {
      if (b===0x7E) { inFrame=!inFrame; continue; }     // HDLC tunnel frame — not console text
      if (inFrame) continue;
      if (b===0x0D) continue;
      if (b===0x0A) { onText(buf); buf=''; } else if (buf.length<400) buf+=String.fromCharCode(b);
    }
  }
  return {
    set onLine(fn){ onText=fn; },
    async serial() {
      port = await navigator.serial.requestPort(); await port.open({baudRate:115200});
      writer = port.writable.getWriter();
      (async()=>{ const rd=port.readable.getReader(); try{for(;;){const{value,done}=await rd.read(); if(done)break; feed(value);}}catch(e){} })();
      return 'USB';
    },
    async ble() {
      dev = await navigator.bluetooth.requestDevice({filters:[{namePrefix:'ALN'}], optionalServices:[NUS]});
      const srv = await (await dev.gatt.connect()).getPrimaryService(NUS);
      const rx = await srv.getCharacteristic(NUS_RX), tx = await srv.getCharacteristic(NUS_TX);
      await tx.startNotifications();
      tx.addEventListener('characteristicvaluechanged', e=>feed(new Uint8Array(e.target.value.buffer)));
      writer = { write: async s => { const d=new TextEncoder().encode(s); for(let i=0;i<d.length;i+=20) await rx.writeValueWithoutResponse(d.slice(i,i+20)); } };
      return 'BLE '+dev.name;
    },
    async send(line) { if(!writer) return; const s=line+'\n';
      if (writer.write.length===1 && dev) await writer.write(s);
      else await writer.write(new TextEncoder().encode(s)); },
    close() { try{port&&port.close();}catch(e){} try{dev&&dev.gatt.disconnect();}catch(e){} writer=null; }
  };
}

// ---------- provision & configure (one connection) ----------
let prov = makeConn(), provNodeId = null, provConnected = false, loadedRf = null;
function plog(m){ const el=$('provLog'); el.textContent=(el.textContent+m+'\n').split('\n').slice(-200).join('\n'); el.scrollTop=el.scrollHeight; }
function mark(id, state, text){ const e=$(id); e.textContent=text; e.className='pill'+(state?' '+state:''); }

function fillRf(r){
  $('rf_freq').value = (r.freq_hz/1e6).toFixed(3).replace(/\.?0+$/,'');
  for(const o of $('rf_bw').options){ if(Math.round(parseFloat(o.value)*1000)===r.bw_hz){ $('rf_bw').value=o.value; break; } }
  $('rf_sf').value  = String(r.sf);
  $('rf_cr').value  = String(r.cr);
  $('rf_sync').value= r.sync.toString(16).toUpperCase().padStart(2,'0');
  $('rf_pre').value = String(r.preamble);
  $('rf_pwr').value = String(r.power_dbm); $('rf_pwr_val').textContent = r.power_dbm;
}
function formRf(){
  return {
    freq_hz: Math.round(parseFloat($('rf_freq').value)*1e6),
    bw_hz:   Math.round(parseFloat($('rf_bw').value)*1000),
    sf:+$('rf_sf').value, cr:+$('rf_cr').value, power_dbm:+$('rf_pwr').value,
    sync: parseInt($('rf_sync').value||'0',16), preamble:+$('rf_pre').value,
  };
}

prov.onLine = t => {
  plog(t);
  let m;
  // Node id = the full 32-hex from id=/node=/registered…at; fall back to the ALN- adv name.
  if ((m=t.match(/(?:\bid=|node[= ]|registered.* at )([0-9A-F]{8,64})/i)) || (m=t.match(/ALN-([0-9A-F]{8,64})/i))) {
    provNodeId = m[1].toUpperCase(); $('provNode').textContent = provNodeId; $('provNode').className='pill ok';
  }
  if ((m=t.match(/^fw[= ](\S+)/))) $('provFw').textContent='fw '+m[1];
  if ((m=t.match(/PIN=(\d{6})/))) $('pin').textContent=m[1].split('').join(' ');
  // friendly name: `name: <v>` / `name set: <v>`; `(none …)`/cleared => blank
  if ((m=t.match(/^name(?: set)?: (.+)$/))) $('name_in').value = m[1].startsWith('(none') ? '' : m[1];
  if (/^name cleared/.test(t)) $('name_in').value='';
  // mobility: `mobile on/off …` reply or `… mobile=on/off` info line
  if ((m=t.match(/^mobile (on|off)/))||(m=t.match(/mobile=(on|off)/)))
    $('mob_val').textContent = m[1]==='on' ? '🚗 mobile (reserve band, raise fast / trim slow)' : '📍 fixed (power optimised)';
  // battery
  if ((m=t.match(/^batt raw=(\d+) mv=(\d+) pct=(\d+)/))) $('batt_val').textContent=m[2]+' mV — '+m[3]+'%  (raw '+m[1]+')';
  else if ((m=t.match(/^batt raw=(\d+) UNCALIBRATED/))) $('batt_val').textContent='raw '+m[1]+' — UNCALIBRATED';
  else if ((m=t.match(/^batt mv=(\d+) pct=(\d+)/))) $('batt_val').textContent=m[1]+' mV — '+m[2]+'%';
  if ((m=t.match(/batt=(\d+)mV\/(\d+)%/))) $('batt_val').textContent=m[1]+' mV — '+m[2]+'%';
  // BLE advertising state
  if ((m=t.match(/adv(?:ertising)?=([01])/))){ const on=m[1]==='1';
    $('advstate').textContent='BLE: '+(on?'ON — advertising as ALN-…':'off'); if(on) mark('ck-pin','ok','✓ BLE on'); }
  if (/BLE PIN=\d{6}/.test(t)) mark('ck-pin','ok','✓ PIN set, BLE on');
  // radio config:  [rf] freq_hz=… bw_hz=… sf=… cr=… power_dbm=… sync=0x4D preamble=… (active)
  if (t.startsWith('[rf]') && t.includes('(active)')) {
    const g=k=>{ const mm=t.match(new RegExp(k+'=(-?(?:0x)?[0-9A-Fa-f]+)')); return mm?mm[1]:null; };
    const r={ freq_hz:+g('freq_hz'), bw_hz:+g('bw_hz'), sf:+g('sf'), cr:+g('cr'),
              power_dbm:+g('power_dbm'), sync:parseInt(g('sync'),16), preamble:+g('preamble') };
    if(!isNaN(r.freq_hz)){ loadedRf=r; fillRf(r); }
  }
  // controller key
  if (/ctrlkey set/i.test(t)) mark('ck-key','ok','✓ provisioned');
  if (/none \(control/i.test(t)) mark('ck-key','bad','no key on node');
  if ((m=t.match(/^ctrlkey ([0-9A-F]{2})([0-9A-F]{2})\.\.([0-9A-F]{2})([0-9A-F]{2}) counter=(\d+)/i)) && ctrlKeys) {
    const pk = ctrlKeys.pubHex;
    const match = (m[1]+m[2]).toUpperCase()===pk.slice(0,4) && (m[3]+m[4]).toUpperCase()===pk.slice(-4);
    mark('ck-key', match?'ok':'bad', match ? '✓ matches this browser (ctr '+m[5]+')' : '✗ DIFFERENT key — re-provision');
    plog(match ? 'verified: node holds THIS browser’s controller key.'
               : 'MISMATCH: node holds a different key — click “Provision controller key” again.');
  }
};

async function provConnect(kind) {
  $('provLog').textContent='';
  try {
    const label = await (kind==='usb'?prov.serial():prov.ble()); $('provNode').textContent=label;
    provConnected = true; plog('connected — reading node…');
    setTimeout(()=>prov.send('info'), 400);      // fw + node id + mesh state
    setTimeout(()=>prov.send('rf show'), 800);    // radio PHY -> fills the form
    setTimeout(()=>prov.send('ble'), 1200);       // BLE state + PIN
    setTimeout(()=>prov.send('ctrlkey'), 1600);   // is a controller key already present
    setTimeout(()=>prov.send('name'), 2000);      // friendly name
    setTimeout(()=>prov.send('mobile'), 2400);    // mobility flag
    setTimeout(()=>prov.send('batt'), 2800);      // battery reading
  } catch(e){ plog('connect failed: '+e); }
}
function provDisconnect(){ prov.close(); provNodeId=null; provConnected=false;
  $('provNode').textContent='not connected'; $('provNode').className='pill'; $('provFw').textContent='fw —';
  for (const id of ['ck-pin','ck-key']) mark(id,'','pending'); }
function needConn(){ if(!provConnected){ plog('⚠ connect to the node first (USB or BLE)'); return false; } return true; }

async function applyRf(){
  if(!needConn()) return;
  const r=formRf();
  if(isNaN(r.freq_hz)||isNaN(r.sync)){ plog('radio: bad freq/sync value'); return; }
  const crit = !loadedRf || r.freq_hz!==loadedRf.freq_hz || r.bw_hz!==loadedRf.bw_hz || r.sf!==loadedRf.sf;
  if(crit && !confirm(
      'This changes a network-wide PHY parameter (frequency/bandwidth/SF).\n\n'+
      'This node will RETUNE and stop hearing any node still on the old settings.\n'+
      'Only proceed if every node will be moved to these settings.\n\nApply anyway?')) return;
  const hex=r.sync.toString(16).toUpperCase().padStart(2,'0');
  await prov.send('rf freq '+r.freq_hz);
  await prov.send('rf bw '+$('rf_bw').value);
  await prov.send('rf sf '+r.sf);
  await prov.send('rf cr '+r.cr);
  await prov.send('rf power '+r.power_dbm);
  await prov.send('rf sync 0x'+hex);
  await prov.send('rf preamble '+r.preamble);
  await prov.send('rf apply');
  setTimeout(()=>prov.send('rf show'), 400);
  plog('radio: staged + apply sent'+(crit?' (RETUNE)':' (power/minor)'));
}

// connection
$('provConnectUsb').onclick=()=>provConnect('usb');
$('provConnectBle').onclick=()=>provConnect('ble');
// controller key
$('provKey').onclick=async()=>{ if(!needConn())return; if(!ctrlKeys){plog('no controller key');return;}
  mark('ck-key','warn','setting…'); await prov.send('ctrlkey '+ctrlKeys.pubHex);
  plog('→ sent ctrlkey '+ctrlKeys.pubHex.slice(0,8)+'… (verifying)');
  setTimeout(()=>prov.send('ctrlkey'), 600); };
// node name
$('name_set').onclick=()=>{ if(!needConn())return; const v=$('name_in').value.trim(); if(v) prov.send('name '+v); };
$('name_clear').onclick=()=>{ if(!needConn())return; prov.send('name clear'); $('name_in').value=''; };
// BLE pairing
$('enable').onclick=()=>{ if(needConn()) prov.send('ble on'); };
$('disable').onclick=()=>{ if(needConn()) prov.send('ble off'); };
$('newpin').onclick=()=>{ if(needConn()) prov.send('blepin random'); };
$('setbtn').onclick=()=>{ if(!needConn())return; const p=$('setpin').value; if(/^\d{6}$/.test(p)) prov.send('blepin '+p); else plog('PIN must be 6 digits'); };
// radio
$('rf_pwr').oninput=()=>$('rf_pwr_val').textContent=$('rf_pwr').value;
$('rf_apply').onclick=applyRf;
$('rf_reload').onclick=()=>{ if(needConn()) prov.send('rf show'); };
// battery
$('batt_read').onclick=()=>{ if(needConn()) prov.send('batt'); };
$('batt_cal').onclick=()=>{ if(!needConn())return; let v=+$('batt_mv').value;
  if(v>0&&v<10) v=Math.round(v*1000);                       // volts entered (4.02) -> mV
  if(v>=1000&&v<=6000) prov.send('batt cal '+v); else plog('enter measured battery (volts e.g. 4.02, or mV e.g. 4020)'); };
// mobility
$('mob_fixed').onclick=()=>{ if(needConn()) prov.send('mobile off'); };
$('mob_mobile').onclick=()=>{ if(needConn()) prov.send('mobile on'); };
// console
$('cmdsend').onclick=()=>{ if(!needConn())return; const c=$('cmd').value.trim(); if(c){ prov.send(c); $('cmd').value=''; } };
$('cmd').addEventListener('keydown',e=>{ if(e.key==='Enter') $('cmdsend').onclick(); });
// verify: re-read everything that should have stuck
$('verifyProv').onclick=async()=>{ if(!needConn())return; plog('— verifying —');
  await prov.send('ctrlkey'); await prov.send('ble'); await prov.send('rf show');
  await prov.send('name'); await prov.send('mobile'); };
