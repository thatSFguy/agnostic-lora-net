// Host unit tests for the hop-by-hop link ARQ engine (lib/mesh/link_arq).
//
//   pio test -e native

#include <unity.h>
#include "link_arq.h"

using namespace mesh;

// Captures retransmit callbacks from tick().
struct Cap {
    int      resends  = 0;
    uint16_t last_len = 0;
};
static void cap_fn(void* ctx, const uint8_t* /*frame*/, uint16_t len) {
    Cap* c = (Cap*)ctx;
    c->resends++;
    c->last_len = len;
}

static const uint8_t FRAME[8] = {1, 2, 3, 4, 5, 6, 7, 8};

// An ACK clears the pending frame; nothing is retransmitted afterwards.
static void test_ack_clears_pending() {
    LinkArq arq;
    Cap cap;
    uint8_t seq = arq.next_seq();
    TEST_ASSERT_TRUE(arq.track(seq, 0xB, FRAME, sizeof(FRAME), 1000));
    TEST_ASSERT_EQUAL_UINT8(1, arq.pending_count());

    TEST_ASSERT_TRUE(arq.on_ack(seq));
    TEST_ASSERT_EQUAL_UINT8(0, arq.pending_count());
    TEST_ASSERT_EQUAL_UINT32(1, arq.acked_count());

    arq.tick(1000000, cap_fn, &cap);     // long after — nothing pending
    TEST_ASSERT_EQUAL_INT(0, cap.resends);
}

// A frame with no ACK is retransmitted once its timeout elapses, not before.
static void test_timeout_triggers_resend() {
    LinkArq arq;
    Cap cap;
    uint8_t seq = arq.next_seq();
    arq.track(seq, 0xB, FRAME, sizeof(FRAME), 1000, /*timeout*/500, /*retries*/3);

    arq.tick(1499, cap_fn, &cap);        // not due yet
    TEST_ASSERT_EQUAL_INT(0, cap.resends);

    arq.tick(1500, cap_fn, &cap);        // due
    TEST_ASSERT_EQUAL_INT(1, cap.resends);
    TEST_ASSERT_EQUAL_UINT16(sizeof(FRAME), cap.last_len);
    TEST_ASSERT_EQUAL_UINT8(1, arq.pending_count());   // still waiting
}

// After exhausting retries the frame is given up (dropped), not resent forever.
static void test_retries_exhaust_then_drop() {
    LinkArq arq;
    Cap cap;
    uint8_t seq = arq.next_seq();
    arq.track(seq, 0xB, FRAME, sizeof(FRAME), 0, /*timeout*/100, /*retries*/2);

    TEST_ASSERT_EQUAL_UINT8(0, arq.tick(100, cap_fn, &cap));  // resend 1
    TEST_ASSERT_EQUAL_UINT8(0, arq.tick(200, cap_fn, &cap));  // resend 2
    TEST_ASSERT_EQUAL_UINT8(1, arq.tick(300, cap_fn, &cap));  // give up -> 1 dropped

    TEST_ASSERT_EQUAL_INT(2, cap.resends);
    TEST_ASSERT_EQUAL_UINT8(0, arq.pending_count());
    TEST_ASSERT_EQUAL_UINT32(1, arq.dropped_count());
}

// An ACK for an unknown sequence does nothing and leaves real pending intact.
static void test_unmatched_ack_ignored() {
    LinkArq arq;
    uint8_t seq = arq.next_seq();
    arq.track(seq, 0xB, FRAME, sizeof(FRAME), 1000);

    TEST_ASSERT_FALSE(arq.on_ack((uint8_t)(seq + 7)));
    TEST_ASSERT_EQUAL_UINT8(1, arq.pending_count());
    TEST_ASSERT_EQUAL_UINT32(0, arq.acked_count());
}

// The pending table has a fixed capacity; overflow is refused (frame still sent).
static void test_pending_table_full() {
    LinkArq arq;
    for (uint8_t i = 0; i < ARQ_MAX_PENDING; i++) {
        TEST_ASSERT_TRUE(arq.track(arq.next_seq(), 0xB, FRAME, sizeof(FRAME), 1000));
    }
    TEST_ASSERT_FALSE(arq.track(arq.next_seq(), 0xB, FRAME, sizeof(FRAME), 1000));
    TEST_ASSERT_EQUAL_UINT8(ARQ_MAX_PENDING, arq.pending_count());
}

// A late ACK still stops further retransmission.
static void test_ack_stops_resend() {
    LinkArq arq;
    Cap cap;
    uint8_t seq = arq.next_seq();
    arq.track(seq, 0xB, FRAME, sizeof(FRAME), 0, /*timeout*/100, /*retries*/3);

    arq.tick(100, cap_fn, &cap);         // one resend
    TEST_ASSERT_EQUAL_INT(1, cap.resends);
    TEST_ASSERT_TRUE(arq.on_ack(seq));   // ACK arrives
    arq.tick(1000, cap_fn, &cap);        // no more resends
    TEST_ASSERT_EQUAL_INT(1, cap.resends);
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_ack_clears_pending);
    RUN_TEST(test_timeout_triggers_resend);
    RUN_TEST(test_retries_exhaust_then_drop);
    RUN_TEST(test_unmatched_ack_ignored);
    RUN_TEST(test_pending_table_full);
    RUN_TEST(test_ack_stops_resend);
    return UNITY_END();
}
