// Host unit tests for the KISS TNC framing (lib/mesh/kiss.h).
//
//   pio test -e native

#include <unity.h>
#include <string.h>
#include "kiss.h"

using namespace mesh;

// Helper: run a byte buffer through a decoder, return frames found.
static int pump(KissDecoder& d, const uint8_t* b, uint16_t n,
                uint8_t out[][KISS_MAX_FRAME], uint16_t* lens, int cap) {
    int found = 0;
    for (uint16_t i = 0; i < n; i++)
        if (d.feed(b[i]) && found < cap) {
            memcpy(out[found], d.frame(), d.len());
            lens[found] = d.len();
            found++;
        }
    return found;
}

static void test_roundtrip_with_escapes() {
    // payload containing both special bytes
    uint8_t pay[6] = { 'h', KISS_FEND, 'i', KISS_FESC, '!', 0x00 };
    uint8_t enc[32];
    uint16_t n = kiss_encode(KISS_CMD_DATA, pay, sizeof(pay), enc, sizeof(enc));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_UINT8(KISS_FEND, enc[0]);
    TEST_ASSERT_EQUAL_UINT8(KISS_FEND, enc[n - 1]);

    KissDecoder d;
    uint8_t frames[2][KISS_MAX_FRAME]; uint16_t lens[2];
    TEST_ASSERT_EQUAL_INT(1, pump(d, enc, n, frames, lens, 2));
    TEST_ASSERT_EQUAL_UINT16(1 + sizeof(pay), lens[0]);
    TEST_ASSERT_EQUAL_UINT8(KISS_CMD_DATA, frames[0][0]);
    TEST_ASSERT_EQUAL_MEMORY(pay, frames[0] + 1, sizeof(pay));
}

static void test_back_to_back_frames_share_fend() {
    // ...FEND data1 FEND data2 FEND... — the middle FEND closes one and opens the next
    uint8_t a[4], b[4];
    uint16_t na = kiss_encode(0x00, (const uint8_t*)"A", 1, a, sizeof(a));
    uint16_t nb = kiss_encode(0x00, (const uint8_t*)"B", 1, b, sizeof(b));
    uint8_t stream[8]; memcpy(stream, a, na); memcpy(stream + na, b, nb);
    KissDecoder d;
    uint8_t frames[3][KISS_MAX_FRAME]; uint16_t lens[3];
    TEST_ASSERT_EQUAL_INT(2, pump(d, stream, (uint16_t)(na + nb), frames, lens, 3));
    TEST_ASSERT_EQUAL_UINT8('A', frames[0][1]);
    TEST_ASSERT_EQUAL_UINT8('B', frames[1][1]);
}

static void test_garbage_between_frames_ignored() {
    uint8_t enc[16];
    uint16_t n = kiss_encode(0x00, (const uint8_t*)"ok", 2, enc, sizeof(enc));
    uint8_t stream[64]; uint16_t sn = 0;
    const char* noise = "[hb] up=12s console text\n";
    memcpy(stream, noise, strlen(noise)); sn += strlen(noise);
    memcpy(stream + sn, enc, n); sn += n;
    KissDecoder d;
    uint8_t frames[2][KISS_MAX_FRAME]; uint16_t lens[2];
    TEST_ASSERT_EQUAL_INT(1, pump(d, stream, sn, frames, lens, 2));
    TEST_ASSERT_EQUAL_UINT8('o', frames[0][1]);
}

static void test_exit_command() {
    uint8_t enc[8];
    uint16_t n = kiss_encode(KISS_CMD_RETURN, nullptr, 0, enc, sizeof(enc));
    KissDecoder d;
    uint8_t frames[2][KISS_MAX_FRAME]; uint16_t lens[2];
    TEST_ASSERT_EQUAL_INT(1, pump(d, enc, n, frames, lens, 2));
    TEST_ASSERT_EQUAL_UINT16(1, lens[0]);
    TEST_ASSERT_EQUAL_UINT8(KISS_CMD_RETURN, frames[0][0]);
}

static void test_encode_cap_too_small() {
    uint8_t pay[4] = { KISS_FEND, KISS_FEND, KISS_FEND, KISS_FEND };  // worst case 2x
    uint8_t enc[8];   // needs 2 + 8 + 1 = 11
    TEST_ASSERT_EQUAL_UINT16(0, kiss_encode(0x00, pay, sizeof(pay), enc, sizeof(enc)));
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_with_escapes);
    RUN_TEST(test_back_to_back_frames_share_fend);
    RUN_TEST(test_garbage_between_frames_ignored);
    RUN_TEST(test_exit_command);
    RUN_TEST(test_encode_cap_too_small);
    return UNITY_END();
}
