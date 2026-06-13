// Cross-implementation parity: the Go controller's signer (controller/internal/sign)
// must produce control messages this firmware accepts — byte-for-byte. Both sides use
// RFC 8032 Ed25519 (Go crypto/ed25519 ; firmware monocypher-ed25519) and the 64-byte
// secret key is seed‖pub in both, so for a fixed seed the messages are identical.
//
// The vector below was emitted by `go test ./internal/sign -run EmitVector` with
// seed[i] = i*7+1, CTRL_POWER, target=0x9828F51B, arg=10, counter=42. If the wire format
// or signing ever diverges, this test fails — the Phase 4b signature-parity gate.
//
//   pio test -e native -f test_ctrl_interop

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "control.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

using namespace mesh;

// --- vector from the Go signer (hex) ---
static const char* GO_MSG_HEX =
    "01011bf528980a2a000000"
    "f0bca74062ccf86a999125664b11b047f2e1e4f79a056d4248cfb9fa1152698e"
    "6f9ba1f06eb5ee99bd78b7115d215752476ba19b4a5325634b132dba8f7b3102";
static const char* GO_PUB_HEX =
    "e4030998cfd5ad1723c169f956aa0b9eb8619b5992bd612c2af428ebc79f8df0";
// BLOCK vector: target=0x9828F51B blocks victim=0x1FAE0DBD, ttl=30min, counter=7.
static const char* GO_BLK_HEX =
    "01031bf528981ebd0dae1f07000000"
    "4c7a503430441861b3a3caeaa1d655e6f4eb05fee349806246d194582b15f50f"
    "0d5513fef38988c9abc3581cefceda89fa35740c6d917155e78d5124a9a85f0f";

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

// 1) Same seed -> same public key in monocypher and Go (key-derivation parity).
static void test_pubkey_parity() {
    uint8_t goPub[32];
    hex2bin(GO_PUB_HEX, goPub, 32);
    TEST_ASSERT_EQUAL_MEMORY(goPub, PK, 32);
}

// 2) The firmware's ctrl_build output is byte-identical to the Go message
//    (layout + deterministic signature parity).
static void test_message_byte_identical() {
    uint8_t m[CTRL_MSG_BYTES];
    uint16_t n = ctrl_build(CTRL_POWER, 0x9828F51Bu, 10, 42, SK, m, sizeof(m));
    TEST_ASSERT_EQUAL_UINT16(CTRL_MSG_BYTES, n);
    uint8_t goMsg[CTRL_MSG_BYTES];
    hex2bin(GO_MSG_HEX, goMsg, CTRL_MSG_BYTES);
    TEST_ASSERT_EQUAL_MEMORY(goMsg, m, CTRL_MSG_BYTES);
}

// 3) The Go-built message verifies on-device (the real acceptance path).
static void test_go_message_verifies() {
    uint8_t goMsg[CTRL_MSG_BYTES];
    hex2bin(GO_MSG_HEX, goMsg, CTRL_MSG_BYTES);
    CtrlMsg out;
    TEST_ASSERT_EQUAL_UINT8(CTRL_OK, ctrl_verify(goMsg, CTRL_MSG_BYTES, PK, 41, &out));
    TEST_ASSERT_EQUAL_UINT8(CTRL_POWER, out.cmd);
    TEST_ASSERT_EQUAL_HEX32(0x9828F51Bu, out.target);
    TEST_ASSERT_EQUAL_INT8(10, out.arg);
    TEST_ASSERT_EQUAL_UINT32(42, out.counter);
}

// 4) The firmware's ctrl_build_block output is byte-identical to the Go BLOCK message.
static void test_block_byte_identical() {
    uint8_t m[CTRL_BLK_BYTES];
    uint16_t n = ctrl_build_block(CTRL_BLOCK, 0x9828F51Bu, 0x1FAE0DBDu, 30, 7, SK, m, sizeof(m));
    TEST_ASSERT_EQUAL_UINT16(CTRL_BLK_BYTES, n);
    uint8_t goBlk[CTRL_BLK_BYTES];
    hex2bin(GO_BLK_HEX, goBlk, CTRL_BLK_BYTES);
    TEST_ASSERT_EQUAL_MEMORY(goBlk, m, CTRL_BLK_BYTES);
}

// 5) The Go BLOCK message verifies on-device and decodes recipient + victim + ttl.
static void test_block_verifies() {
    uint8_t goBlk[CTRL_BLK_BYTES];
    hex2bin(GO_BLK_HEX, goBlk, CTRL_BLK_BYTES);
    CtrlMsg out;
    TEST_ASSERT_EQUAL_UINT8(CTRL_OK, ctrl_verify(goBlk, CTRL_BLK_BYTES, PK, 6, &out));
    TEST_ASSERT_EQUAL_UINT8(CTRL_BLOCK, out.cmd);
    TEST_ASSERT_EQUAL_HEX32(0x9828F51Bu, out.target);   // recipient
    TEST_ASSERT_EQUAL_HEX32(0x1FAE0DBDu, out.aux);      // victim
    TEST_ASSERT_EQUAL_INT8(30, out.arg);                // ttl minutes
    TEST_ASSERT_EQUAL_UINT32(7, out.counter);
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    make_keys();
    UNITY_BEGIN();
    RUN_TEST(test_pubkey_parity);
    RUN_TEST(test_message_byte_identical);
    RUN_TEST(test_go_message_verifies);
    RUN_TEST(test_block_byte_identical);
    RUN_TEST(test_block_verifies);
    return UNITY_END();
}
