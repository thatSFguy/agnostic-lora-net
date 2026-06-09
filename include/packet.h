// packet.h — on-air frame format (Agent.md §5, draft; finalized in Phase 0).
//
// Two stacked headers so hop-by-hop reliability is cleanly separated from
// end-to-end routing, and asymmetric paths fall out for free:
//
//   [ LinkHeader ][ NetHeader ][ payload ... ]
//   |<- per hop ->|<--------- end to end --------->|
//
// The LinkHeader is rewritten at every hop (link-local neighbour aliases + ARQ).
// The NetHeader is set by the source and only its `ttl` changes in flight.
// Payload is OPAQUE to the backbone — apps bring their own crypto (§2.5). Only
// control packets are authenticated (a signature rides in the payload, added in
// Phase 4).
//
// Wire layout is little-endian (nRF52 / ESP32 are both LE) and the structs are
// packed so sizeof() == on-air size. Goal: combined header in the low double
// digits of bytes so small messages stay cheap at SF11 (§5).
#pragma once

#include <stdint.h>

// 4-byte node ID, derived from the node's public key (self-certifying, §3).
// In Phase 0 this is a provisional value from the chip's FICR DEVICEID.
typedef uint32_t node_id_t;

static const node_id_t NODE_ID_BROADCAST = 0xFFFFFFFFu;

// 1-byte link-local neighbour aliases, negotiated per link (§5). 0 = unassigned;
// 0xFF = link-local broadcast (all neighbours, e.g. for beacons).
typedef uint8_t link_addr_t;
static const link_addr_t LINK_ADDR_NONE      = 0x00;
static const link_addr_t LINK_ADDR_BROADCAST = 0xFF;

// Protocol version carried in NetHeader.ver_type (high nibble). Bump on any
// wire-incompatible change.
static const uint8_t PROTO_VERSION = 1;

// Packet class — NetHeader.ver_type low nibble (§5: data | control | ack | beacon).
enum PacketType : uint8_t {
    PKT_DATA    = 0,  // opaque application payload
    PKT_CONTROL = 1,  // signed controller command (power / block / route)
    PKT_ACK     = 2,  // end-to-end acknowledgement
    PKT_BEACON  = 3,  // neighbour discovery + piggybacked link metrics / DV updates
};

// NetHeader.flags bits.
enum NetFlags : uint8_t {
    NET_FLAG_ACK_REQ = 1 << 0,  // source wants an end-to-end ACK
};

// LinkHeader.flags bits — hop-by-hop ARQ (§5).
enum LinkFlags : uint8_t {
    LINK_FLAG_ACK_REQ = 1 << 0,  // this hop wants a link-layer ACK
    LINK_FLAG_IS_ACK  = 1 << 1,  // this frame *is* a link-layer ACK
};

#if defined(__GNUC__)
#  define AGN_PACKED __attribute__((packed))
#else
#  define AGN_PACKED
#endif

// --- Link header (per hop) -------------------------------------------------
// Rewritten at every relay. `prev_hop`/`next_hop` are the negotiated link-local
// aliases; `link_seq` + `flags` drive hop-by-hop ACK/retry.
struct AGN_PACKED LinkHeader {
    link_addr_t prev_hop;  // alias of the neighbour this frame came from
    link_addr_t next_hop;  // alias of the neighbour this frame is for (BROADCAST for beacons)
    uint8_t     link_seq;  // per-link sequence number for ARQ
    uint8_t     flags;     // LinkFlags
};

// --- Network header (end to end) -------------------------------------------
struct AGN_PACKED NetHeader {
    uint8_t   ver_type;  // high nibble = PROTO_VERSION, low nibble = PacketType
    uint8_t   flags;     // NetFlags
    uint8_t   ttl;       // hop limit, decremented per relay
    node_id_t dst;       // destination node ID (NODE_ID_BROADCAST for flood/beacon)
    node_id_t src;       // originating node ID
    uint16_t  pkt_id;    // dedup + end-to-end ACK correlation
};

static const uint8_t  DEFAULT_TTL = 16;
static const uint16_t MAX_PAYLOAD = 200;  // conservative cap under the LoRa frame limit

// --- Beacon payload (§4 Tier 0: neighbour discovery) -----------------------
// Phase 0 carries only enough to prove the link and print RSSI/SNR. Phase 2 will
// extend this with the per-neighbour bidirectional link metrics and DV updates
// that piggyback on beacons (§6).
struct AGN_PACKED BeaconPayload {
    uint8_t  hw_class;     // sender hardware class (0 = RAK4631) — for per-class power caps
    uint8_t  reserved;     // alignment / future flags
    uint16_t uptime_s;     // sender uptime in seconds (wraps; rough liveness only)
};

// Convenience: assembled header size, asserted to stay in the low double digits.
static const uint16_t HEADER_BYTES = sizeof(LinkHeader) + sizeof(NetHeader);

// Build / read the packed ver_type byte.
static inline uint8_t net_ver_type(PacketType t) {
    return (uint8_t)((PROTO_VERSION << 4) | (t & 0x0F));
}
static inline PacketType net_type_of(uint8_t ver_type) {
    return (PacketType)(ver_type & 0x0F);
}
static inline uint8_t net_ver_of(uint8_t ver_type) {
    return (uint8_t)(ver_type >> 4);
}

// Compile-time guarantees that the structs are truly packed (no padding) and the
// header stays small — if these ever fail, the wire format silently changed.
static_assert(sizeof(LinkHeader) == 4,  "LinkHeader must be 4 bytes on-air");
static_assert(sizeof(NetHeader)  == 13, "NetHeader must be 13 bytes on-air");
static_assert(sizeof(BeaconPayload) == 4, "BeaconPayload must be 4 bytes on-air");
static_assert(HEADER_BYTES < 20, "combined header must stay in the low double digits (§5)");
