// main.cpp — agnostic-LoRa-Net node firmware (Agent.md §6/§7).
//
// Each node beacons periodically; every beacon carries the node's announce
// (neighbour reports + distance-vector table), and every received frame's RSSI/SNR
// feeds the portable routing core (lib/mesh). Nodes build a live per-direction
// neighbour/route table over the air; DATA packets are delivered, forwarded toward
// next_hop(), or dropped (dedup / TTL / no-route) by the relay engine, with
// hop-by-hop ARQ and application-layer SAR on top.
//
// Northbound, a host can drive the mesh as an opaque transport: a USB serial console
// + a binary HDLC `tunnel` (used by the Reticulum interface), and — with -DAGN_BLE —
// a PIN-paired BLE Nordic UART Service that tunnels app frames in/out (Req 1: BLE +
// LoRa coexist because the radio core never blocks). The routing/codec/relay/ARQ/SAR
// logic is unit-tested host-side (test/*).

#include <Arduino.h>
#include "board_config.h"
#include "packet.h"
#include "radio_hal.h"
#include "router.h"
#include "link_metric.h"
#include "announce_codec.h"
#include "forwarder.h"
#include "link_arq.h"
#include "sar.h"
#include "locator_dir.h"
// Persistent config (radio PHY + BLE) lives in LittleFS on internal flash — always
// compiled in (radio config is not BLE-specific).
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
#ifdef AGN_BLE
#include <bluefruit.h>     // nRF52 SoftDevice BLE — Req 1 coexistence experiment
#endif

#ifndef AGN_FW_VERSION
#  define AGN_FW_VERSION "dev"
#endif
#ifndef AGN_NODE_ID
#  define AGN_NODE_ID 0
#endif

static RadioHal        radio;
static node_id_t       my_id;
static mesh::Router*   router    = nullptr;  // constructed in setup() once my_id is known
static mesh::Forwarder* forwarder = nullptr;
static mesh::LinkArq   arq;                  // hop-by-hop ACK + retry
static uint32_t        next_arq_ms = 0;
static const uint32_t  ARQ_TICK_MS = 250;    // how often to check for retransmits
static uint16_t        beacon_seq     = 0;
static uint16_t        data_seq       = 0;
static uint32_t        next_beacon_ms = 0;
static uint32_t        next_tick_ms   = 0;
static uint32_t        boot_ms        = 0;

// --- SAR file transfer state (the "app" riding on the backbone) ---
static uint8_t   sar_buf[mesh::SAR_MAX_FILE];   // outbound load buffer
static uint32_t  sar_len      = 0;              // bytes loaded
static uint32_t  sar_crc      = 0;              // expected CRC of the load
static uint16_t  sar_xfer_id  = 0;              // increments per transfer
static bool      sar_tx_active = false;
static node_id_t sar_tx_dst   = 0;
static uint16_t  sar_tx_idx   = 0;
static uint16_t  sar_tx_count = 0;
static mesh::SarReassembler sar_rx;             // inbound reassembly
static uint32_t  sar_quiet_until = 0;           // suppress our beacons during a transfer
// Tunnel mode: USB serial becomes a binary HDLC pipe carrying [u32 node][payload],
// so a host bridge (e.g. a Reticulum interface) can push/pull opaque app packets
// through the mesh. Enabled by the `tunnel` console command.
static bool      tunnel_mode = false;
static const uint8_t HDLC_FLAG = 0x7E, HDLC_ESC = 0x7D, HDLC_ESC_MASK = 0x20;
// Host-tunnel frame envelope (host<->node), TYPED + LENGTH-PREFIXED so the address can
// widen (4-byte locator today -> 16-byte node-pubkey hash later, identity-vs-locator §6)
// with no flag day, and an identity-addressed mode can be added as a type, not a rewrite:
//   frame body := [u8 addr_type][u8 addr_len][addr bytes…][payload…]
static const uint8_t TUN_ADDR_LOCATOR  = 0x01;   // addr = node-id locator (the only live type)
static const uint8_t TUN_ADDR_IDENTITY = 0x02;   // reserved: resolve-and-forward (deferred)
static void tunnel_emit(node_id_t src, const uint8_t* payload, uint16_t plen);  // fwd decl
static void tunnel_rx_frame(const uint8_t* f, uint16_t n);                       // fwd decl

// The console (rf/ble/info/help/…) can be driven over USB serial OR, when a paired
// client is connected, over the BLE NUS — so a node can be reconfigured in the field
// over BLE (the retune-rescue path of docs/remote-config.md). Command *output* follows
// the source: g_con points at whichever transport the current command arrived on.
// handle_command()/print_info()/rf_print() locally shadow `Serial` with *g_con so all
// their prints route there; console_exec() sets/restores it per line.
static Print* g_con = &Serial;
static void handle_command(char* line);            // fwd decl (console dispatcher)
static void console_exec(char* line, Print* out);  // fwd decl (run one line, route output)

// --- persistent radio PHY config (freq/BW/SF/CR/power/sync/preamble) ------------
// Stored in LittleFS, separate from the BLE config. `rf_work` is the staging copy
// the console edits; `rf apply` commits it to the radio + flash. Save-if-dirty
// (read-back + byte-compare) honours Agent.md Req 4 (don't churn nRF52 flash).
// These same fields are what the future authenticated remote-config control plane
// will set over LoRa (see docs/remote-config.md).
static const char     RF_PATH[]  = "/agn_rf.cfg";
static const uint32_t RF_MAGIC   = 0x46524741;   // "AGRF"
static RadioCfg       rf_work;                   // staged edits (committed by `rf apply`)
struct __attribute__((packed)) RfStore { uint32_t magic; RadioCfg cfg; };

static bool rf_read(RadioCfg& out) {
    RfStore rec;
    File f(InternalFS);
    if (!f.open(RF_PATH, FILE_O_READ)) return false;
    int n = f.read((void*)&rec, sizeof(rec));
    f.close();
    if (n != (int)sizeof(rec) || rec.magic != RF_MAGIC) return false;
    out = rec.cfg;
    return true;
}

static void rf_save(const RadioCfg& c) {
    RadioCfg cur;
    if (rf_read(cur) && memcmp(&cur, &c, sizeof(RadioCfg)) == 0) return;  // unchanged -> no write
    RfStore want; want.magic = RF_MAGIC; want.cfg = c;
    InternalFS.remove(RF_PATH);
    File f(InternalFS);
    if (f.open(RF_PATH, FILE_O_WRITE)) { f.write((const uint8_t*)&want, sizeof(want)); f.close(); }
}

// Load the persisted PHY at boot, or fall back to the compile-time network defaults.
static RadioCfg rf_load() {
    InternalFS.begin();   // idempotent; BLE config shares the same FS
    RadioCfg c;
    if (rf_read(c)) return c;
    return radio_default_config();
}

static void rf_print(const RadioCfg& c, const char* tag) {
    Print& Serial = *g_con;   // route to the active console sink (USB or BLE)
    char m[120];
    snprintf(m, sizeof(m),
        "[rf] freq_hz=%lu bw_hz=%lu sf=%u cr=%u power_dbm=%d sync=0x%02X preamble=%u (%s)",
        (unsigned long)c.freq_hz, (unsigned long)c.bw_hz,
        (unsigned)c.sf, (unsigned)c.cr, (int)c.power_dbm, (unsigned)c.sync,
        (unsigned)c.preamble, tag);
    Serial.println(m);
}
#ifdef AGN_BLE
static volatile bool ble_connected = false;
static uint32_t  ble_rxb = 0, ble_txb = 0;
#endif

// A host transport (USB tunnel or BLE) is attached and wants opaque app frames.
static inline bool host_tunnel_active() {
#ifdef AGN_BLE
    if (ble_connected) return true;
#endif
    return tunnel_mode;
}

#ifdef AGN_BLE
// --- BLE peripheral (Nordic UART Service) -----------------------------------
// A phone/Web-Bluetooth client connects over BLE; we tunnel its app frames into
// the LoRa mesh and deliver mesh packets back out over BLE. So the full path is
// webapp -> BLE -> LoRa mesh -> BLE -> webapp, with the node also proving Req 1
// (BLE link stays up while the LoRa radio is busy).
static BLEUart   bleuart;
static char      ble_pin[7]      = "000000";   // 6-digit pairing PIN
static bool      ble_advertising = false;
static bool      ble_inited      = false;      // SoftDevice/BLE stack brought up yet?

static void ble_connect_cb(uint16_t)            { ble_connected = true; }
static void ble_disconnect_cb(uint16_t, uint8_t){ ble_connected = false; }

// Generate a fresh 6-digit pairing PIN.
static void ble_gen_pin() {
    randomSeed((uint32_t)micros() ^ my_id ^ (uint32_t)(millis() << 3));
    snprintf(ble_pin, sizeof(ble_pin), "%06lu", (unsigned long)random(0, 1000000));
}

// --- persistent config (PIN + BLE-enabled) in internal flash ----------------
// Stored in LittleFS on the nRF52's internal flash. Following Agent.md Req 4
// (nRF52 flash is ~10k erase cycles/page — a sloppy write loop can brick a solar
// node), we only write on a real change: read-back + byte-equal compare first.
static const char     CFG_PATH[]  = "/agn_ble.cfg";
static const uint32_t CFG_MAGIC   = 0x314E4741;   // "AGN1"
static bool           cfg_first_boot  = false;
static bool           cfg_ble_enabled = false;
struct __attribute__((packed)) AgnBleCfg { uint32_t magic; char pin[7]; uint8_t enabled; };

static bool cfg_read(AgnBleCfg& out) {
    File f(InternalFS);
    if (!f.open(CFG_PATH, FILE_O_READ)) return false;
    int n = f.read((void*)&out, sizeof(out));
    f.close();
    return n == (int)sizeof(out) && out.magic == CFG_MAGIC;
}

static void cfg_save() {
    AgnBleCfg want = {};
    want.magic = CFG_MAGIC;
    strncpy(want.pin, ble_pin, 6); want.pin[6] = 0;
    want.enabled = ble_advertising ? 1 : 0;
    AgnBleCfg cur;
    if (cfg_read(cur) && memcmp(&cur, &want, sizeof(want)) == 0) return;  // unchanged -> no write
    InternalFS.remove(CFG_PATH);
    File f(InternalFS);
    if (f.open(CFG_PATH, FILE_O_WRITE)) { f.write((const uint8_t*)&want, sizeof(want)); f.close(); }
}

// Load PIN + enabled state at boot. On the very first boot (no config yet) a stable
// PIN is generated and persisted (below, once the SoftDevice is up), so it no longer
// changes every reboot.
static void cfg_load() {
    InternalFS.begin();
    AgnBleCfg c;
    if (cfg_read(c)) { strncpy(ble_pin, c.pin, 6); ble_pin[6] = 0; cfg_ble_enabled = c.enabled; }
    else { ble_gen_pin(); cfg_first_boot = true; }
}

static void ble_setup();   // fwd decl — start_adv brings the stack up on demand

static void ble_start_adv() {
    ble_setup();                          // lazily enable the SoftDevice on first use
    Bluefruit.Security.setPIN(ble_pin);   // (re)apply the current PIN before pairing
    Bluefruit.Advertising.start(0);       // 0 = advertise until connected
    ble_advertising = true;
}
static void ble_stop_adv() {
    if (!ble_inited) { ble_advertising = false; return; }   // never enabled — nothing to stop
    Bluefruit.Advertising.stop();
    if (ble_connected) Bluefruit.disconnect(Bluefruit.connHandle());
    ble_advertising = false;
}

// Init the SoftDevice + a PIN-secured UART, but DON'T advertise yet — BLE is turned
// on per node, on demand, from the management console (`ble on`). Lazy + idempotent:
// the SoftDevice is only enabled the first time BLE is actually turned on, so a node
// that never uses BLE pays no runtime/power cost (Req 4) — just the flash it occupies.
static void ble_setup() {
    if (ble_inited) return;
    ble_inited = true;
    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);   // must precede begin()
    Bluefruit.begin();                              // start the SoftDevice (before radio.begin)
    Bluefruit.setTxPower(4);
    char nm[24]; snprintf(nm, sizeof(nm), "AgnLoRa-%08lX", (unsigned long)my_id);
    Bluefruit.setName(nm);
    Bluefruit.Security.setPIN(ble_pin);             // static passkey -> phone must enter it
    Bluefruit.Periph.setConnectCallback(ble_connect_cb);
    Bluefruit.Periph.setDisconnectCallback(ble_disconnect_cb);
    bleuart.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);  // require pairing
    bleuart.begin();
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(bleuart);
    Bluefruit.ScanResponse.addName();
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 244);
    Bluefruit.Advertising.setFastTimeout(30);
    // advertising started later via `ble on`
}

// Service the BLE NUS. The same characteristic carries two channels, told apart with
// zero ambiguity because HDLC frames are 0x7E-delimited and ASCII console text never
// contains 0x7E:
//   * HDLC frames ([u32 dst][payload])  -> the mesh tunnel (web/ble.html, Reticulum).
//   * plain text lines ending in '\n'   -> the management console (config-over-BLE),
//     with responses sent back over BLE. This is the field-config / retune-rescue path.
static void ble_poll() {
    if (!ble_inited) return;   // stack not up (BLE never enabled) — nothing to service
    static uint8_t  bf[4 + MAX_PAYLOAD];   // HDLC tunnel frame
    static uint16_t bl = 0;
    static bool     inf = false, esc = false;
    static char     cl[160];               // text console-line accumulator
    static uint16_t cn = 0;
    while (bleuart.available()) {
        uint8_t c = (uint8_t)bleuart.read();
        ble_rxb++;
        if (c == HDLC_FLAG) {                       // frame boundary -> tunnel channel
            if (inf) { inf = false; if (bl > 0) tunnel_rx_frame(bf, bl); }
            else     { inf = true; bl = 0; esc = false; }
            cn = 0;                                 // never part of a text line
            continue;
        }
        if (inf) {                                  // inside an HDLC tunnel frame
            if (bl < sizeof(bf)) {
                if (c == HDLC_ESC) esc = true;
                else { bf[bl++] = esc ? (c ^ HDLC_ESC_MASK) : c; esc = false; }
            }
            continue;
        }
        // Outside any frame: accumulate a text console line (config-over-BLE).
        if (c == '\r') continue;
        if (c == '\n') {
            if (cn > 0) { cl[cn] = 0; cn = 0; console_exec(cl, &bleuart); }
        } else if (cn < sizeof(cl) - 1) {
            cl[cn++] = (char)c;
        }
    }
}
#endif // AGN_BLE
// resend queue (sender side, fed by NACKs)
static uint16_t  sar_resend[mesh::SAR_MAX_FRAGS];
static uint16_t  sar_resend_n   = 0;
static uint16_t  sar_resend_idx = 0;
static bool      sar_resend_active = false;
// missing-fragment request (receiver side)
static node_id_t sar_rx_sender   = 0;
static uint32_t  sar_rx_last_ms  = 0;
static uint16_t  sar_rx_last_xfer = 0xFFFF;
static uint8_t   sar_nack_rounds = 0;
static const uint32_t SAR_NACK_TIMEOUT_MS = 6000;   // sender quiet this long => ask for missing
static const uint8_t  SAR_MAX_NACK_ROUNDS = 6;

// Cadence for ageing out dead neighbours/routes.
static const uint32_t TICK_PERIOD_MS = 5000;

// Beacon cadence. Airtime is the scarcest resource (§2.4): one short beacon every
// ~10 s, with per-node jitter so two nodes don't lock-step and collide forever.
static const uint32_t BEACON_PERIOD_MS = 10000;
static const uint32_t BEACON_JITTER_MS = 2500;

// Provisional 4-byte node ID. The real ID is derived from the node's public key
// (self-certifying, §3); until crypto lands we use a per-chip stable value from
// the nRF52 FICR DEVICEID, or an explicit build-time override (-DAGN_NODE_ID=...).
static node_id_t derive_node_id() {
    if ((uint32_t)AGN_NODE_ID != 0u) return (node_id_t)AGN_NODE_ID;
#if defined(NRF_FICR)
    return (node_id_t)(NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1]);
#else
    return 0xDEADBEEFu;  // non-nRF target (e.g. host tooling) — fixed placeholder
#endif
}

static uint32_t schedule_next_beacon() {
    return millis() + BEACON_PERIOD_MS + (uint32_t)random(0, BEACON_JITTER_MS);
}

// --- Outbound TX queue -----------------------------------------------------
// The radio sends one frame at a time (non-blocking). Beacons, data, forwards,
// ACKs and ARQ retransmits are all TX sources, so they enqueue here and a single
// pump drains to the radio whenever it's free. Keeps every path non-blocking and
// stops two sends from colliding mid-air.
static const uint8_t TXQ_DEPTH = 6;
struct TxItem { uint16_t len; uint8_t frame[1 + MAX_PAYLOAD]; };
static TxItem  txq[TXQ_DEPTH];
static uint8_t txq_head = 0, txq_tail = 0, txq_count = 0;

static bool txq_push(const uint8_t* frame, uint16_t len) {
    if (txq_count >= TXQ_DEPTH || len > sizeof(txq[0].frame)) return false;
    txq[txq_tail].len = len;
    memcpy(txq[txq_tail].frame, frame, len);
    txq_tail = (uint8_t)((txq_tail + 1) % TXQ_DEPTH);
    txq_count++;
    return true;
}

static void txq_pump() {
    if (txq_count == 0 || radio.busy()) return;
    if (radio.send(txq[txq_head].frame, txq[txq_head].len)) {
        txq_head = (uint8_t)((txq_head + 1) % TXQ_DEPTH);
        txq_count--;
    }
}

// ARQ retransmit callback — re-enqueue the stored frame bytes verbatim.
static void arq_resend(void* /*ctx*/, const uint8_t* frame, uint16_t len) {
    txq_push(frame, len);
}

// Send a frame addressed to a specific next hop. If the link header carries a real
// alias (not broadcast), stamp a link sequence + ACK request and hand it to the ARQ
// engine for retransmit; broadcast fallback frames go out unreliably.
static void tx_unicast(uint8_t* frame, uint16_t len, node_id_t next_hop) {
    LinkHeader link;
    memcpy(&link, frame, sizeof(link));
    if (link.next_hop != LINK_ADDR_BROADCAST) {
        uint8_t seq = arq.next_seq();
        link.link_seq = seq;
        link.flags   |= LINK_FLAG_ACK_REQ;
        memcpy(frame, &link, sizeof(link));
        txq_push(frame, len);
        arq.track(seq, next_hop, frame, len, millis());
    } else {
        txq_push(frame, len);
    }
}

// Send a tiny link-layer ACK for `seq` back to the previous hop `to`.
static void send_ack(node_id_t to, uint8_t seq) {
    uint8_t frame[HEADER_BYTES];
    link_addr_t na = router ? router->link_addr_for(to) : LINK_ADDR_NONE;

    LinkHeader link;
    link.prev_hop = router ? router->my_alias_for(to) : LINK_ADDR_NONE;
    link.next_hop = (na != LINK_ADDR_NONE) ? na : LINK_ADDR_BROADCAST;
    link.link_seq = seq;                 // echo the sequence being acknowledged
    link.flags    = LINK_FLAG_IS_ACK;

    NetHeader net;
    net.ver_type = net_ver_type(PKT_ACK);
    net.flags    = 0;
    net.ttl      = 1;                    // link-local, never relayed
    net.dst      = to;
    net.src      = my_id;
    net.pkt_id   = seq;

    memcpy(frame, &link, sizeof(link));
    memcpy(frame + sizeof(link), &net, sizeof(net));
    txq_push(frame, sizeof(frame));
}

// Assemble and transmit one beacon frame: LinkHeader + NetHeader + BeaconPayload.
static void send_beacon() {
    // Headers + BeaconPayload + the serialised routing announce (neighbour reports
    // + DV table). Sized to the radio's frame ceiling; the codec only fills what
    // fits (airtime budget, §2.4).
    uint8_t frame[1 + MAX_PAYLOAD];
    const uint16_t base = HEADER_BYTES + sizeof(BeaconPayload);

    LinkHeader link;
    link.prev_hop = LINK_ADDR_NONE;        // aliases negotiated per link in Phase 2
    link.next_hop = LINK_ADDR_BROADCAST;   // beacon = to every neighbour
    link.link_seq = (uint8_t)beacon_seq;
    link.flags    = 0;

    NetHeader net;
    net.ver_type = net_ver_type(PKT_BEACON);
    net.flags    = 0;
    net.ttl      = 1;                       // beacons are single-hop (neighbour discovery)
    net.dst      = NODE_ID_BROADCAST;
    net.src      = my_id;
    net.pkt_id   = beacon_seq;

    BeaconPayload pl;
    pl.hw_class = 0;                         // 0 = RAK4631
    pl.reserved = 0;
    pl.uptime_s = (uint16_t)((millis() - boot_ms) / 1000u);

    memcpy(frame, &link, sizeof(link));
    memcpy(frame + sizeof(link), &net, sizeof(net));
    memcpy(frame + HEADER_BYTES, &pl, sizeof(pl));

    // Piggyback this node's announce so distance-vector + reverse-link quality
    // propagate (Agent.md §6).
    uint16_t ann_len = 0;
    if (router) {
        mesh::Announce ann;
        router->build_announce(ann);
        ann_len = mesh::announce_serialize(ann, frame + base, (uint16_t)(sizeof(frame) - base));
    }
    const uint16_t frame_len = base + ann_len;

    if (txq_push(frame, frame_len)) {
        char hdr[72];
        snprintf(hdr, sizeof(hdr), "[TX] beacon seq=%u from %08lX  +announce %uB",
                 (unsigned)beacon_seq, (unsigned long)my_id, (unsigned)ann_len);
        Serial.println(hdr);
        beacon_seq++;
    } else {
        Serial.println("[TX] queue full, beacon dropped");
    }
}

// Originate a unicast DATA packet toward `dst` carrying `payload` (opaque to the
// backbone, §2.5). `verbose` prints a per-packet TX line (off for bulk SAR sends).
static void send_data_bytes(node_id_t dst, const uint8_t* payload, uint16_t plen, bool verbose) {
    uint8_t  frame[1 + MAX_PAYLOAD];
    if (plen > MAX_PAYLOAD - HEADER_BYTES) plen = MAX_PAYLOAD - HEADER_BYTES;

    // Directed link addressing: stamp the next hop's alias so only it acts. The
    // next hop toward dst is a direct neighbour; if its alias isn't negotiated yet,
    // fall back to broadcast so the frame still propagates during convergence.
    node_id_t   nh = router ? router->next_hop(dst) : 0;
    link_addr_t na = (router && nh) ? router->link_addr_for(nh) : LINK_ADDR_NONE;
    LinkHeader  link;
    link.prev_hop = (router && nh) ? router->my_alias_for(nh) : LINK_ADDR_NONE;
    link.next_hop = (na != LINK_ADDR_NONE) ? na : LINK_ADDR_BROADCAST;
    link.link_seq = (uint8_t)data_seq;
    link.flags    = 0;

    NetHeader  net;
    net.ver_type = net_ver_type(PKT_DATA);
    net.flags    = 0;
    net.ttl      = DEFAULT_TTL;
    net.dst      = dst;
    net.src      = my_id;
    net.pkt_id   = data_seq;

    memcpy(frame, &link, sizeof(link));
    memcpy(frame + sizeof(link), &net, sizeof(net));
    memcpy(frame + HEADER_BYTES, payload, plen);

    if (forwarder) forwarder->mark_seen(my_id, data_seq);  // ignore our own rebroadcast

    if (verbose) {
        char hdr[88];
        snprintf(hdr, sizeof(hdr), "[TX] data  id=%u -> %08lX  (next hop %08lX alias %u)",
                 (unsigned)data_seq, (unsigned long)dst, (unsigned long)nh, (unsigned)link.next_hop);
        Serial.println(hdr);
    }
    tx_unicast(frame, HEADER_BYTES + plen, nh);   // ARQ-tracked when directed
    data_seq++;
}

static void send_data(node_id_t dst, const char* msg) {
    send_data_bytes(dst, (const uint8_t*)msg, (uint16_t)strlen(msg), true);
}

// Relay a received frame onward toward `next` with TTL decremented to `out_ttl`.
// Re-stamps the link header for the NEW next hop (its own alias, sequence + ACK).
static void forward_frame(const uint8_t* buf, uint16_t len, node_id_t next, uint8_t out_ttl) {
    uint8_t frame[1 + MAX_PAYLOAD];
    if (len > sizeof(frame)) return;
    memcpy(frame, buf, len);

    LinkHeader link;
    memcpy(&link, frame, sizeof(link));
    link_addr_t na = router ? router->link_addr_for(next) : LINK_ADDR_NONE;
    link.prev_hop = router ? router->my_alias_for(next) : LINK_ADDR_NONE;
    link.next_hop = (na != LINK_ADDR_NONE) ? na : LINK_ADDR_BROADCAST;
    link.flags    = 0;                       // tx_unicast sets ACK_REQ/seq for this hop
    memcpy(frame, &link, sizeof(link));

    NetHeader net;
    memcpy(&net, frame + sizeof(LinkHeader), sizeof(net));
    net.ttl = out_ttl;
    memcpy(frame + sizeof(LinkHeader), &net, sizeof(net));

    tx_unicast(frame, len, next);            // each hop runs its own ARQ
}

// --- distributed locator directory (Phase 2) ------------------------------------
// REGISTER/QUERY flood across the mesh, REPLY routes back by DV. The directory only
// answers "which node hosts identity X"; the mesh still routes on locators (node ids).
// See docs/distributed-lookup-plan.md / docs/identity-vs-locator.md.
static mesh::LocatorDir      locdir;
static mesh::LocatorResolver locres;
static uint16_t loc_epoch   = 0;            // per-boot nonce of this node as an id-owner
static uint16_t loc_seq     = 0;            // monotonic registration seq within this boot
static uint16_t loc_pktid   = 0;           // pkt_id space for loc packets we originate
static const uint16_t LOC_TTL_S      = 600;    // binding lifetime we advertise (s)
static const uint8_t  LOC_FLOOD_TTL  = 4;      // hop budget for REGISTER/QUERY floods
static const uint16_t LOC_RESOLVE_MS = 8000;   // give up on a QUERY after this
static const uint32_t LOC_REREG_MS   = 240000; // re-register cadence (< TTL, refresh)
static uint32_t next_rereg_ms = 0;

// Ids this node serves locally (a handful), re-registered periodically so they don't lapse.
struct LocReg { uint8_t id[mesh::LOC_ID_MAX]; uint8_t id_len; bool used; };
static LocReg my_regs[4];

// Dedup ring so a flooded REGISTER/QUERY isn't processed or re-flooded twice.
struct LocSeen { node_id_t src; uint16_t pid; bool used; };
static LocSeen loc_seen[16];
static uint8_t loc_seen_i = 0;
static bool loc_flood_seen(node_id_t src, uint16_t pid) {
    for (auto& s : loc_seen) if (s.used && s.src == src && s.pid == pid) return true;
    return false;
}
static void loc_flood_mark(node_id_t src, uint16_t pid) {
    loc_seen[loc_seen_i] = { src, pid, true };
    loc_seen_i = (uint8_t)((loc_seen_i + 1) % 16);
}
static void loc_id_hex(const uint8_t* id, uint8_t n, char* out) {
    for (uint8_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02X", id[i]);
    out[2 * n] = '\0';
}

// Broadcast a REGISTER/QUERY into the mesh (multi-hop flood, TTL-bounded).
static void send_loc_flood(const uint8_t* msg, uint16_t mlen, uint8_t ttl) {
    uint8_t frame[1 + MAX_PAYLOAD];
    if ((uint16_t)(HEADER_BYTES + mlen) > sizeof(frame)) return;
    LinkHeader link; link.prev_hop = LINK_ADDR_NONE; link.next_hop = LINK_ADDR_BROADCAST;
    link.link_seq = (uint8_t)loc_pktid; link.flags = 0;
    NetHeader net; net.ver_type = net_ver_type(PKT_LOC); net.flags = 0; net.ttl = ttl;
    net.dst = NODE_ID_BROADCAST; net.src = my_id; net.pkt_id = loc_pktid;
    memcpy(frame, &link, sizeof(link));
    memcpy(frame + sizeof(link), &net, sizeof(net));
    memcpy(frame + HEADER_BYTES, msg, mlen);
    loc_flood_mark(my_id, loc_pktid);            // never re-process our own flood
    txq_push(frame, (uint16_t)(HEADER_BYTES + mlen));
    loc_pktid++;
}

// Re-broadcast a received flood with the hop budget decremented (multi-hop reach).
static void loc_reflood(const uint8_t* buf, uint16_t len, uint8_t out_ttl) {
    uint8_t frame[1 + MAX_PAYLOAD];
    if (len > sizeof(frame)) return;
    memcpy(frame, buf, len);
    LinkHeader link; memcpy(&link, frame, sizeof(link));
    link.prev_hop = LINK_ADDR_NONE; link.next_hop = LINK_ADDR_BROADCAST; link.flags = 0;
    memcpy(frame, &link, sizeof(link));
    NetHeader net; memcpy(&net, frame + sizeof(LinkHeader), sizeof(net));
    net.ttl = out_ttl; memcpy(frame + sizeof(LinkHeader), &net, sizeof(net));
    txq_push(frame, len);
}

// Send a REPLY back to the asker — DV-routed unicast, exactly like DATA.
static void send_loc_unicast(node_id_t dst, const uint8_t* msg, uint16_t mlen) {
    uint8_t frame[1 + MAX_PAYLOAD];
    if (mlen > MAX_PAYLOAD - HEADER_BYTES) return;
    node_id_t   nh = router ? router->next_hop(dst) : 0;
    link_addr_t na = (router && nh) ? router->link_addr_for(nh) : LINK_ADDR_NONE;
    LinkHeader link;
    link.prev_hop = (router && nh) ? router->my_alias_for(nh) : LINK_ADDR_NONE;
    link.next_hop = (na != LINK_ADDR_NONE) ? na : LINK_ADDR_BROADCAST;
    link.link_seq = (uint8_t)loc_pktid; link.flags = 0;
    NetHeader net; net.ver_type = net_ver_type(PKT_LOC); net.flags = 0; net.ttl = DEFAULT_TTL;
    net.dst = dst; net.src = my_id; net.pkt_id = loc_pktid;
    memcpy(frame, &link, sizeof(link));
    memcpy(frame + sizeof(link), &net, sizeof(net));
    memcpy(frame + HEADER_BYTES, msg, mlen);
    if (forwarder) forwarder->mark_seen(my_id, loc_pktid);   // ignore our own rebroadcast
    tx_unicast(frame, (uint16_t)(HEADER_BYTES + mlen), nh);
    loc_pktid++;
}

// Register an opaque id as served by THIS node: cache locally + flood a REGISTER.
static void loc_register(const uint8_t* id, uint8_t id_len) {
    if (id_len == 0 || id_len > mesh::LOC_ID_MAX) return;
    if (loc_epoch == 0) { loc_epoch = (uint16_t)((uint32_t)micros() ^ my_id); if (!loc_epoch) loc_epoch = 1; }
    uint16_t seq = ++loc_seq;
    int slot = -1;
    for (int i = 0; i < 4; i++) {
        if (my_regs[i].used && my_regs[i].id_len == id_len && memcmp(my_regs[i].id, id, id_len) == 0) { slot = i; break; }
        if (slot < 0 && !my_regs[i].used) slot = i;
    }
    if (slot >= 0) { my_regs[slot].used = true; my_regs[slot].id_len = id_len; memcpy(my_regs[slot].id, id, id_len); }
    locdir.upsert(id, id_len, my_id, loc_epoch, seq, LOC_TTL_S, millis());
    uint8_t msg[8 + mesh::LOC_ID_MAX];
    uint16_t mlen = mesh::loc_build_register(loc_epoch, seq, LOC_TTL_S, id, id_len, msg, sizeof(msg));
    if (mlen) send_loc_flood(msg, mlen, LOC_FLOOD_TTL);
}

// Resolve an id to its serving node. Cache hit -> answer `sink` now; miss -> flood a
// QUERY (the REPLY prints `loc <id> <node>` asynchronously via on_rx).
static void loc_resolve(const uint8_t* id, uint8_t id_len, Print* sink) {
    node_id_t loc = 0;
    if (locdir.lookup(id, id_len, &loc, millis())) {
        char hx[2 * mesh::LOC_ID_MAX + 1]; loc_id_hex(id, id_len, hx);
        char line[80]; snprintf(line, sizeof(line), "loc %s %08lX", hx, (unsigned long)loc);
        sink->println(line);
        return;
    }
    uint16_t qid = locres.begin(id, id_len, millis(), LOC_RESOLVE_MS);
    if (!qid) { sink->println("loc busy"); return; }
    uint8_t msg[4 + mesh::LOC_ID_MAX];
    uint16_t mlen = mesh::loc_build_query(qid, id, id_len, msg, sizeof(msg));
    if (mlen) send_loc_flood(msg, mlen, LOC_FLOOD_TTL);   // answer arrives async (or times out)
}

static void loc_dirdump(Print* sink) {
    mesh::LocatorDir::View v[mesh::LOC_DIR_CAP];
    uint16_t n = locdir.snapshot(v, mesh::LOC_DIR_CAP, millis());
    char line[96]; snprintf(line, sizeof(line), "[dir] %u binding(s):", (unsigned)n); sink->println(line);
    for (uint16_t i = 0; i < n; i++) {
        char hx[2 * mesh::LOC_ID_MAX + 1]; loc_id_hex(v[i].id, v[i].id_len, hx);
        snprintf(line, sizeof(line), "  %s -> %08lX  ttl=%us", hx, (unsigned long)v[i].loc, (unsigned)v[i].ttl_s);
        sink->println(line);
    }
}

// Handle a received PKT_LOC frame (REGISTER/QUERY flood, or REPLY unicast).
static void on_loc_rx(const uint8_t* buf, uint16_t len, const NetHeader& net, const LinkHeader& lh) {
    const uint8_t* pay = buf + HEADER_BYTES;
    uint16_t plen = (uint16_t)(len - HEADER_BYTES);
    mesh::LocMsg m;
    if (!mesh::loc_parse(pay, plen, &m)) return;

    if (net.dst == NODE_ID_BROADCAST) {                  // REGISTER / QUERY flood
        if (loc_flood_seen(net.src, net.pkt_id)) return; // already handled this flood
        loc_flood_mark(net.src, net.pkt_id);
        if (m.kind == mesh::LOC_REGISTER) {
            locdir.upsert(m.id, m.id_len, net.src, m.epoch, m.seq, m.ttl_s, millis());
        } else if (m.kind == mesh::LOC_QUERY) {
            mesh::LocBinding b;
            if (locdir.lookup_full(m.id, m.id_len, &b, millis())) {
                uint8_t r[14 + mesh::LOC_ID_MAX];
                uint16_t rlen = mesh::loc_build_reply(m.qid, b.epoch, b.seq, b.ttl_s, b.loc,
                                                      m.id, m.id_len, r, sizeof(r));
                if (rlen) send_loc_unicast(net.src, r, rlen);    // answer the asker
            }
        }
        if (net.ttl > 1) loc_reflood(buf, len, (uint8_t)(net.ttl - 1));   // extend the flood
        return;
    }

    // REPLY: DV-routed unicast — deliver to us, or forward onward.
    if (!forwarder) return;
    mesh::PacketRef p{ net.src, net.dst, net.pkt_id, net.ttl };
    mesh::Decision d = forwarder->decide(p);
    if (d.action == mesh::Action::DELIVER) {
        if (m.kind == mesh::LOC_REPLY) {
            locdir.upsert(m.id, m.id_len, m.loc, m.epoch, m.seq, m.ttl_s, millis());
            locres.on_reply(m.qid);
            char hx[2 * mesh::LOC_ID_MAX + 1]; loc_id_hex(m.id, m.id_len, hx);
            char line[80]; snprintf(line, sizeof(line), "loc %s %08lX", hx, (unsigned long)m.loc);
            Serial.println(line);                        // async resolve result
        }
    } else if (d.action == mesh::Action::FORWARD && lh.next_hop != LINK_ADDR_BROADCAST) {
        forward_frame(buf, len, d.next_hop, d.out_ttl);
    }
}

// Called from radio.poll() (task context, not an ISR) for every received frame.
static void on_rx(const uint8_t* buf, uint16_t len, float rssi, float snr) {
    if (len < HEADER_BYTES) {
        Serial.println("[RX] runt frame, dropped");
        return;
    }

    NetHeader net;
    memcpy(&net, buf + sizeof(LinkHeader), sizeof(net));

    if (net_ver_of(net.ver_type) != PROTO_VERSION) return;  // not ours
    if (net.src == my_id) return;                            // ignore our own echo

    PacketType type = net_type_of(net.ver_type);

    // Learn link quality, DV updates, and aliases ONLY from beacons: a beacon is
    // single-hop (src == the actual transmitter), so RSSI/SNR attribute correctly.
    // A forwarded DATA frame's src is the origin, not the relay that just sent it.
    float q = mesh::quality_from_rf(snr, rssi);
    if (router && type == PKT_BEACON) {
        mesh::Announce ann;              // cleared to empty by default
        const uint16_t base = HEADER_BYTES + sizeof(BeaconPayload);
        if (len > base) {
            // Untrusted radio data — deserialize is bounds-checked and just leaves
            // `ann` empty if the bytes are malformed.
            mesh::announce_deserialize(buf + base, (uint16_t)(len - base), ann);
        }
        router->on_beacon(net.src, q, ann, millis());
    }

    // Link-layer filter: a directed frame carries the next hop's alias. If it's
    // addressed to someone else's alias (not broadcast, not one of ours), it isn't
    // for us to act on — drop it without running the relay engine.
    LinkHeader lh;
    memcpy(&lh, buf, sizeof(lh));
    if (lh.next_hop != LINK_ADDR_BROADCAST && router && !router->is_my_alias(lh.next_hop)) {
        return;
    }

    // A link-layer ACK addressed to us: clear the matching pending frame, done.
    if (lh.flags & LINK_FLAG_IS_ACK) {
        arq.on_ack(lh.link_seq);
        return;                          // ACKs are hop-local, never forwarded
    }

    // A directed frame that requested an ACK: acknowledge the previous hop now —
    // even for a duplicate, so the sender's retransmit stops. The previous hop is
    // the neighbour we assigned this frame's next_hop alias to.
    if ((lh.flags & LINK_FLAG_ACK_REQ) && lh.next_hop != LINK_ADDR_BROADCAST && router) {
        node_id_t prev = router->neighbors().neighbor_by_my_alias(lh.next_hop);
        if (prev) send_ack(prev, lh.link_seq);
    }

    // --- LOC: distributed locator directory (REGISTER/QUERY flood, REPLY unicast) ---
    if (type == PKT_LOC) {
        on_loc_rx(buf, len, net, lh);
        return;
    }

    // --- DATA: deliver, forward, or drop using the relay engine ---
    if (type == PKT_DATA && forwarder) {
        mesh::PacketRef p{ net.src, net.dst, net.pkt_id, net.ttl };
        mesh::Decision d = forwarder->decide(p);
        switch (d.action) {
            case mesh::Action::DELIVER: {
                const uint8_t* pay = buf + HEADER_BYTES;
                uint16_t plen = len - HEADER_BYTES;
                char line[96];
                if (mesh::sar_is_nack(pay, plen)) {
                    // A receiver is asking us to resend the fragments it's missing.
                    uint16_t xid; uint16_t req[mesh::SAR_MAX_FRAGS];
                    uint16_t nr = mesh::sar_parse_nack(pay, plen, &xid, req, mesh::SAR_MAX_FRAGS);
                    if (xid == sar_xfer_id && sar_len > 0 && nr > 0) {
                        memcpy(sar_resend, req, nr * sizeof(uint16_t));
                        sar_resend_n = nr; sar_resend_idx = 0; sar_resend_active = true;
                        sar_tx_dst = net.src;
                        snprintf(line, sizeof(line), "[SAR] NACK from %08lX: resending %u frag(s)",
                                 (unsigned long)net.src, (unsigned)nr);
                        Serial.println(line);
                    }
                    break;
                }
                if (mesh::sar_is_fragment(pay, plen)) {
                    // A file-transfer fragment — feed the reassembler, report progress.
                    uint16_t xf;  // reset the NACK round counter on a new transfer
                    memcpy(&xf, pay + 4, 2);
                    if (xf != sar_rx_last_xfer) { sar_rx_last_xfer = xf; sar_nack_rounds = 0; }
                    sar_rx.add(pay, plen);
                    sar_rx_sender  = net.src;
                    sar_rx_last_ms = millis();
                    sar_quiet_until = millis() + 8000;   // hush our beacons mid-transfer (half-duplex)
                    if (sar_rx.verify()) {
                        if (host_tunnel_active())   // hand the reassembled app packet to the host
                            tunnel_emit(sar_rx_sender, sar_rx.data(), (uint16_t)sar_rx.total_len());
                        snprintf(line, sizeof(line), "[SAR] complete xfer=%u len=%lu frags=%u crc=OK",
                                 (unsigned)sar_rx.xfer_id(), (unsigned long)sar_rx.total_len(),
                                 (unsigned)sar_rx.frag_count());
                    } else if (sar_rx.complete()) {
                        snprintf(line, sizeof(line), "[SAR] complete xfer=%u len=%lu CRC MISMATCH",
                                 (unsigned)sar_rx.xfer_id(), (unsigned long)sar_rx.total_len());
                    } else {
                        snprintf(line, sizeof(line), "[SAR] frag %u/%u xfer=%u",
                                 (unsigned)sar_rx.got_count(), (unsigned)sar_rx.frag_count(),
                                 (unsigned)sar_rx.xfer_id());
                    }
                    Serial.println(line);
                } else if (host_tunnel_active()) {
                    tunnel_emit(net.src, pay, plen);     // opaque app packet -> host bridge (USB or BLE)
                } else {
                    char payload[64];
                    uint16_t n = plen < sizeof(payload) - 1 ? plen : (uint16_t)(sizeof(payload) - 1);
                    memcpy(payload, pay, n);
                    payload[n] = '\0';
                    snprintf(line, sizeof(line), "[RX] DATA delivered from %08lX id=%u: \"%s\"",
                             (unsigned long)net.src, (unsigned)net.pkt_id, payload);
                    Serial.println(line);
                }
                break;
            }
            case mesh::Action::FORWARD: {
                // Never relay a BROADCAST-addressed DATA frame. Broadcast is only the
                // sender's brief fallback when it has no negotiated alias for the next
                // hop yet — a 1-hop best-effort. Relaying it floods the mesh and the
                // destination hears the message twice (direct + relayed). Multi-hop is
                // carried by DIRECTED unicast once aliases negotiate (seconds).
                if (lh.next_hop == LINK_ADDR_BROADCAST) {
                    Serial.println("[FWD] skip (broadcast not relayed)");
                    break;
                }
                forward_frame(buf, len, d.next_hop, d.out_ttl);
                sar_quiet_until = millis() + 8000;   // relay stays quiet while data flows through it
                char line[96];
                snprintf(line, sizeof(line),
                         "[FWD] %08lX->%08lX id=%u via %08lX ttl=%u",
                         (unsigned long)net.src, (unsigned long)net.dst,
                         (unsigned)net.pkt_id, (unsigned long)d.next_hop, (unsigned)d.out_ttl);
                Serial.println(line);
                break;
            }
            default: {
                static const char* reason[] = {"", "", "own", "dup", "ttl", "no-route"};
                char line[80];
                snprintf(line, sizeof(line), "[DROP] DATA id=%u (%s)",
                         (unsigned)net.pkt_id, reason[(uint8_t)d.action]);
                Serial.println(line);
                break;
            }
        }
        return;
    }

    // --- BEACON / other: log link metrics + table sizes ---
    char line[96];
    if (type == PKT_BEACON && len >= HEADER_BYTES + (uint16_t)sizeof(BeaconPayload)) {
        BeaconPayload pl;
        memcpy(&pl, buf + HEADER_BYTES, sizeof(pl));
        snprintf(line, sizeof(line),
                 "[RX] beacon  src=%08lX seq=%u up=%us  rssi=",
                 (unsigned long)net.src, (unsigned)net.pkt_id, (unsigned)pl.uptime_s);
    } else {
        snprintf(line, sizeof(line),
                 "[RX] type=%u  src=%08lX seq=%u len=%u  rssi=",
                 (unsigned)type, (unsigned long)net.src, (unsigned)net.pkt_id,
                 (unsigned)len);
    }

    // Float fields printed via Print::print(float, digits) to dodge newlib-nano's
    // missing %f in snprintf.
    Serial.print(line);
    Serial.print(rssi, 1);
    Serial.print(" dBm  snr=");
    Serial.print(snr, 1);
    Serial.print(" dB  q=");
    Serial.print(q, 2);
    Serial.print("  neighbors=");
    Serial.print(router ? router->neighbors().count() : 0);
    Serial.print("  routes=");
    Serial.println(router ? router->routes().count() : 0);
}

// --- tunnel framing (HDLC, matches RNS's PipeInterface) ---------------------
// (HDLC constants declared earlier so BLE input decoding can share them.)

// Route raw bytes to the attached host transport: BLE if a client is connected,
// else USB serial. BLEUart.write chunks across notifications internally.
static void host_write(const uint8_t* d, uint16_t n) {
#ifdef AGN_BLE
    if (ble_connected) { bleuart.write(d, n); ble_txb += n; return; }
#endif
    Serial.write(d, n);
}

// Write one HDLC frame (FLAG, byte-stuffed payload, FLAG) to the host transport.
static void hdlc_write(const uint8_t* d, uint16_t n) {
    // Sized for the worst case: a fully-escaped envelope ([type][len][locator]+payload).
    static uint8_t out[2 + 2 * (2 + sizeof(node_id_t) + MAX_PAYLOAD)];
    uint16_t o = 0;
    out[o++] = HDLC_FLAG;
    for (uint16_t i = 0; i < n; i++) {
        uint8_t b = d[i];
        if (b == HDLC_FLAG || b == HDLC_ESC) { out[o++] = HDLC_ESC; out[o++] = b ^ HDLC_ESC_MASK; }
        else out[o++] = b;
    }
    out[o++] = HDLC_FLAG;
    host_write(out, o);
}

// Emit a delivered app payload to the host as the typed envelope:
// [LOCATOR][len=sizeof(node_id)][src node-id LE][payload].
static void tunnel_emit(node_id_t src, const uint8_t* payload, uint16_t plen) {
    const uint8_t alen = (uint8_t)sizeof(node_id_t);
    uint8_t f[2 + sizeof(node_id_t) + MAX_PAYLOAD];
    if (plen > MAX_PAYLOAD) plen = MAX_PAYLOAD;
    f[0] = TUN_ADDR_LOCATOR;
    f[1] = alen;
    memcpy(f + 2, &src, alen);
    memcpy(f + 2 + alen, payload, plen);
    hdlc_write(f, (uint16_t)(2 + alen + plen));
}

// Handle a frame from the host: [addr_type][addr_len][addr][payload] -> send into the mesh.
static void tunnel_rx_frame(const uint8_t* f, uint16_t n) {
    if (n < 2) return;
    uint8_t addr_type = f[0];
    uint8_t addr_len  = f[1];
    if (n < (uint16_t)(2 + addr_len)) return;                    // truncated envelope
    if (addr_type == TUN_ADDR_IDENTITY) return;                  // reserved (resolve-and-forward) — not yet
    if (addr_type != TUN_ADDR_LOCATOR || addr_len != sizeof(node_id_t)) return;  // unknown/unsupported addr
    node_id_t dst; memcpy(&dst, f + 2, sizeof(node_id_t));
    const uint8_t* payload = f + 2 + addr_len;
    uint16_t plen = (uint16_t)(n - 2 - addr_len);
    if (plen <= MAX_PAYLOAD - HEADER_BYTES) {
        send_data_bytes(dst, payload, plen, false);   // fits one frame
    } else if (!sar_tx_active && !sar_resend_active && plen <= mesh::SAR_MAX_FILE) {
        memcpy(sar_buf, payload, plen);                // larger: segment over the mesh
        sar_len = plen; sar_crc = mesh::sar_crc32(sar_buf, plen); sar_xfer_id++;
        sar_tx_dst = dst; sar_tx_count = mesh::sar_frag_count(plen); sar_tx_idx = 0; sar_tx_active = true;
    }
    // else: a big packet while a transfer is busy — drop; the app layer will retry.
}

// --- runtime command console (USB serial) ----------------------------------
// The local control surface. `send` originates app data; `block`/`unblock` drive
// the Tier-1 link-block setting at runtime. When the controller lands (Phase 4),
// the same Router calls are invoked by signed control packets instead of typed in.
static node_id_t parse_id(const char* s) { return (node_id_t)strtoul(s, nullptr, 16); }

static int hexnyb(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static uint16_t hex_decode(const char* s, uint8_t* out, uint16_t cap) {
    uint16_t n = 0;
    while (s[0] && s[1] && n < cap) {
        int hi = hexnyb(s[0]), lo = hexnyb(s[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

// Pace the SAR send: stop-and-wait, one fragment fully sent + ACKed before the next
// (each ~2 s frame + ~1 s ACK at SF11). The per-hop ARQ recovers single-hop loss; the
// NACK loop (below) recovers anything lost end-to-end. Handles both the initial pass
// and resend requests.
static void sar_tx_tick() {
    if (!sar_tx_active && !sar_resend_active) return;
    if (radio.busy() || txq_count > 0 || arq.pending_count() > 0) return;  // wait for prev

    uint16_t idx;
    if (sar_tx_active) {
        idx = sar_tx_idx++;
        if ((sar_tx_idx % 5) == 0 || sar_tx_idx == sar_tx_count) {
            char l[48]; snprintf(l, sizeof(l), "[SAR] sent %u/%u",
                                 (unsigned)sar_tx_idx, (unsigned)sar_tx_count);
            Serial.println(l);
        }
        if (sar_tx_idx >= sar_tx_count) sar_tx_active = false;
    } else {                                   // resending NACKed fragments
        idx = sar_resend[sar_resend_idx++];
        if (sar_resend_idx >= sar_resend_n) sar_resend_active = false;
    }

    uint8_t  frag[1 + mesh::SAR_HDR_BYTES + mesh::SAR_CHUNK];
    uint16_t flen = mesh::sar_build_fragment(sar_buf, sar_len, sar_xfer_id, sar_crc,
                                             idx, frag, sizeof(frag));
    if (flen) send_data_bytes(sar_tx_dst, frag, flen, false);
}

// Receiver: if a transfer has stalled incomplete (sender went quiet), ask for the
// fragments we're still missing. Bounded rounds so a dead link gives up.
static void sar_rx_tick() {
    if (!sar_rx.active() || sar_rx.complete()) return;
    if ((int32_t)(millis() - sar_rx_last_ms) < (int32_t)SAR_NACK_TIMEOUT_MS) return;
    if (sar_nack_rounds >= SAR_MAX_NACK_ROUNDS) return;

    uint16_t miss[mesh::SAR_MAX_FRAGS];
    uint16_t nm = sar_rx.missing(miss, mesh::SAR_MAX_FRAGS);
    if (nm == 0) return;
    uint8_t nack[1 + MAX_PAYLOAD];
    uint16_t nlen = mesh::sar_build_nack(sar_rx.xfer_id(), miss, nm, nack,
                                         (uint16_t)(MAX_PAYLOAD - HEADER_BYTES));
    send_data_bytes(sar_rx_sender, nack, nlen, false);
    sar_nack_rounds++;
    sar_rx_last_ms = millis();
    sar_quiet_until = millis() + 8000;
    char l[64]; snprintf(l, sizeof(l), "[SAR] NACK round %u: %u missing",
                         (unsigned)sar_nack_rounds, (unsigned)nm);
    Serial.println(l);
}

static void print_info() {
    if (!router) return;
    Print& Serial = *g_con;   // route to the active console sink (USB or BLE)
    char l[100];
    snprintf(l, sizeof(l), "node %08lX  neighbors=%u routes=%u blocked=%u",
             (unsigned long)my_id, (unsigned)router->neighbors().count(),
             (unsigned)router->routes().count(), (unsigned)router->blocked_count());
    Serial.println(l);
    const mesh::NeighborTable& nt = router->neighbors();
    for (uint8_t i = 0; i < mesh::MAX_NEIGHBORS; i++) {
        const mesh::Neighbor* n = nt.at(i);
        if (!n->used) continue;
        snprintf(l, sizeof(l), "  nbr %08lX  q_rx=%d q_tx=%d (x100)  myAlias=%u theirAlias=%u",
                 (unsigned long)n->id, (int)(n->q_rx * 100), (int)(n->q_tx * 100),
                 (unsigned)n->my_alias, (unsigned)n->their_alias);
        Serial.println(l);
    }
    const mesh::RoutingTable& rt = router->routes();
    for (uint8_t i = 0; i < mesh::MAX_ROUTES; i++) {
        const mesh::Route* r = rt.at(i);
        if (!r->used) continue;
        snprintf(l, sizeof(l), "  route dst=%08lX via=%08lX cost=%d hops=%u",
                 (unsigned long)r->dst, (unsigned long)r->next_hop,
                 (int)(r->cost * 16), (unsigned)r->hops);
        Serial.println(l);
    }
}

static void handle_command(char* line) {
    Print& Serial = *g_con;   // route command output to the source transport (USB or BLE)
    char* cmd = strtok(line, " ");
    if (!cmd) return;

    if (!strcmp(cmd, "send")) {
        char* ids = strtok(nullptr, " ");
        char* msg = strtok(nullptr, "");          // rest of the line is the payload
        if (!ids) { Serial.println("usage: send <hexid> <msg>"); return; }
        send_data(parse_id(ids), msg ? msg : "ping");
    } else if (!strcmp(cmd, "block") || !strcmp(cmd, "unblock")) {
        char* ids = strtok(nullptr, " ");
        if (!ids) { Serial.println("usage: block/unblock <hexid>"); return; }
        node_id_t id = parse_id(ids);
        char l[48];
        if (cmd[0] == 'b') { bool ok = router && router->block(id);
            snprintf(l, sizeof(l), "block %08lX -> %s", (unsigned long)id, ok ? "ok" : "fail"); }
        else { if (router) router->unblock(id);
            snprintf(l, sizeof(l), "unblock %08lX", (unsigned long)id); }
        Serial.println(l);
    } else if (!strcmp(cmd, "info")) {
        print_info();
    } else if (!strcmp(cmd, "sbegin")) {            // start loading a blob to send
        char* lenS = strtok(nullptr, " ");
        char* crcS = strtok(nullptr, " ");
        if (!lenS || !crcS) { Serial.println("usage: sbegin <len> <crc32hex>"); return; }
        sar_len = 0;
        sar_crc = (uint32_t)strtoul(crcS, nullptr, 16);
        char l[48]; snprintf(l, sizeof(l), "sbegin len=%lu crc=%08lX",
                             (unsigned long)strtoul(lenS, nullptr, 10), (unsigned long)sar_crc);
        Serial.println(l);
    } else if (!strcmp(cmd, "sdata")) {             // append hex bytes to the blob
        char* hx = strtok(nullptr, " ");
        if (!hx) { Serial.println("usage: sdata <hex>"); return; }
        uint16_t got = hex_decode(hx, sar_buf + sar_len, (uint16_t)(mesh::SAR_MAX_FILE - sar_len));
        sar_len += got;
        char l[48]; snprintf(l, sizeof(l), "sdata +%u total=%lu", (unsigned)got, (unsigned long)sar_len);
        Serial.println(l);
    } else if (!strcmp(cmd, "xfer")) {              // segment+send the blob to <hexid>
        char* ids = strtok(nullptr, " ");
        if (!ids) { Serial.println("usage: xfer <hexid>"); return; }
        sar_tx_dst = parse_id(ids); sar_xfer_id++;
        sar_tx_count = mesh::sar_frag_count(sar_len); sar_tx_idx = 0; sar_tx_active = true;
        char l[64]; snprintf(l, sizeof(l), "[SAR] xfer start -> %08lX len=%lu frags=%u",
                             (unsigned long)sar_tx_dst, (unsigned long)sar_len, (unsigned)sar_tx_count);
        Serial.println(l);
    } else if (!strcmp(cmd, "dump")) {              // print the received blob as hex
        if (!sar_rx.complete()) { Serial.println("[DUMP] no complete transfer"); return; }
        uint32_t n = sar_rx.total_len(); const uint8_t* d = sar_rx.data();
        char l[48]; snprintf(l, sizeof(l), "[DUMP] len=%lu crc=%08lX",
                             (unsigned long)n, (unsigned long)mesh::sar_crc32(d, n));
        Serial.println(l);
        char hexline[65];
        for (uint32_t off = 0; off < n; off += 32) {
            uint32_t m = (n - off) < 32 ? (n - off) : 32;
            for (uint32_t i = 0; i < m; i++) snprintf(hexline + i * 2, 3, "%02X", d[off + i]);
            hexline[m * 2] = 0; Serial.println(hexline);
        }
        Serial.println("[ENDDUMP]");
    } else if (!strcmp(cmd, "tunnel")) {
        tunnel_mode = true;     // serial becomes a binary HDLC pipe for a host bridge
        Serial.println("tunnel on");
#ifdef AGN_BLE
    } else if (!strcmp(cmd, "ble")) {        // ble | ble on | ble off
        char* a = strtok(nullptr, " ");
        if (a && !strcmp(a, "on"))  { ble_start_adv(); cfg_save(); }
        else if (a && !strcmp(a, "off")) { ble_stop_adv(); cfg_save(); }
        char m[80];
        snprintf(m, sizeof(m), "BLE name=AgnLoRa-%08lX advertising=%d connected=%d PIN=%s",
                 (unsigned long)my_id, (int)ble_advertising, (int)ble_connected, ble_pin);
        Serial.println(m);
    } else if (!strcmp(cmd, "blepin")) {     // blepin | blepin random | blepin <6 digits>
        char* a = strtok(nullptr, " ");
        if (a && !strcmp(a, "random")) ble_gen_pin();
        else if (a && strlen(a) == 6) { strncpy(ble_pin, a, 6); ble_pin[6] = 0; }
        if (ble_advertising) ble_start_adv();   // re-apply for the next pairing
        cfg_save();                             // persist the new PIN across reboots
        char m[40]; snprintf(m, sizeof(m), "BLE PIN=%s (saved)", ble_pin); Serial.println(m);
#endif
    } else if (!strcmp(cmd, "rf")) {           // rf [show] | rf <field> <val> | rf apply|revert|default
        char* a = strtok(nullptr, " ");
        char* v = a ? strtok(nullptr, " ") : nullptr;
        if (!a || !strcmp(a, "show")) {
            rf_print(radio.config(), "active");
            if (memcmp(&rf_work, &radio.config(), sizeof(RadioCfg)) != 0)
                rf_print(rf_work, "staged — rf apply to commit");
        } else if (!strcmp(a, "apply")) {
            // Commit staged PHY to the radio. WARNING: changing freq/SF/BW drops this
            // node off-air from any peer still on the old PHY (a coordinated retune).
            int16_t st = radio.apply_config(rf_work);
            if (st == RADIOLIB_ERR_NONE) {
                rf_save(radio.config());          // persist only on success (save-if-dirty)
                Serial.println("[rf] applied + saved");
                rf_print(radio.config(), "active");
            } else {
                char m[48]; snprintf(m, sizeof(m), "[rf] apply FAILED: %d (radio unchanged)", (int)st);
                Serial.println(m);
                rf_work = radio.config();         // resync staging to reality
            }
        } else if (!strcmp(a, "revert")) {
            rf_work = radio.config();
            rf_print(rf_work, "staged=active");
        } else if (!strcmp(a, "default")) {
            rf_work = radio_default_config();
            rf_print(rf_work, "staged defaults — rf apply to commit");
        } else if (v) {
            bool ok = true;
            if      (!strcmp(a, "freq"))     rf_work.freq_hz    = (uint32_t)strtoul(v, nullptr, 10);
            else if (!strcmp(a, "bw"))       rf_work.bw_hz      = (uint32_t)lroundf(atof(v) * 1000.0f);
            else if (!strcmp(a, "sf"))       rf_work.sf         = (uint8_t)atoi(v);
            else if (!strcmp(a, "cr"))       rf_work.cr         = (uint8_t)atoi(v);
            else if (!strcmp(a, "power") || !strcmp(a, "pwr")) rf_work.power_dbm = (int8_t)atoi(v);
            else if (!strcmp(a, "sync"))     rf_work.sync       = (uint8_t)strtol(v, nullptr, 0);
            else if (!strcmp(a, "preamble") || !strcmp(a, "pre")) rf_work.preamble = (uint16_t)atoi(v);
            else { ok = false; Serial.println("[rf] unknown field (freq|bw|sf|cr|power|sync|preamble)"); }
            if (ok) rf_print(rf_work, "staged — rf apply to commit");
        } else {
            Serial.println("[rf] usage: rf show | rf <freq|bw|sf|cr|power|sync|preamble> <val> | rf apply | rf revert | rf default");
        }
    } else if (!strcmp(cmd, "register")) {     // register <idhex> — assert this node serves id
        char* a = strtok(nullptr, " ");
        uint8_t id[mesh::LOC_ID_MAX];
        uint16_t n = a ? hex_decode(a, id, mesh::LOC_ID_MAX) : 0;
        if (n == 0) { Serial.println("usage: register <idhex> (1..16 bytes)"); }
        else {
            loc_register(id, (uint8_t)n);
            char l[56]; snprintf(l, sizeof(l), "registered %u-byte id at %08lX",
                                 (unsigned)n, (unsigned long)my_id); Serial.println(l);
        }
    } else if (!strcmp(cmd, "resolve")) {      // resolve <idhex> — find the serving node
        char* a = strtok(nullptr, " ");
        uint8_t id[mesh::LOC_ID_MAX];
        uint16_t n = a ? hex_decode(a, id, mesh::LOC_ID_MAX) : 0;
        if (n == 0) Serial.println("usage: resolve <idhex>");
        else loc_resolve(id, (uint8_t)n, &Serial);   // Serial is *g_con (the source transport)
    } else if (!strcmp(cmd, "dirdump")) {
        loc_dirdump(&Serial);
    } else if (!strcmp(cmd, "help")) {
        Serial.println("send <id> <msg> | block/unblock <id> | info | sbegin <len> <crc> | sdata <hex> | xfer <id> | dump | tunnel");
        Serial.println("rf [show] | rf <freq|bw|sf|cr|power|sync|preamble> <val> | rf apply | rf revert | rf default");
        Serial.println("register <idhex> | resolve <idhex> | dirdump");
#ifdef AGN_BLE
        Serial.println("ble [on|off] | blepin [random|<6 digits>]");
#endif
    } else {
        Serial.print("unknown cmd: "); Serial.println(cmd);
    }
}

// Run one console line, routing its output to `out` (the transport it came in on),
// then restore the default sink. Used by both the USB console and the BLE demux.
static void console_exec(char* line, Print* out) {
    g_con = out;
    handle_command(line);
    g_con = &Serial;
}

static void poll_console() {
    // Text-line console until `tunnel` switches us to a binary HDLC pipe.
    static char    buf[720];
    static uint16_t len = 0;
    // HDLC decode state (tunnel mode)
    static uint8_t  frame[4 + MAX_PAYLOAD];
    static uint16_t flen = 0;
    static bool     in_frame = false, esc = false;

    while (Serial.available()) {
        uint8_t c = (uint8_t)Serial.read();

        if (!tunnel_mode) {
            if (c == '\r') continue;
            if (c == '\n') {
                if (len > 0) { buf[len] = 0; Serial.print("> "); Serial.println((char*)buf); console_exec((char*)buf, &Serial); len = 0; }
            } else if (len < sizeof(buf) - 1) {
                buf[len++] = (char)c;
            }
            continue;
        }

        // --- tunnel: demux HDLC data frames vs. text console lines (like ble_poll) ---
        // 0x7E-delimited bytes are tunnel frames; plain text lines are console commands
        // (so `register`/`resolve`/`rf`/… work over the USB tunnel, not just BLE).
        if (c == HDLC_FLAG) {
            if (in_frame) { in_frame = false; if (flen > 0) tunnel_rx_frame(frame, flen); }
            else          { in_frame = true; flen = 0; esc = false; }
            len = 0;                                 // a frame boundary is never part of a line
        } else if (in_frame) {
            if (flen < sizeof(frame)) {
                if (c == HDLC_ESC) esc = true;
                else { frame[flen++] = esc ? (c ^ HDLC_ESC_MASK) : c; esc = false; }
            }
        } else if (c == '\r') {
            // ignore
        } else if (c == '\n') {
            if (len > 0) { buf[len] = 0; console_exec((char*)buf, &Serial); len = 0; }
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = (char)c;
        }
    }
}

void setup() {
    boot_ms = millis();
    Serial.begin(115200);
#ifdef LED_BUILTIN
    pinMode(LED_BUILTIN, OUTPUT);   // heartbeat / radio-fault indicator (see loop & below)
#endif
    // Brief wait for a USB host to attach, so the banner isn't lost — never spin
    // forever (headless nodes have no host).
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { /* wait for monitor, briefly */ }

    my_id = derive_node_id();
    randomSeed(my_id);
    static mesh::Router router_inst(my_id);   // static storage, no heap
    router = &router_inst;
    static mesh::Forwarder fwd_inst(my_id, router_inst);
    forwarder = &fwd_inst;

    Serial.println();
    Serial.println("=== LoRa Mesh Backbone — Phase 0 link prober ===");
    char banner[80];
    snprintf(banner, sizeof(banner), "fw=%s  node=%08lX",
             AGN_FW_VERSION, (unsigned long)my_id);
    Serial.println(banner);
    snprintf(banner, sizeof(banner), "PHY: %d.%03d MHz BW250 SF%d CR4/%d sync=0x%02X",
             (int)PHY_FREQ_MHZ, (int)((PHY_FREQ_MHZ - (int)PHY_FREQ_MHZ) * 1000 + 0.5f),
             (int)PHY_SF, (int)PHY_CODING_RATE, (unsigned)PHY_SYNC_WORD);
    Serial.println(banner);
    snprintf(banner, sizeof(banner), "header=%u bytes (link=%u net=%u)",
             (unsigned)HEADER_BYTES, (unsigned)sizeof(LinkHeader),
             (unsigned)sizeof(NetHeader));
    Serial.println(banner);

#ifdef AGN_BLE
    cfg_load();    // restore the persisted PIN + BLE-enabled state from flash (no SoftDevice needed)
    if (cfg_first_boot) cfg_save();          // persist the freshly-generated PIN (SD-safe write)
    if (cfg_ble_enabled) ble_start_adv();    // was on before reboot -> bring up the SoftDevice + advertise
    // If BLE was off, the SoftDevice is left disabled: zero runtime/power cost until `ble on`.
    { char m[72]; snprintf(m, sizeof(m), "BLE compiled in. PIN=%s advertising=%d stack=%s (persisted)",
                           ble_pin, (int)ble_advertising, ble_inited ? "up" : "lazy"); Serial.println(m); }
#endif

    Serial.println("initializing radio...");
    Serial.flush();
    RadioCfg rfcfg = rf_load();   // persisted PHY, or compile-time defaults on first boot
    rf_work = rfcfg;              // staging copy starts from what's actually applied
    rf_print(rfcfg, "active");
    int16_t st = radio.begin(on_rx, rfcfg);
    // LED semaphore (serial is unreliable over WSL/usbip):
    //   solid          -> radio.begin() HUNG (never returned)
    //   FAST blink ~5Hz -> radio.begin() returned an ERROR (code in serial if avail)
    //   slow blink (3s) -> success, main loop running (see heartbeat below)
    if (st != RADIOLIB_ERR_NONE) {
        uint32_t lastp = 0;
        while (true) {
#ifdef LED_BUILTIN
            digitalWrite(LED_BUILTIN, HIGH); delay(100);
            digitalWrite(LED_BUILTIN, LOW);  delay(100);
#endif
            if (millis() - lastp > 1500) {
                lastp = millis();
                char m[64];
                snprintf(m, sizeof(m), "RADIO INIT FAILED: %d", (int)st);
                Serial.println(m);
            }
        }
    }
    Serial.println("radio up, listening...");
    Serial.println("commands: send <hexid> <msg> | block <hexid> | unblock <hexid> | info | help");
    next_beacon_ms = schedule_next_beacon();
    next_tick_ms   = millis() + TICK_PERIOD_MS;
    next_arq_ms    = millis() + ARQ_TICK_MS;
}

void loop() {
    // 0) Bring-up heartbeat: a line every ~3 s so a monitor opened at any moment
    //    sees the node is alive (and whether it has found neighbours), without
    //    waiting for the ~10 s beacon cadence.
    static uint32_t next_hb_ms = 0;
    if ((int32_t)(millis() - next_hb_ms) >= 0) {
        char hb[80];
        snprintf(hb, sizeof(hb), "[hb] up=%lus  node=%08lX  nbrs=%u routes=%u txq=%u",
                 (unsigned long)((millis() - boot_ms) / 1000u), (unsigned long)my_id,
                 (unsigned)(router ? router->neighbors().count() : 0),
                 (unsigned)(router ? router->routes().count() : 0),
                 (unsigned)txq_count);
        Serial.println(hb);
#ifdef AGN_BLE
        char bl[72];
        snprintf(bl, sizeof(bl), "[ble] adv=%d connected=%d rx=%lu tx=%lu PIN=%s",
                 (int)ble_advertising, (int)ble_connected,
                 (unsigned long)ble_rxb, (unsigned long)ble_txb, ble_pin);
        Serial.println(bl);   // watch this stay connected=1 through the LoRa beacons above
#endif
#ifdef LED_BUILTIN
        static bool led = false; led = !led; digitalWrite(LED_BUILTIN, led);  // slow blink = alive & radio up
#endif
        next_hb_ms = millis() + 3000;
    }

    // 1) Service the radio (TX-done / RX-done). Cheap when nothing is pending; this
    //    is the only place SPI work happens, keeping the radio core non-blocking.
    radio.poll();

    // 2) Drain the outbound queue to the radio whenever it's free.
    txq_pump();

    // 3) Time to beacon? Enqueue it — but stay quiet during an active file transfer
    //    (sending, or recently received a fragment): half-duplex means our beacon
    //    would clobber an in-flight fragment/ACK. Reschedule so we resume after.
    if ((int32_t)(millis() - next_beacon_ms) >= 0) {
        if (sar_tx_active || sar_resend_active || (int32_t)(millis() - sar_quiet_until) < 0) {
            next_beacon_ms = millis() + 2000;   // defer; recheck shortly
        } else {
            send_beacon();
            next_beacon_ms = schedule_next_beacon();
        }
    }

    // 4) Hop-by-hop ARQ: retransmit unacked frames / give up after the retry limit.
    if ((int32_t)(millis() - next_arq_ms) >= 0) {
        arq.tick(millis(), arq_resend, nullptr);
        next_arq_ms = millis() + ARQ_TICK_MS;
    }

    // 5) Age out neighbours/routes we've stopped hearing.
    if (router && (int32_t)(millis() - next_tick_ms) >= 0) {
        router->tick(millis());
        next_tick_ms = millis() + TICK_PERIOD_MS;
    }

    // 6) Drive an in-progress SAR file transfer + the receiver's missing-frag requests.
    sar_tx_tick();
    sar_rx_tick();

    // 6b) Locator directory: expire timed-out resolutions, and re-register our own ids
    //     before their bindings lapse (keeps the distributed cache fresh).
    locres.tick(millis());
    if ((int32_t)(millis() - next_rereg_ms) >= 0) {
        for (auto& r : my_regs) if (r.used) loc_register(r.id, r.id_len);
        next_rereg_ms = millis() + LOC_REREG_MS;
    }

#ifdef AGN_BLE
    ble_poll();    // service the BLE UART (echo) — must stay non-blocking
#endif

    // 7) Service the runtime command console (USB serial). This is the local stand-in
    //    for the Tier-1 control plane — `block`/`unblock` here will later arrive as
    //    signed control packets from the RPi controller, driving the same Router API.
    poll_console();
}
