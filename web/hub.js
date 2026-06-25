// hub.js — logic for the commissioning hub (flash.html).
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

function showTab(t) {
  for (const x of ['flash','config']) {
    $('tab-'+x).classList.toggle('hide', x !== t);
    $('nav-'+x).classList.toggle('on', x === t);
  }
}
function log(m) { const el=$('log'); el.textContent=(el.textContent+m+'\n').split('\n').slice(-200).join('\n'); el.scrollTop=el.scrollHeight; }
function fwBaseUrl() { return ($('fwBase').value.trim() || './fw/'); }

// ---------- shared controller key (same localStorage key as map.html) ----------
let ctrlKeys = null;
const CTRL_DOMAIN = new TextEncoder().encode('AGN-CTRL-1');
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
      if (b===0x7E) { inFrame=!inFrame; continue; }
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
      dev = await navigator.bluetooth.requestDevice({filters:[{namePrefix:'AgnLoRa'}], optionalServices:[NUS]});
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

// ---------- provisioning ----------
let prov = makeConn(), provNodeId = null, provConnected = false;
function plog(m){ const el=$('provLog'); el.textContent=(el.textContent+m+'\n').split('\n').slice(-200).join('\n'); el.scrollTop=el.scrollHeight; }
function mark(id, state, text){ const e=$(id); e.textContent=text; e.className='pill'+(state?' '+state:''); }
prov.onLine = t => {
  plog(t);
  let m;
  if ((m=t.match(/node[= ]([0-9A-F]{8})/i)) || (m=t.match(/AgnLoRa-([0-9A-F]{8})/i))) {
    provNodeId = m[1].toUpperCase(); $('provNode').textContent = provNodeId; $('provNode').className='pill ok';
  }
  // pre-fill the radio fields from the node's actual settings (was always 22)
  if ((m=t.match(/\[rf\].*sf=(\d+).*power_dbm=(-?\d+).*\(active\)/))) {
    $('sf').value = m[1]; $('pwr').value = m[2]; mark('ck-rf','ok','current: SF'+m[1]+' / '+m[2]+' dBm');
  }
  // PIN / BLE
  if (/BLE PIN=\d{6}/.test(t) || /advertising=1/.test(t)) mark('ck-pin','ok','✓ PIN set, BLE on');
  // controller-key confirmations
  if (/ctrlkey set/i.test(t)) mark('ck-key','ok','✓ provisioned');
  if (/none \(control/i.test(t)) mark('ck-key','bad','no key on node');
  // read-back: `ctrlkey AABB..YYZZ counter=N` — compare to THIS browser's key
  if ((m=t.match(/^ctrlkey ([0-9A-F]{2})([0-9A-F]{2})\.\.([0-9A-F]{2})([0-9A-F]{2}) counter=(\d+)/i)) && ctrlKeys) {
    const pk = ctrlKeys.pubHex;
    const match = (m[1]+m[2]).toUpperCase()===pk.slice(0,4) && (m[3]+m[4]).toUpperCase()===pk.slice(-4);
    mark('ck-key', match?'ok':'bad', match ? '✓ matches this browser (counter '+m[5]+')' : '✗ DIFFERENT key — re-provision');
    plog(match ? 'verified: node holds THIS browser’s controller key.'
               : 'MISMATCH: node holds a different key — click “Provision controller key” again.');
  }
};
async function provConnect(kind) {
  $('provLog').textContent='';
  try { const label = await (kind==='usb'?prov.serial():prov.ble()); $('provNode').textContent=label;
    provConnected = true; plog('connected — reading node…');
    setTimeout(()=>prov.send('info'), 400);
    setTimeout(()=>prov.send('rf show'), 800);     // pre-fill radio fields
    setTimeout(()=>prov.send('ctrlkey'), 1200);    // show whether a key is already present
  } catch(e){ plog('connect failed: '+e); }
}
function provDisconnect(){ prov.close(); provNodeId=null; provConnected=false;
  for (const id of ['ck-pin','ck-key']) mark(id,'','pending'); mark('ck-rf','','unchanged'); }
function needConn(){ if(!provConnected){ plog('⚠ connect to the node first (USB or BLE)'); return false; } return true; }
$('provConnectUsb').onclick=()=>provConnect('usb');
$('provConnectBle').onclick=()=>provConnect('ble');
$('setPin').onclick=async()=>{ if(!needConn())return; const p=$('pin').value.trim(); if(!/^\d{6}$/.test(p)){plog('PIN must be 6 digits');return;}
  mark('ck-pin','warn','setting…'); await prov.send('blepin '+p); await prov.send('ble on'); plog('→ sent blepin + ble on'); };
$('provKey').onclick=async()=>{ if(!needConn())return; if(!ctrlKeys){plog('no controller key');return;}
  mark('ck-key','warn','setting…'); await prov.send('ctrlkey '+ctrlKeys.pubHex);
  plog('→ sent ctrlkey '+ctrlKeys.pubHex.slice(0,8)+'… (verifying)');
  setTimeout(()=>prov.send('ctrlkey'), 600);   // read it back -> match/mismatch shown above
};
$('applyRf').onclick=async()=>{ if(!needConn())return; mark('ck-rf','warn','applying…');
  await prov.send('rf sf '+$('sf').value); await prov.send('rf power '+$('pwr').value);
  await prov.send('rf apply'); await prov.send('rf show'); plog('→ sent rf sf/power/apply'); };
// Re-read the node and confirm what actually stuck.
$('verifyProv').onclick=async()=>{ if(!needConn())return; plog('— verifying —');
  await prov.send('ctrlkey'); await prov.send('ble'); await prov.send('rf show'); };

// ---------- configure tab ----------
let cfg = makeConn();
cfg.onLine = t => { const el=$('cfgLog'); el.textContent=(el.textContent+t+'\n').split('\n').slice(-300).join('\n'); el.scrollTop=el.scrollHeight;
  let m; if((m=t.match(/node[= ]([0-9A-F]{8})/i))){ $('cfgNode').textContent=m[1].toUpperCase(); $('cfgNode').className='pill ok'; } };
async function cfgConnect(kind){ try{ const l=await(kind==='usb'?cfg.serial():cfg.ble()); $('cfgNode').textContent=l;
  $('cfgCmd').disabled=$('cfgSend').disabled=false; setTimeout(()=>cfg.send('info'),400);}catch(e){$('cfgLog').textContent+='connect: '+e+'\n';} }
$('cfgUsb').onclick=()=>cfgConnect('usb');
$('cfgBle').onclick=()=>cfgConnect('ble');
$('cfgSend').onclick=()=>{ const v=$('cfgCmd').value.trim(); if(v){cfg.send(v);$('cfgCmd').value='';} };
$('cfgCmd').addEventListener('keydown',e=>{ if(e.key==='Enter')$('cfgSend').click(); });
