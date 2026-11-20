#include "test_harness.h"
#include "hal/hal_midi.h"

#include <limits.h>
#include <stdint.h>

static void test_spsc_empty_and_fifo(void) {
    sp1_midi_spsc_queue_t q;
    sp1_midi_queue_init(&q);

    TEST_ASSERT_EQ_U32(0U, sp1_midi_queue_count(&q));
    TEST_ASSERT_EQ_U32(0U, sp1_midi_queue_dropped(&q));

    sp1_midi_msg_t out;
    TEST_ASSERT(!sp1_midi_queue_pop(&q, &out));

    const sp1_midi_msg_t m1 = sp1_midi_msg_realtime(SP1_MIDI_STATUS_START, 100U);
    const sp1_midi_msg_t m2 = sp1_midi_msg_realtime(SP1_MIDI_STATUS_CLOCK, 200U);
    const sp1_midi_msg_t m3 = sp1_midi_msg_note_on(0U, 60U, 127U, 300U);

    TEST_ASSERT(sp1_midi_queue_push(&q, &m1));
    TEST_ASSERT(sp1_midi_queue_push(&q, &m2));
    TEST_ASSERT(sp1_midi_queue_push(&q, &m3));
    TEST_ASSERT_EQ_U32(3U, sp1_midi_queue_count(&q));

    TEST_ASSERT(sp1_midi_queue_pop(&q, &out));
    TEST_ASSERT_EQ_U32(SP1_MIDI_STATUS_START, out.status);
    TEST_ASSERT_EQ_U32(100U, out.timestamp_us);

    TEST_ASSERT(sp1_midi_queue_pop(&q, &out));
    TEST_ASSERT_EQ_U32(SP1_MIDI_STATUS_CLOCK, out.status);
    TEST_ASSERT_EQ_U32(200U, out.timestamp_us);

    TEST_ASSERT(sp1_midi_queue_pop(&q, &out));
    TEST_ASSERT_EQ_U32(SP1_MIDI_STATUS_NOTE_ON, out.status);
    TEST_ASSERT_EQ_U32(60U, out.data1);
    TEST_ASSERT_EQ_U32(127U, out.data2);
    TEST_ASSERT_EQ_U32(300U, out.timestamp_us);

    TEST_ASSERT_EQ_U32(0U, sp1_midi_queue_count(&q));
    TEST_ASSERT(!sp1_midi_queue_pop(&q, &out));
}

static void test_spsc_capacity_and_overflow_drop(void) {
    sp1_midi_spsc_queue_t q;
    sp1_midi_queue_init(&q);

    for (uint32_t i = 0U; i < SP1_MIDI_QUEUE_CAPACITY; ++i) {
        const sp1_midi_msg_t msg = sp1_midi_msg_cc(0U, (uint8_t)(i & 0x7FU), 100U, i);
        TEST_ASSERT(sp1_midi_queue_push(&q, &msg));
    }
    TEST_ASSERT_EQ_U32(SP1_MIDI_QUEUE_CAPACITY, sp1_midi_queue_count(&q));
    TEST_ASSERT_EQ_U32(0U, sp1_midi_queue_dropped(&q));

    /* Next pushes must fail cleanly without overwriting and increment dropped counter. */
    const sp1_midi_msg_t extra = sp1_midi_msg_realtime(SP1_MIDI_STATUS_CLOCK, 999U);
    TEST_ASSERT(!sp1_midi_queue_push(&q, &extra));
    TEST_ASSERT(!sp1_midi_queue_push(&q, &extra));
    TEST_ASSERT_EQ_U32(2U, sp1_midi_queue_dropped(&q));
    TEST_ASSERT_EQ_U32(SP1_MIDI_QUEUE_CAPACITY, sp1_midi_queue_count(&q));

    /* Drain all 64 and verify FIFO preservation. */
    for (uint32_t i = 0U; i < SP1_MIDI_QUEUE_CAPACITY; ++i) {
        sp1_midi_msg_t out;
        TEST_ASSERT(sp1_midi_queue_pop(&q, &out));
        TEST_ASSERT_EQ_U32(i, out.timestamp_us);
        TEST_ASSERT_EQ_U32(i & 0x7FU, out.data1);
    }
    TEST_ASSERT_EQ_U32(0U, sp1_midi_queue_count(&q));
}

static void test_spsc_ring_and_uint32_wraparound(void) {
    sp1_midi_spsc_queue_t q;
    sp1_midi_queue_init(&q);

    /* Multi-cycle ring buffer wrap-around (> 16 full revolutions). */
    for (uint32_t cycle = 0U; cycle < 1024U; ++cycle) {
        const sp1_midi_msg_t in =
            sp1_midi_msg_note_on(0U, (uint8_t)(cycle & 0x7FU), 90U, cycle * 10U);
        TEST_ASSERT(sp1_midi_queue_push(&q, &in));

        sp1_midi_msg_t out;
        TEST_ASSERT(sp1_midi_queue_pop(&q, &out));
        TEST_ASSERT_EQ_U32(cycle * 10U, out.timestamp_us);
        TEST_ASSERT_EQ_U32(cycle & 0x7FU, out.data1);
    }
    TEST_ASSERT_EQ_U32(0U, sp1_midi_queue_count(&q));
    TEST_ASSERT_EQ_U32(0U, sp1_midi_queue_dropped(&q));

    /* Explicit uint32_t overflow boundary test (UINT32_MAX - 5 -> wraps through 0). */
    q.head = UINT32_MAX - 5U;
    q.tail = UINT32_MAX - 5U;
    TEST_ASSERT_EQ_U32(0U, sp1_midi_queue_count(&q));

    for (uint32_t i = 0U; i < SP1_MIDI_QUEUE_CAPACITY; ++i) {
        const sp1_midi_msg_t in = sp1_midi_msg_realtime(SP1_MIDI_STATUS_CLOCK, 5000U + i);
        TEST_ASSERT(sp1_midi_queue_push(&q, &in));
        TEST_ASSERT_EQ_U32(i + 1U, sp1_midi_queue_count(&q));
    }

    /* Queue is now full across UINT32_MAX wrap boundary. */
    const sp1_midi_msg_t overflow_msg = sp1_midi_msg_realtime(SP1_MIDI_STATUS_STOP, 0U);
    TEST_ASSERT(!sp1_midi_queue_push(&q, &overflow_msg));
    TEST_ASSERT_EQ_U32(1U, sp1_midi_queue_dropped(&q));

    for (uint32_t i = 0U; i < SP1_MIDI_QUEUE_CAPACITY; ++i) {
        sp1_midi_msg_t out;
        TEST_ASSERT(sp1_midi_queue_pop(&q, &out));
        TEST_ASSERT_EQ_U32(5000U + i, out.timestamp_us);
    }
    TEST_ASSERT_EQ_U32(0U, sp1_midi_queue_count(&q));
}

static void test_ump32_roundtrip(void) {
    const sp1_midi_msg_t cases[] = {
        sp1_midi_msg_realtime(SP1_MIDI_STATUS_CLOCK, 111U),
        sp1_midi_msg_realtime(SP1_MIDI_STATUS_START, 222U),
        sp1_midi_msg_realtime(SP1_MIDI_STATUS_CONTINUE, 333U),
        sp1_midi_msg_realtime(SP1_MIDI_STATUS_STOP, 444U),
        sp1_midi_msg_note_on(0U, 60U, 110U, 555U),
        sp1_midi_msg_note_off(3U, 63U, 0U, 666U),
        sp1_midi_msg_cc(9U, 74U, 127U, 777U),
    };

    const uint32_t num_cases = (uint32_t)(sizeof(cases) / sizeof(cases[0]));
    for (uint32_t i = 0U; i < num_cases; ++i) {
        uint32_t word0 = 0U;
        TEST_ASSERT(sp1_midi_encode_ump32(&cases[i], 0U, &word0));

        sp1_midi_msg_t decoded;
        TEST_ASSERT(sp1_midi_decode_ump32(word0, cases[i].timestamp_us, &decoded));
        TEST_ASSERT_EQ_U32(cases[i].timestamp_us, decoded.timestamp_us);
        TEST_ASSERT_EQ_U32(cases[i].status, decoded.status);
        TEST_ASSERT_EQ_U32(cases[i].data1, decoded.data1);
        TEST_ASSERT_EQ_U32(cases[i].data2, decoded.data2);
        TEST_ASSERT_EQ_U32(cases[i].length, decoded.length);
    }

    /* Unsupported UMP message type (e.g., MT=0xF Stream) must return false cleanly. */
    sp1_midi_msg_t ignored;
    TEST_ASSERT(!sp1_midi_decode_ump32(0xF0000000U, 0U, &ignored));
}

int main(void) {
    RUN_TEST(test_spsc_empty_and_fifo);
    RUN_TEST(test_spsc_capacity_and_overflow_drop);
    RUN_TEST(test_spsc_ring_and_uint32_wraparound);
    RUN_TEST(test_ump32_roundtrip);
    return sp1_test_summary();
}
