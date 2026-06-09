// mesh_types.h — minimal shared types for the routing core.
//
// The routing library is deliberately self-contained: it does NOT depend on the
// firmware's wire-format header (packet.h), so it builds standalone for host unit
// tests and drops into any transport. node_id_t matches packet.h's definition
// (identical typedef — safe to include both).
#pragma once

#include <stdint.h>

// 4-byte node ID, derived from the node's public key (self-certifying, Agent.md §3).
typedef uint32_t node_id_t;

// 1-byte link-local neighbour alias (Agent.md §5). Identical typedef to packet.h's,
// so both headers can be included together.
typedef uint8_t link_addr_t;
