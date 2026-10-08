// Host tests for the BLE framing (src/nav_frag.c):
//   pio test -e native
#include <unity.h>

#include <stdlib.h>
#include <string.h>

#include "nav_frag.h"

#define FRAME_CAP 600

static nav_frag_t rx;
static uint8_t msg[NAV_FRAG_MAX_MESSAGE];
static uint8_t frames[NAV_FRAG_MAX_CHUNKS][FRAME_CAP];
static size_t frame_len[NAV_FRAG_MAX_CHUNKS];

void setUp(void)
{
    nav_frag_init(&rx);
}

void tearDown(void)
{
}

// Fills msg with a recognisable pattern for message `id`.
static void make_message(size_t len, uint8_t id)
{
    for (size_t i = 0; i < len; i++)
    {
        msg[i] = (uint8_t)(i * 7 + id);
    }
}

// Splits msg into frames[]; returns the frame count.
static uint8_t split(size_t len, uint8_t id, size_t frame_size)
{
    uint8_t count = nav_frag_count(len, frame_size);
    TEST_ASSERT_GREATER_THAN(0, count);
    for (uint8_t i = 0; i < count; i++)
    {
        frame_len[i] = nav_frag_build(msg, len, id, frame_size, i, frames[i], FRAME_CAP);
        TEST_ASSERT_GREATER_THAN(NAV_FRAG_HEADER_SIZE, frame_len[i]);
        TEST_ASSERT_LESS_OR_EQUAL(frame_size, frame_len[i]);
    }
    return count;
}

static nav_frag_result_t push(uint8_t i, const uint8_t **out, size_t *out_len)
{
    return nav_frag_push(&rx, frames[i], frame_len[i], out, out_len);
}

// Sends a whole message in order and checks it comes out intact.
static void send_all(size_t len, uint8_t id, size_t frame_size)
{
    make_message(len, id);
    uint8_t count = split(len, id, frame_size);
    const uint8_t *out = NULL;
    size_t out_len = 0;
    for (uint8_t i = 0; i + 1 < count; i++)
    {
        TEST_ASSERT_EQUAL(NAV_FRAG_PENDING, push(i, &out, &out_len));
    }
    TEST_ASSERT_EQUAL(NAV_FRAG_COMPLETE, push(count - 1, &out, &out_len));
    TEST_ASSERT_EQUAL(len, out_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(msg, out, len);
}

// ---- Splitting -------------------------------------------------------------

static void test_frame_counts(void)
{
    TEST_ASSERT_EQUAL(18, nav_frag_count(NAV_MSG_MAX_SIZE, NAV_FRAG_MIN_FRAME));
    TEST_ASSERT_EQUAL(2, nav_frag_count(NAV_MSG_MAX_SIZE, 182));
    TEST_ASSERT_EQUAL(1, nav_frag_count(NAV_MSG_MAX_SIZE, 514));
    TEST_ASSERT_EQUAL(1, nav_frag_count(11, NAV_FRAG_MIN_FRAME));
    TEST_ASSERT_EQUAL(0, nav_frag_count(0, NAV_FRAG_MIN_FRAME));
    TEST_ASSERT_EQUAL(0, nav_frag_count(NAV_MSG_MAX_SIZE + 1, 514));
    TEST_ASSERT_EQUAL(0, nav_frag_count(10, NAV_FRAG_HEADER_SIZE));
    // 275 bytes at 8 payload bytes per frame would need 35 frames.
    TEST_ASSERT_EQUAL(0, nav_frag_count(NAV_MSG_MAX_SIZE, 12));
}

// The same bytes are checked by tools/ble_sender/tests/test_navproto.py, so
// both ends agree on the layout.
static void test_golden_frames(void)
{
    const uint8_t message[19] = {0x4E, 0xAB, 0x03, 0x01, 0x34, 0x12, 0x02, 0x01, 99, 2,
                                 0xF0, 0xFF, 0x28, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00};
    const uint8_t expected[5][8] = {
        {0xCE, 0xAB, 0, 5, 0x4E, 0xAB, 0x03, 0x01},
        {0xCE, 0xAB, 1, 5, 0x34, 0x12, 0x02, 0x01},
        {0xCE, 0xAB, 2, 5, 99, 2, 0xF0, 0xFF},
        {0xCE, 0xAB, 3, 5, 0x28, 0x00, 0x00, 0x00},
        {0xCE, 0xAB, 4, 5, 0x00, 0x05, 0x00},
    };
    TEST_ASSERT_EQUAL(5, nav_frag_count(sizeof(message), 8));
    const uint8_t *out = NULL;
    size_t out_len = 0;
    for (uint8_t i = 0; i < 5; i++)
    {
        uint8_t frame[8];
        size_t len = nav_frag_build(message, sizeof(message), 0xAB, 8, i, frame, sizeof(frame));
        TEST_ASSERT_EQUAL(i < 4 ? 8 : 7, len);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(expected[i], frame, len);
        TEST_ASSERT_EQUAL(i < 4 ? NAV_FRAG_PENDING : NAV_FRAG_COMPLETE, nav_frag_push(&rx, frame, len, &out, &out_len));
    }
    TEST_ASSERT_EQUAL(sizeof(message), out_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(message, out, sizeof(message));
}

static void test_build_rejects_bad_arguments(void)
{
    make_message(40, 1);
    uint8_t frame[FRAME_CAP];
    TEST_ASSERT_EQUAL(0, nav_frag_build(msg, 40, 1, 20, 3, frame, sizeof(frame))); // only 3 frames
    TEST_ASSERT_EQUAL(0, nav_frag_build(msg, 40, 1, 20, 0, frame, 19));            // out too small
    TEST_ASSERT_EQUAL(0, nav_frag_build(NULL, 40, 1, 20, 0, frame, sizeof(frame)));
    TEST_ASSERT_EQUAL(0, nav_frag_build(msg, 40, 1, 20, 0, NULL, sizeof(frame)));
}

// ---- Normal order ----------------------------------------------------------

static void test_reassembles_in_order_at_every_frame_size(void)
{
    const size_t sizes[] = {NAV_FRAG_MIN_FRAME, 21, 64, 182, 279, 514};
    uint8_t id = 0;
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
    {
        send_all(NAV_MSG_MAX_SIZE, id++, sizes[s]);
        send_all(11, id++, sizes[s]);
        send_all(176, id++, sizes[s]);
    }
    TEST_ASSERT_EQUAL(18, rx.stats.complete);
    TEST_ASSERT_EQUAL(0, rx.stats.dropped);
    TEST_ASSERT_EQUAL(0, rx.stats.bad_frames);
    TEST_ASSERT_EQUAL(0, rx.stats.duplicates);
}

static void test_message_ids_wrap(void)
{
    for (int i = 0; i < 600; i++)
    {
        send_all(100, (uint8_t)i, NAV_FRAG_MIN_FRAME);
    }
    TEST_ASSERT_EQUAL(600, rx.stats.complete);
    TEST_ASSERT_EQUAL(0, rx.stats.dropped);
}

// ---- Lost chunks -----------------------------------------------------------

static void check_lost_chunk(uint8_t lost)
{
    nav_frag_init(&rx);
    make_message(NAV_MSG_MAX_SIZE, 10);
    uint8_t count = split(NAV_MSG_MAX_SIZE, 10, NAV_FRAG_MIN_FRAME);
    for (uint8_t i = 0; i < count; i++)
    {
        if (i != lost)
        {
            TEST_ASSERT_NOT_EQUAL(NAV_FRAG_COMPLETE, push(i, NULL, NULL));
        }
    }
    send_all(NAV_MSG_MAX_SIZE, 11, NAV_FRAG_MIN_FRAME);
    TEST_ASSERT_EQUAL(1, rx.stats.complete);
    TEST_ASSERT_EQUAL(1, rx.stats.dropped);
    TEST_ASSERT_EQUAL(0, rx.stats.bad_frames);
}

static void test_lost_chunk_drops_only_that_message(void)
{
    check_lost_chunk(0);  // first: the rest is ignored
    check_lost_chunk(7);  // middle: dropped at the gap
    check_lost_chunk(17); // last: dropped when the next message starts
}

static void test_frames_after_a_gap_are_ignored(void)
{
    make_message(NAV_MSG_MAX_SIZE, 3);
    uint8_t count = split(NAV_MSG_MAX_SIZE, 3, NAV_FRAG_MIN_FRAME);
    TEST_ASSERT_EQUAL(NAV_FRAG_PENDING, push(0, NULL, NULL));
    TEST_ASSERT_EQUAL(NAV_FRAG_PENDING, push(1, NULL, NULL));
    for (uint8_t i = 3; i < count; i++)
    {
        TEST_ASSERT_EQUAL(NAV_FRAG_IGNORED, push(i, NULL, NULL));
    }
    TEST_ASSERT_EQUAL(1, rx.stats.dropped);
}

static void test_reset_forgets_a_partial_message(void)
{
    make_message(100, 4);
    split(100, 4, NAV_FRAG_MIN_FRAME);
    push(0, NULL, NULL);
    push(1, NULL, NULL);
    nav_frag_reset(&rx);
    TEST_ASSERT_EQUAL(NAV_FRAG_IGNORED, push(2, NULL, NULL)); // start was before the reset
    send_all(100, 5, NAV_FRAG_MIN_FRAME);
    TEST_ASSERT_EQUAL(1, rx.stats.complete);
}

// ---- Duplicates ------------------------------------------------------------

static void test_duplicate_chunks_are_ignored(void)
{
    make_message(100, 8);
    uint8_t count = split(100, 8, NAV_FRAG_MIN_FRAME);
    const uint8_t *out = NULL;
    size_t out_len = 0;
    for (uint8_t i = 0; i + 1 < count; i++)
    {
        TEST_ASSERT_EQUAL(NAV_FRAG_PENDING, push(i, NULL, NULL));
        TEST_ASSERT_EQUAL(NAV_FRAG_IGNORED, push(i, NULL, NULL));
    }
    TEST_ASSERT_EQUAL(NAV_FRAG_COMPLETE, push(count - 1, &out, &out_len));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(msg, out, 100);
    TEST_ASSERT_EQUAL(count - 1, rx.stats.duplicates);
    TEST_ASSERT_EQUAL(0, rx.stats.dropped);
}

static void test_completed_message_is_never_delivered_twice(void)
{
    send_all(100, 9, NAV_FRAG_MIN_FRAME);
    uint8_t count = split(100, 9, NAV_FRAG_MIN_FRAME);
    TEST_ASSERT_EQUAL(NAV_FRAG_IGNORED, push(count - 1, NULL, NULL));
    TEST_ASSERT_EQUAL(NAV_FRAG_IGNORED, push(0, NULL, NULL));

    // Single-frame messages (large MTU) too.
    send_all(100, 10, 514);
    TEST_ASSERT_EQUAL(NAV_FRAG_IGNORED, push(0, NULL, NULL));
    TEST_ASSERT_EQUAL(2, rx.stats.complete);
    TEST_ASSERT_EQUAL(0, rx.stats.dropped);
}

// ---- Interruptions ---------------------------------------------------------

static void test_new_message_interrupts_the_old_one(void)
{
    make_message(NAV_MSG_MAX_SIZE, 20);
    split(NAV_MSG_MAX_SIZE, 20, NAV_FRAG_MIN_FRAME);
    for (uint8_t i = 0; i < 5; i++)
    {
        TEST_ASSERT_EQUAL(NAV_FRAG_PENDING, push(i, NULL, NULL));
    }
    send_all(150, 21, NAV_FRAG_MIN_FRAME);
    TEST_ASSERT_EQUAL(1, rx.stats.complete);
    TEST_ASSERT_EQUAL(1, rx.stats.dropped);
}

static void test_message_whose_start_was_lost_counts_once(void)
{
    make_message(NAV_MSG_MAX_SIZE, 30);
    uint8_t count = split(NAV_MSG_MAX_SIZE, 30, NAV_FRAG_MIN_FRAME);
    for (uint8_t i = 4; i < count; i++)
    {
        TEST_ASSERT_EQUAL(NAV_FRAG_IGNORED, push(i, NULL, NULL));
    }
    TEST_ASSERT_EQUAL(1, rx.stats.dropped);
    send_all(NAV_MSG_MAX_SIZE, 31, NAV_FRAG_MIN_FRAME);
    TEST_ASSERT_EQUAL(1, rx.stats.complete);
}

static void test_dropped_id_is_usable_again_after_wrapping(void)
{
    make_message(100, 40);
    split(100, 40, NAV_FRAG_MIN_FRAME);
    push(1, NULL, NULL); // start lost: id 40 is skipped
    for (int i = 41; i < 41 + 256; i++)
    {
        send_all(100, (uint8_t)i, NAV_FRAG_MIN_FRAME); // includes id 40 again
    }
    TEST_ASSERT_EQUAL(256, rx.stats.complete);
    TEST_ASSERT_EQUAL(1, rx.stats.dropped);
}

// ---- Bad headers -----------------------------------------------------------

static void test_bad_headers_are_rejected(void)
{
    uint8_t f[8] = {NAV_FRAG_TYPE, 1, 0, 2, 0x11, 0x22, 0x33, 0x44};

    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, nav_frag_push(&rx, f, NAV_FRAG_HEADER_SIZE, NULL, NULL)); // no payload
    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, nav_frag_push(&rx, f, 2, NULL, NULL));
    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, nav_frag_push(&rx, NULL, 8, NULL, NULL));

    f[0] = 'G';
    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, nav_frag_push(&rx, f, 8, NULL, NULL));
    f[0] = NAV_FRAG_TYPE;

    f[3] = 0; // count 0
    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, nav_frag_push(&rx, f, 8, NULL, NULL));
    f[3] = NAV_FRAG_MAX_CHUNKS + 1;
    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, nav_frag_push(&rx, f, 8, NULL, NULL));
    f[3] = 255;
    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, nav_frag_push(&rx, f, 8, NULL, NULL));

    f[3] = 2;
    f[2] = 2; // index == count
    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, nav_frag_push(&rx, f, 8, NULL, NULL));
    f[2] = 200;
    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, nav_frag_push(&rx, f, 8, NULL, NULL));

    TEST_ASSERT_EQUAL(9, rx.stats.bad_frames);
    TEST_ASSERT_EQUAL(0, rx.stats.complete);
}

static void test_text_message_is_not_a_frame(void)
{
    const char *text = "GPS:-6.2088,106.8456,25.0";
    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, nav_frag_push(&rx, (const uint8_t *)text, strlen(text), NULL, NULL));
}

static void test_oversized_frame_is_rejected(void)
{
    static uint8_t f[NAV_FRAG_HEADER_SIZE + NAV_FRAG_MAX_MESSAGE + 1];
    memset(f, 0, sizeof(f));
    f[0] = NAV_FRAG_TYPE;
    f[3] = 1;
    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, nav_frag_push(&rx, f, sizeof(f), NULL, NULL));
    TEST_ASSERT_EQUAL(NAV_FRAG_COMPLETE, nav_frag_push(&rx, f, sizeof(f) - 1, NULL, NULL));
}

static void test_message_outgrowing_the_buffer_is_dropped(void)
{
    // 32 chunks of 16 bytes = 512 bytes, more than any message.
    uint8_t f[NAV_FRAG_HEADER_SIZE + 16] = {NAV_FRAG_TYPE, 50, 0, NAV_FRAG_MAX_CHUNKS};
    nav_frag_result_t r = NAV_FRAG_PENDING;
    uint8_t i = 0;
    for (; i < NAV_FRAG_MAX_CHUNKS && r == NAV_FRAG_PENDING; i++)
    {
        f[2] = i;
        r = nav_frag_push(&rx, f, sizeof(f), NULL, NULL);
    }
    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, r);
    TEST_ASSERT_EQUAL(NAV_FRAG_MAX_MESSAGE / 16 + 1, i);
    TEST_ASSERT_EQUAL(1, rx.stats.dropped);
    TEST_ASSERT_EQUAL(0, rx.stats.complete);
}

static void test_count_changing_mid_message_is_rejected(void)
{
    make_message(100, 60);
    split(100, 60, NAV_FRAG_MIN_FRAME);
    push(0, NULL, NULL);
    frames[1][3] = frames[1][3] + 1;
    TEST_ASSERT_EQUAL(NAV_FRAG_BAD, push(1, NULL, NULL));
    TEST_ASSERT_EQUAL(1, rx.stats.dropped);
    TEST_ASSERT_EQUAL(NAV_FRAG_IGNORED, push(2, NULL, NULL));
}

// ---- Through the codec -----------------------------------------------------

static void test_nav_message_survives_minimum_mtu(void)
{
    nav_data_t in;
    memset(&in, 0, sizeof(in));
    in.maneuver = NAV_MANEUVER_TURN_RIGHT;
    in.distance_to_turn_m = 120.0f;
    in.schematic_point_count = NAV_SCHEMATIC_MAX_POINTS;
    for (uint8_t i = 0; i < NAV_SCHEMATIC_MAX_POINTS; i++)
    {
        in.schematic_points[i] = (nav_point_t){0.5f * i, 3.0f * i - 12.0f};
    }
    size_t len = nav_encode(&in, msg, sizeof(msg), 77);
    uint8_t count = split(len, 77, NAV_FRAG_MIN_FRAME);
    const uint8_t *out = NULL;
    size_t out_len = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        push(i, &out, &out_len);
    }
    nav_data_t decoded;
    uint8_t seq = 0;
    TEST_ASSERT_EQUAL(NAV_DECODE_OK, nav_decode(out, out_len, &decoded, &seq));
    TEST_ASSERT_EQUAL_UINT8(77, seq);
    TEST_ASSERT_EQUAL_UINT8(NAV_SCHEMATIC_MAX_POINTS, decoded.schematic_point_count);
}

// ---- Robustness ------------------------------------------------------------

// Random frames and mangled real ones: no crash, no overflow (run it under
// ASan too), and whatever completes fits the buffer.
static void test_random_frames_never_break_the_receiver(void)
{
    srand(99);
    uint8_t f[64];
    for (int iter = 0; iter < 300000; iter++)
    {
        size_t len = (size_t)(rand() % (int)sizeof(f));
        for (size_t i = 0; i < len; i++)
        {
            f[i] = (uint8_t)rand();
        }
        if (len > 0 && rand() % 2)
        {
            f[0] = NAV_FRAG_TYPE;
        }
        if (len > 3 && rand() % 2)
        {
            f[1] = (uint8_t)(rand() % 4); // few ids, so frames collide
            f[3] = (uint8_t)(1 + rand() % 6);
            f[2] = (uint8_t)(rand() % 7);
        }
        uint8_t *copy = malloc(len ? len : 1);
        memcpy(copy, f, len);
        const uint8_t *out = NULL;
        size_t out_len = 0;
        if (nav_frag_push(&rx, copy, len, &out, &out_len) == NAV_FRAG_COMPLETE)
        {
            TEST_ASSERT_TRUE(out_len >= 1 && out_len <= NAV_FRAG_MAX_MESSAGE);
        }
        free(copy);
    }
    TEST_ASSERT_GREATER_THAN(0, rx.stats.complete);
    TEST_ASSERT_GREATER_THAN(0, rx.stats.dropped);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_counts);
    RUN_TEST(test_golden_frames);
    RUN_TEST(test_build_rejects_bad_arguments);
    RUN_TEST(test_reassembles_in_order_at_every_frame_size);
    RUN_TEST(test_message_ids_wrap);
    RUN_TEST(test_lost_chunk_drops_only_that_message);
    RUN_TEST(test_frames_after_a_gap_are_ignored);
    RUN_TEST(test_reset_forgets_a_partial_message);
    RUN_TEST(test_duplicate_chunks_are_ignored);
    RUN_TEST(test_completed_message_is_never_delivered_twice);
    RUN_TEST(test_new_message_interrupts_the_old_one);
    RUN_TEST(test_message_whose_start_was_lost_counts_once);
    RUN_TEST(test_dropped_id_is_usable_again_after_wrapping);
    RUN_TEST(test_bad_headers_are_rejected);
    RUN_TEST(test_text_message_is_not_a_frame);
    RUN_TEST(test_oversized_frame_is_rejected);
    RUN_TEST(test_message_outgrowing_the_buffer_is_dropped);
    RUN_TEST(test_count_changing_mid_message_is_rejected);
    RUN_TEST(test_nav_message_survives_minimum_mtu);
    RUN_TEST(test_random_frames_never_break_the_receiver);
    return UNITY_END();
}
