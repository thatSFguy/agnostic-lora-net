// Host unit tests for the signed control plane (lib/mesh/control + monocypher).
//
//   pio test -e native

#include <unity.h>
#include <string.h>
#include "control.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

using namespace mesh;

static uint8_t SK[64], PK[32];          // controller keypair
static uint8_t SK2[64], PK2[32];        // an attacker's keypair

static void make_keys() {
    uint8_t seed[32], seed2[32];
    for (int i = 0; i < 32; i++) { seed[i] = (uint8_t)(i * 7 + 1); seed2[i] = (uint8_t)(i * 13 + 5); }
    crypto_ed25519_key_pair(SK, PK, seed);     // NOTE: consumes (wipes) the seed
    crypto_ed25519_key_pair(SK2, PK2, seed2);
}

static void test_sign_verify_roundtrip() {
    uint8_t m[CTRL_MSG_BYTES];
    uint16_t n = ctrl_build(CTRL_POWER, nid_from_u32(0x9828F51Bu), 10, 42, SK, m, sizeof(m));
    TEST_ASSERT_EQUAL_UINT16(CTRL_MSG_BYTES, n);
    CtrlMsg out;
    TEST_ASSERT_EQUAL_UINT8(CTRL_OK, ctrl_verify(m, n, PK, 41, &out));
    TEST_ASSERT_EQUAL_UINT8(CTRL_POWER, out.cmd);
    TEST_ASSERT_TRUE(out.target == nid_from_u32(0x9828F51Bu));
    TEST_ASSERT_EQUAL_INT8(10, out.arg);
    TEST_ASSERT_EQUAL_UINT32(42, out.counter);
}

static void test_replay_rejected() {
    uint8_t m[CTRL_MSG_BYTES];
    uint16_t n = ctrl_build(CTRL_POWER, nid_from_u32(0xD97EEC3Au), 22, 100, SK, m, sizeof(m));
    // floor == counter -> replay; floor > counter -> replay
    TEST_ASSERT_EQUAL_UINT8(CTRL_REPLAY, ctrl_verify(m, n, PK, 100, nullptr));
    TEST_ASSERT_EQUAL_UINT8(CTRL_REPLAY, ctrl_verify(m, n, PK, 500, nullptr));
    TEST_ASSERT_EQUAL_UINT8(CTRL_OK,     ctrl_verify(m, n, PK, 99,  nullptr));
}

static void test_tamper_rejected() {
    uint8_t m[CTRL_MSG_BYTES];
    uint16_t n = ctrl_build(CTRL_POWER, nid_from_u32(0xD97EEC3Au), 10, 7, SK, m, sizeof(m));
    m[18] = (uint8_t)22;                             // attacker edits power 10 -> 22 (arg @ ver|cmd|target[16])
    TEST_ASSERT_EQUAL_UINT8(CTRL_BAD_SIG, ctrl_verify(m, n, PK, 0, nullptr));
    // bit-flip the counter (would advance the replay floor if accepted)
    n = ctrl_build(CTRL_POWER, nid_from_u32(0xD97EEC3Au), 10, 7, SK, m, sizeof(m));
    m[19] ^= 0x01;                                   // counter byte 0 @ ver|cmd|target[16]|arg
    TEST_ASSERT_EQUAL_UINT8(CTRL_BAD_SIG, ctrl_verify(m, n, PK, 0, nullptr));
}

static void test_wrong_key_rejected() {
    uint8_t m[CTRL_MSG_BYTES];
    uint16_t n = ctrl_build(CTRL_POWER, nid_from_u32(0xD97EEC3Au), 10, 7, SK2, m, sizeof(m));  // attacker signs
    TEST_ASSERT_EQUAL_UINT8(CTRL_BAD_SIG, ctrl_verify(m, n, PK, 0, nullptr));    // node holds PK
}

static void test_ack_roundtrip_and_discrimination() {
    uint8_t a[CTRL_ACK_BYTES];
    uint16_t n = ctrl_build_ack(CTRL_POWER, nid_from_u32(0x1FAE0DBDu), 10, 1, 42, a, sizeof(a));
    TEST_ASSERT_EQUAL_UINT16(CTRL_ACK_BYTES, n);
    TEST_ASSERT_TRUE(ctrl_is_ack(a, n));
    CtrlAck out;
    TEST_ASSERT_TRUE(ctrl_parse_ack(a, n, &out));
    TEST_ASSERT_EQUAL_UINT8(CTRL_POWER, out.cmd);
    TEST_ASSERT_TRUE(out.origin == nid_from_u32(0x1FAE0DBDu));
    TEST_ASSERT_EQUAL_INT8(10, out.applied);
    TEST_ASSERT_EQUAL_UINT8(1, out.provisional);
    // a command is not an ack
    uint8_t m[CTRL_MSG_BYTES];
    ctrl_build(CTRL_POWER, nid_from_u32(1), 10, 1, SK, m, sizeof(m));
    TEST_ASSERT_FALSE(ctrl_is_ack(m, CTRL_MSG_BYTES));
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    make_keys();
    UNITY_BEGIN();
    RUN_TEST(test_sign_verify_roundtrip);
    RUN_TEST(test_replay_rejected);
    RUN_TEST(test_tamper_rejected);
    RUN_TEST(test_wrong_key_rejected);
    RUN_TEST(test_ack_roundtrip_and_discrimination);
    return UNITY_END();
}
