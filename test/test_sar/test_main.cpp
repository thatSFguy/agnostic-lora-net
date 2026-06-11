// Host unit tests for segmentation/reassembly (lib/mesh/sar).
//
//   pio test -e native

#include <unity.h>
#include "sar.h"

using namespace mesh;

// A deterministic "file" of `n` bytes.
static void make_blob(uint8_t* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)(i * 31 + 7);
}

// CRC-32 must match zlib's (so the laptop can verify what the node computed).
static void test_crc32_known_answer() {
    const uint8_t* s = (const uint8_t*)"123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, sar_crc32(s, 9));  // canonical CRC-32 check value
}

// Fragment a blob and reassemble it in order.
static void test_roundtrip_in_order() {
    uint8_t blob[500];
    make_blob(blob, sizeof(blob));
    uint32_t crc = sar_crc32(blob, sizeof(blob));
    uint16_t n = sar_frag_count(sizeof(blob));

    SarReassembler r;
    uint8_t frag[200];
    for (uint16_t i = 0; i < n; i++) {
        uint16_t flen = sar_build_fragment(blob, sizeof(blob), 0x1234, crc, i, frag, sizeof(frag));
        TEST_ASSERT_TRUE(flen > 0);
        TEST_ASSERT_TRUE(r.add(frag, flen));
    }
    TEST_ASSERT_TRUE(r.complete());
    TEST_ASSERT_TRUE(r.verify());
    TEST_ASSERT_EQUAL_UINT32(sizeof(blob), r.total_len());
    TEST_ASSERT_EQUAL_MEMORY(blob, r.data(), sizeof(blob));
}

// Reassembly must not care about arrival order.
static void test_out_of_order() {
    uint8_t blob[400];
    make_blob(blob, sizeof(blob));
    uint32_t crc = sar_crc32(blob, sizeof(blob));
    uint16_t n = sar_frag_count(sizeof(blob));

    SarReassembler r;
    uint8_t frag[200];
    for (int i = (int)n - 1; i >= 0; i--) {   // reverse order
        uint16_t flen = sar_build_fragment(blob, sizeof(blob), 7, crc, (uint16_t)i, frag, sizeof(frag));
        TEST_ASSERT_TRUE(r.add(frag, flen));
    }
    TEST_ASSERT_TRUE(r.verify());
    TEST_ASSERT_EQUAL_MEMORY(blob, r.data(), sizeof(blob));
}

// A missing fragment leaves the transfer incomplete (and unverifiable).
static void test_missing_fragment() {
    uint8_t blob[500];
    make_blob(blob, sizeof(blob));
    uint32_t crc = sar_crc32(blob, sizeof(blob));
    uint16_t n = sar_frag_count(sizeof(blob));

    SarReassembler r;
    uint8_t frag[200];
    for (uint16_t i = 0; i < n; i++) {
        if (i == 1) continue;   // drop fragment 1
        uint16_t flen = sar_build_fragment(blob, sizeof(blob), 9, crc, i, frag, sizeof(frag));
        r.add(frag, flen);
    }
    TEST_ASSERT_FALSE(r.complete());
    TEST_ASSERT_FALSE(r.verify());
    TEST_ASSERT_EQUAL_UINT16(n - 1, r.got_count());
}

// Corrupted content reassembles fully but fails the CRC check.
static void test_corruption_caught() {
    uint8_t blob[300];
    make_blob(blob, sizeof(blob));
    uint32_t crc = sar_crc32(blob, sizeof(blob));
    uint16_t n = sar_frag_count(sizeof(blob));

    SarReassembler r;
    uint8_t frag[200];
    for (uint16_t i = 0; i < n; i++) {
        uint16_t flen = sar_build_fragment(blob, sizeof(blob), 3, crc, i, frag, sizeof(frag));
        if (i == 0) frag[SAR_HDR_BYTES] ^= 0xFF;   // flip a payload byte in transit
        r.add(frag, flen);
    }
    TEST_ASSERT_TRUE(r.complete());     // all fragments arrived
    TEST_ASSERT_FALSE(r.verify());      // but the CRC catches the bit flip
}

// End-to-end recovery: drop fragments, the receiver reports them, a NACK carries
// the list, the sender resends exactly those, and the transfer completes.
static void test_nack_recovery() {
    uint8_t blob[700];
    make_blob(blob, sizeof(blob));
    uint32_t crc = sar_crc32(blob, sizeof(blob));
    uint16_t n = sar_frag_count(sizeof(blob));

    SarReassembler r;
    uint8_t frag[200];
    // First pass: drop fragments 2 and 4.
    for (uint16_t i = 0; i < n; i++) {
        if (i == 2 || i == 4) continue;
        uint16_t flen = sar_build_fragment(blob, sizeof(blob), 0x55, crc, i, frag, sizeof(frag));
        r.add(frag, flen);
    }
    TEST_ASSERT_FALSE(r.complete());

    // Receiver computes its missing list and packs a NACK.
    uint16_t miss[16];
    uint16_t nm = r.missing(miss, 16);
    TEST_ASSERT_EQUAL_UINT16(2, nm);
    TEST_ASSERT_EQUAL_UINT16(2, miss[0]);
    TEST_ASSERT_EQUAL_UINT16(4, miss[1]);

    uint8_t nack[64];
    uint16_t nlen = sar_build_nack(r.xfer_id(), miss, nm, nack, sizeof(nack));

    // Sender parses the NACK and resends exactly those fragments.
    uint16_t xid; uint16_t req[16];
    uint16_t nr = sar_parse_nack(nack, nlen, &xid, req, 16);
    TEST_ASSERT_EQUAL_UINT16(0x55, xid);
    TEST_ASSERT_EQUAL_UINT16(2, nr);
    for (uint16_t i = 0; i < nr; i++) {
        uint16_t flen = sar_build_fragment(blob, sizeof(blob), 0x55, crc, req[i], frag, sizeof(frag));
        r.add(frag, flen);
    }

    TEST_ASSERT_TRUE(r.verify());
    TEST_ASSERT_EQUAL_MEMORY(blob, r.data(), sizeof(blob));
}

static void test_done_roundtrip() {
    // Receiver confirms a verified transfer; sender matches it to its xfer id.
    uint8_t done[16];
    uint16_t dl = sar_build_done(0xBEEF, done, sizeof(done));
    TEST_ASSERT_EQUAL_UINT16(SAR_DONE_BYTES, dl);
    TEST_ASSERT_TRUE(sar_is_done(done, dl));
    TEST_ASSERT_EQUAL_UINT16(0xBEEF, sar_parse_done(done, dl));

    // Not confusable with the other SAR control messages, or with junk.
    TEST_ASSERT_FALSE(sar_is_nack(done, dl));
    TEST_ASSERT_FALSE(sar_is_fragment(done, dl));
    uint8_t junk[6] = {'S', 'A', 'R', 'X', 0, 0};
    TEST_ASSERT_FALSE(sar_is_done(junk, sizeof(junk)));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, sar_parse_done(junk, sizeof(junk)));
    TEST_ASSERT_FALSE(sar_is_done(done, 3));   // truncated
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_crc32_known_answer);
    RUN_TEST(test_roundtrip_in_order);
    RUN_TEST(test_out_of_order);
    RUN_TEST(test_missing_fragment);
    RUN_TEST(test_corruption_caught);
    RUN_TEST(test_nack_recovery);
    RUN_TEST(test_done_roundtrip);
    return UNITY_END();
}
