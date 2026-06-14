// Cross-implementation parity: the Go controller's signer (controller/internal/sign)
// must produce control messages this firmware accepts — byte-for-byte. Both sides use
// RFC 8032 Ed25519 (Go crypto/ed25519 ; firmware monocypher-ed25519) and the 64-byte
// secret key is seed‖pub in both, so for a fixed seed the messages are identical.
//
//   pio test -e native -f test_ctrl_interop
//
// ⚠️ v2 (16-byte node ids) NOTE: the byte-for-byte Go vectors below were emitted for the
// v1 4-byte-target wire format and are STALE — the firmware now uses 16-byte targets
// (CTRL_VER=2). The controller's Go signer must be updated to v2 and the vectors
// regenerated via `go test ./internal/sign -run EmitVector`, then the byte-identical
// gate restored (see test_message_byte_identical_v2 below, currently a firmware
// sign->verify round-trip). The KEY-DERIVATION parity (seed -> pubkey) is wire-format
// independent and is still pinned against the Go vector.

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "control.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

using namespace mesh;

// seed[i] = i*7+1 -> this Ed25519 public key (Go crypto/ed25519, wire-format independent).
static const char* GO_PUB_HEX =
    "e4030998cfd5ad1723c169f956aa0b9eb8619b5992bd612c2af428ebc79f8df0";

static void hex2bin(const char* h, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned b = 0;
        sscanf(h + 2 * i, "%2x", &b);
        out[i] = (uint8_t)b;
    }
}

static uint8_t SK[64], PK[32];

static void make_keys() {
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 1);
    crypto_ed25519_key_pair(SK, PK, seed); // consumes (wipes) seed
}

// A representative 16-byte target/victim for the round-trip tests.
static NodeId target_id() { return nid_from_u32(0x9828F51Bu); }
static NodeId victim_id() { return nid_from_u32(0x1FAE0DBDu); }

// 1) Same seed -> same public key in monocypher and Go (key-derivation parity).
//    This is independent of the v1->v2 wire change and remains a real cross-impl gate.
static void test_pubkey_parity() {
    uint8_t goPub[32];
    hex2bin(GO_PUB_HEX, goPub, 32);
    TEST_ASSERT_EQUAL_MEMORY(goPub, PK, 32);
}

// 2) POWER round-trip: ctrl_build output verifies and decodes its fields.
//    TODO(controller-v2): restore byte-for-byte equality against a regenerated Go vector.
static void test_message_byte_identical_v2() {
    uint8_t m[CTRL_MSG_BYTES];
    uint16_t n = ctrl_build(CTRL_POWER, target_id(), 10, 42, SK, m, sizeof(m));
    TEST_ASSERT_EQUAL_UINT16(CTRL_MSG_BYTES, n);

    CtrlMsg out;
    TEST_ASSERT_EQUAL_UINT8(CTRL_OK, ctrl_verify(m, n, PK, 41, &out));
    TEST_ASSERT_EQUAL_UINT8(CTRL_POWER, out.cmd);
    TEST_ASSERT_TRUE(out.target == target_id());
    TEST_ASSERT_EQUAL_INT8(10, out.arg);
    TEST_ASSERT_EQUAL_UINT32(42, out.counter);
}

// 3) A tampered byte fails verification (signature integrity).
static void test_tamper_rejected() {
    uint8_t m[CTRL_MSG_BYTES];
    ctrl_build(CTRL_POWER, target_id(), 10, 42, SK, m, sizeof(m));
    m[6] ^= 0x01;                                  // flip a target byte
    CtrlMsg out;
    TEST_ASSERT_EQUAL_UINT8(CTRL_BAD_SIG, ctrl_verify(m, CTRL_MSG_BYTES, PK, 41, &out));
}

// 4) BLOCK round-trip: recipient + victim + ttl decode correctly.
//    TODO(controller-v2): restore byte-for-byte equality against a regenerated Go vector.
static void test_block_byte_identical_v2() {
    uint8_t m[CTRL_BLK_BYTES];
    uint16_t n = ctrl_build_block(CTRL_BLOCK, target_id(), victim_id(), 30, 7, SK, m, sizeof(m));
    TEST_ASSERT_EQUAL_UINT16(CTRL_BLK_BYTES, n);

    CtrlMsg out;
    TEST_ASSERT_EQUAL_UINT8(CTRL_OK, ctrl_verify(m, n, PK, 6, &out));
    TEST_ASSERT_EQUAL_UINT8(CTRL_BLOCK, out.cmd);
    TEST_ASSERT_TRUE(out.target == target_id());   // recipient
    TEST_ASSERT_TRUE(out.aux == victim_id());       // victim
    TEST_ASSERT_EQUAL_INT8(30, out.arg);            // ttl minutes
    TEST_ASSERT_EQUAL_UINT32(7, out.counter);
}

// 4b) CTRL_BLE (remote BLE toggle) shares the POWER-style layout: build -> verify -> arg.
static void test_ble_cmd_roundtrip() {
    uint8_t m[CTRL_MSG_BYTES];
    uint16_t n = ctrl_build(CTRL_BLE, target_id(), 1, 99, SK, m, sizeof(m));  // arg 1 = enable
    TEST_ASSERT_EQUAL_UINT16(CTRL_MSG_BYTES, n);
    CtrlMsg out;
    TEST_ASSERT_EQUAL_UINT8(CTRL_OK, ctrl_verify(m, n, PK, 98, &out));
    TEST_ASSERT_EQUAL_UINT8(CTRL_BLE, out.cmd);
    TEST_ASSERT_TRUE(out.target == target_id());
    TEST_ASSERT_EQUAL_INT8(1, out.arg);
}

// 4c) CTRL_RETUNE: build -> verify, the 13-byte PHY blob round-trips, tamper is caught.
static void test_retune_cmd_roundtrip() {
    uint8_t cfg[CTRL_RETUNE_CFG];
    uint32_t freq = 904375000u, bw = 250000u;
    cfg[0] = (uint8_t)freq; cfg[1] = (uint8_t)(freq >> 8); cfg[2] = (uint8_t)(freq >> 16); cfg[3] = (uint8_t)(freq >> 24);
    cfg[4] = (uint8_t)bw;   cfg[5] = (uint8_t)(bw >> 8);   cfg[6] = (uint8_t)(bw >> 16);   cfg[7] = (uint8_t)(bw >> 24);
    cfg[8] = 11; cfg[9] = 5; cfg[10] = 0x4D; cfg[11] = 16; cfg[12] = 0;   // sf, cr, sync, preamble(LE)

    uint8_t m[CTRL_RTN_BYTES];
    uint16_t n = ctrl_build_retune(target_id(), cfg, 123, SK, m, sizeof(m));
    TEST_ASSERT_EQUAL_UINT16(CTRL_RTN_BYTES, n);

    CtrlMsg out;
    TEST_ASSERT_EQUAL_UINT8(CTRL_OK, ctrl_verify(m, n, PK, 122, &out));
    TEST_ASSERT_EQUAL_UINT8(CTRL_RETUNE, out.cmd);
    TEST_ASSERT_TRUE(out.target == target_id());
    TEST_ASSERT_EQUAL_MEMORY(cfg, out.cfg, CTRL_RETUNE_CFG);   // PHY blob survives

    m[20] ^= 0x01;                                             // flip a cfg byte (signed region)
    TEST_ASSERT_EQUAL_UINT8(CTRL_BAD_SIG, ctrl_verify(m, n, PK, 122, &out));
}

// 5) Replay floor: a counter at/below the floor is rejected after the sig checks out.
static void test_replay_rejected() {
    uint8_t m[CTRL_MSG_BYTES];
    ctrl_build(CTRL_POWER, target_id(), 10, 42, SK, m, sizeof(m));
    CtrlMsg out;
    TEST_ASSERT_EQUAL_UINT8(CTRL_REPLAY, ctrl_verify(m, CTRL_MSG_BYTES, PK, 42, &out));
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    make_keys();
    UNITY_BEGIN();
    RUN_TEST(test_pubkey_parity);
    RUN_TEST(test_message_byte_identical_v2);
    RUN_TEST(test_tamper_rejected);
    RUN_TEST(test_block_byte_identical_v2);
    RUN_TEST(test_ble_cmd_roundtrip);
    RUN_TEST(test_retune_cmd_roundtrip);
    RUN_TEST(test_replay_rejected);
    return UNITY_END();
}
