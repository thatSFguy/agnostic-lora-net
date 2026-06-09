// main.cpp — link prober + routing + multi-hop forwarding (Agent.md §6/§7).
//
// Each node beacons periodically; every beacon carries the node's announce
// (neighbour reports + distance-vector table), and every received frame's RSSI/SNR
// feeds the portable routing core (lib/mesh). So nodes build a live per-direction
// neighbour/route table over the air, and DATA packets are delivered, forwarded
// toward next_hop(), or dropped (dedup / TTL / no-route) by the relay engine.
//
// All the routing/codec/forwarding *logic* is unit-tested host-side
// (test/test_mesh, test_codec, test_forward); here it is wired onto the
// non-blocking SX1262 transport and cross-compiled for the nRF52. Build with
// -DAGN_DATA_DEST=0x...  on one node to watch a 3-node line forward end to end.
//
// What this is NOT (yet): no BLE (that layer is borrowed wholesale from the forked
// MeshCore later — never hand-rolled, §9), no crypto, no link-layer unicast
// (forwarding rebroadcasts with TTL+dedup until link aliases land in Phase 2/3).

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
static void tunnel_emit(node_id_t src, const uint8_t* payload, uint16_t plen);  // fwd decl
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
                        if (tunnel_mode)   // hand the reassembled app packet to the host
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
                } else if (tunnel_mode) {
                    tunnel_emit(net.src, pay, plen);     // opaque app packet -> host bridge
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
static const uint8_t HDLC_FLAG = 0x7E, HDLC_ESC = 0x7D, HDLC_ESC_MASK = 0x20;

// Write one HDLC frame: FLAG, byte-stuffed payload, FLAG.
static void hdlc_write(const uint8_t* d, uint16_t n) {
    Serial.write(HDLC_FLAG);
    for (uint16_t i = 0; i < n; i++) {
        uint8_t b = d[i];
        if (b == HDLC_FLAG || b == HDLC_ESC) { Serial.write(HDLC_ESC); Serial.write(b ^ HDLC_ESC_MASK); }
        else Serial.write(b);
    }
    Serial.write(HDLC_FLAG);
}

// Emit a delivered app payload to the host as [u32 src][payload].
static void tunnel_emit(node_id_t src, const uint8_t* payload, uint16_t plen) {
    uint8_t f[4 + MAX_PAYLOAD];
    if (plen > MAX_PAYLOAD) plen = MAX_PAYLOAD;
    memcpy(f, &src, 4);
    memcpy(f + 4, payload, plen);
    hdlc_write(f, (uint16_t)(4 + plen));
}

// Handle a frame from the host: [u32 dst][payload] -> send into the mesh.
static void tunnel_rx_frame(const uint8_t* f, uint16_t n) {
    if (n < 4) return;
    node_id_t dst; memcpy(&dst, f, 4);
    const uint8_t* payload = f + 4;
    uint16_t plen = (uint16_t)(n - 4);
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
    } else if (!strcmp(cmd, "help")) {
        Serial.println("send <id> <msg> | block/unblock <id> | info | sbegin <len> <crc> | sdata <hex> | xfer <id> | dump | tunnel");
    } else {
        Serial.print("unknown cmd: "); Serial.println(cmd);
    }
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
                if (len > 0) { buf[len] = 0; Serial.print("> "); Serial.println((char*)buf); handle_command((char*)buf); len = 0; }
            } else if (len < sizeof(buf) - 1) {
                buf[len++] = (char)c;
            }
            continue;
        }

        // --- tunnel: HDLC frame decode ---
        if (in_frame && c == HDLC_FLAG) {            // end of frame
            in_frame = false;
            if (flen > 0) tunnel_rx_frame(frame, flen);
        } else if (c == HDLC_FLAG) {                 // start of frame
            in_frame = true; flen = 0; esc = false;
        } else if (in_frame && flen < sizeof(frame)) {
            if (c == HDLC_ESC) { esc = true; }
            else { frame[flen++] = esc ? (c ^ HDLC_ESC_MASK) : c; esc = false; }
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

    Serial.println("initializing radio...");
    Serial.flush();
    int16_t st = radio.begin(on_rx);
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

    // 7) Service the runtime command console (USB serial). This is the local stand-in
    //    for the Tier-1 control plane — `block`/`unblock` here will later arrive as
    //    signed control packets from the RPi controller, driving the same Router API.
    poll_console();
}
