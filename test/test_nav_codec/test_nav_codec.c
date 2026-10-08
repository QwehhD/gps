// Host tests for the 'N' message codec (src/nav_codec.c):
//   pio test -e native
#include <unity.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nav_codec.h"

// Rounding to 1/16 unit is off by at most half a step.
#define COORD_TOLERANCE (0.5f / NAV_MSG_COORD_SCALE + 1e-5f)

static nav_data_t in;
static nav_data_t out;
static uint8_t packet[NAV_MSG_MAX_SIZE + 16];

void setUp(void)
{
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    memset(packet, 0, sizeof(packet));
}

void tearDown(void)
{
}

static void set_route(uint8_t n, float x0, float y0)
{
    in.schematic_point_count = n;
    for (uint8_t i = 0; i < n; i++)
    {
        in.schematic_points[i].x = x0 + 1.37f * i;
        in.schematic_points[i].y = y0 + 2.91f * i;
    }
}

static void set_side_roads(uint8_t m, uint8_t k)
{
    in.side_road_count = m;
    for (uint8_t i = 0; i < m; i++)
    {
        in.side_roads[i].point_count = k;
        for (uint8_t v = 0; v < k; v++)
        {
            in.side_roads[i].points[v].x = -20.0f + 5.3f * i + 3.1f * v;
            in.side_roads[i].points[v].y = 7.7f * i - 2.2f * v;
        }
    }
}

static size_t encoded_size(uint8_t n, const uint8_t *side_points, uint8_t m)
{
    size_t size = NAV_MSG_HEADER_SIZE + NAV_MSG_POINT_SIZE * n + 1;
    for (uint8_t i = 0; i < m; i++)
    {
        size += 1 + NAV_MSG_POINT_SIZE * side_points[i];
    }
    return size;
}

static void assert_point_near(const nav_point_t *expected, const nav_point_t *actual)
{
    TEST_ASSERT_FLOAT_WITHIN(COORD_TOLERANCE, expected->x, actual->x);
    TEST_ASSERT_FLOAT_WITHIN(COORD_TOLERANCE, expected->y, actual->y);
}

// Encodes `in`, decodes it into `out`, and checks the geometry survived.
static size_t round_trip(uint8_t seq)
{
    size_t len = nav_encode(&in, packet, NAV_MSG_MAX_SIZE, seq);
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_LESS_OR_EQUAL(NAV_MSG_MAX_SIZE, len);

    uint8_t rx_seq = (uint8_t)~seq;
    TEST_ASSERT_EQUAL(NAV_DECODE_OK, nav_decode(packet, len, &out, &rx_seq));
    TEST_ASSERT_EQUAL_UINT8(seq, rx_seq);
    TEST_ASSERT_EQUAL(in.maneuver, out.maneuver);
    TEST_ASSERT_EQUAL_UINT8(in.schematic_point_count, out.schematic_point_count);
    for (uint8_t i = 0; i < in.schematic_point_count; i++)
    {
        assert_point_near(&in.schematic_points[i], &out.schematic_points[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(in.side_road_count, out.side_road_count);
    for (uint8_t i = 0; i < in.side_road_count; i++)
    {
        TEST_ASSERT_EQUAL_UINT8(in.side_roads[i].point_count, out.side_roads[i].point_count);
        for (uint8_t v = 0; v < in.side_roads[i].point_count; v++)
        {
            assert_point_near(&in.side_roads[i].points[v], &out.side_roads[i].points[v]);
        }
    }
    return len;
}

// ---- Round trips -----------------------------------------------------------

static void test_round_trip_typical(void)
{
    in.maneuver = NAV_MANEUVER_TURN_LEFT;
    in.distance_to_turn_m = 234.4f;
    in.total_distance_m = 1234.0f;
    in.speed_kmh = 47.6f;
    set_route(6, -0.4f, -12.0f);
    set_side_roads(3, 4);
    in.side_roads[1].point_count = 2;

    size_t len = round_trip(42);
    const uint8_t side_points[] = {4, 2, 4};
    TEST_ASSERT_EQUAL(encoded_size(6, side_points, 3), len);
    TEST_ASSERT_EQUAL_FLOAT(234.0f, out.distance_to_turn_m);
    TEST_ASSERT_EQUAL_FLOAT(1230.0f, out.total_distance_m);
    TEST_ASSERT_EQUAL_FLOAT(48.0f, out.speed_kmh);
    TEST_ASSERT_EQUAL_HEX8(NAV_MSG_TYPE_NAV, packet[0]);
    TEST_ASSERT_EQUAL_HEX8(NAV_MSG_FLAG_ROUTE_VALID, packet[3]);
}

static void test_round_trip_no_route(void)
{
    in.maneuver = NAV_MANEUVER_STRAIGHT;
    TEST_ASSERT_EQUAL(NAV_MSG_HEADER_SIZE + 1, round_trip(0));
    TEST_ASSERT_EQUAL_HEX8(0, packet[3]);
}

static void test_round_trip_single_point_is_not_a_route(void)
{
    set_route(1, 0.0f, 0.0f);
    round_trip(1);
    TEST_ASSERT_EQUAL_HEX8(0, packet[3]);
}

static void test_round_trip_max_route_points(void)
{
    set_route(NAV_SCHEMATIC_MAX_POINTS, -12.0f, -12.0f);
    TEST_ASSERT_EQUAL(NAV_MSG_HEADER_SIZE + NAV_MSG_POINT_SIZE * NAV_SCHEMATIC_MAX_POINTS + 1, round_trip(7));
}

static void test_round_trip_max_side_roads(void)
{
    set_route(2, 0.0f, -12.0f);
    set_side_roads(NAV_SIDE_ROADS_MAX, NAV_SIDE_ROAD_MAX_POINTS);
    round_trip(8);
}

static void test_round_trip_largest_packet(void)
{
    set_route(NAV_SCHEMATIC_MAX_POINTS, -12.0f, -12.0f);
    set_side_roads(NAV_SIDE_ROADS_MAX, NAV_SIDE_ROAD_MAX_POINTS);
    TEST_ASSERT_EQUAL(275, NAV_MSG_MAX_SIZE);
    TEST_ASSERT_EQUAL(NAV_MSG_MAX_SIZE, round_trip(255));
}

static void test_round_trip_every_maneuver(void)
{
    set_route(3, 0.0f, -1.0f);
    for (int m = 0; m < NAV_MANEUVER_COUNT; m++)
    {
        in.maneuver = (nav_maneuver_t)m;
        round_trip((uint8_t)m);
    }
}

static void test_round_trip_max_values(void)
{
    in.distance_to_turn_m = 65535.0f;
    in.total_distance_m = 655350.0f;
    in.speed_kmh = 255.0f;
    round_trip(3);
    TEST_ASSERT_EQUAL_FLOAT(65535.0f, out.distance_to_turn_m);
    TEST_ASSERT_EQUAL_FLOAT(655350.0f, out.total_distance_m);
    TEST_ASSERT_EQUAL_FLOAT(255.0f, out.speed_kmh);
}

static void test_out_of_range_values_are_clamped(void)
{
    in.distance_to_turn_m = 1e9f;
    in.total_distance_m = 1e9f;
    in.speed_kmh = 300.0f;
    round_trip(4);
    TEST_ASSERT_EQUAL_FLOAT(65535.0f, out.distance_to_turn_m);
    TEST_ASSERT_EQUAL_FLOAT(655350.0f, out.total_distance_m);
    TEST_ASSERT_EQUAL_FLOAT(255.0f, out.speed_kmh);

    in.distance_to_turn_m = -5.0f;
    in.total_distance_m = -5.0f;
    in.speed_kmh = NAN;
    round_trip(5);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.distance_to_turn_m);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.total_distance_m);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.speed_kmh);
}

static void test_round_trip_negative_coordinates(void)
{
    in.schematic_point_count = 4;
    in.schematic_points[0] = (nav_point_t){-12.03f, -11.97f};
    in.schematic_points[1] = (nav_point_t){-0.01f, -0.04f};
    in.schematic_points[2] = (nav_point_t){-29.5f, 33.3f};
    in.schematic_points[3] = (nav_point_t){-2047.9f, -2048.0f};
    round_trip(9);
    TEST_ASSERT_TRUE(out.schematic_points[0].x < 0.0f);
    TEST_ASSERT_TRUE(out.schematic_points[0].y < 0.0f);
}

static void test_coordinates_outside_int16_saturate(void)
{
    in.schematic_point_count = 3;
    in.schematic_points[0] = (nav_point_t){5000.0f, -5000.0f};
    in.schematic_points[1] = (nav_point_t){NAN, 1.0f};
    in.schematic_points[2] = (nav_point_t){INFINITY, -INFINITY};
    size_t len = nav_encode(&in, packet, NAV_MSG_MAX_SIZE, 0);
    TEST_ASSERT_EQUAL(NAV_DECODE_OK, nav_decode(packet, len, &out, NULL));
    TEST_ASSERT_EQUAL_FLOAT(32767.0f / 16.0f, out.schematic_points[0].x);
    TEST_ASSERT_EQUAL_FLOAT(-2048.0f, out.schematic_points[0].y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.schematic_points[1].x);
    TEST_ASSERT_EQUAL_FLOAT(32767.0f / 16.0f, out.schematic_points[2].x);
    TEST_ASSERT_EQUAL_FLOAT(-2048.0f, out.schematic_points[2].y);
}

static void test_quantization_error_is_at_most_half_a_step(void)
{
    in.schematic_point_count = 2;
    float worst = 0.0f;
    for (int i = -200000; i <= 200000; i += 7)
    {
        float v = i / 1000.0f; // -200..200 units in odd steps
        in.schematic_points[0] = (nav_point_t){v, -v};
        size_t len = nav_encode(&in, packet, NAV_MSG_MAX_SIZE, 0);
        TEST_ASSERT_EQUAL(NAV_DECODE_OK, nav_decode(packet, len, &out, NULL));
        worst = fmaxf(worst, fabsf(out.schematic_points[0].x - v));
        worst = fmaxf(worst, fabsf(out.schematic_points[0].y + v));
    }
    TEST_ASSERT_TRUE(worst <= 1.0f / 32.0f + 1e-5f);
}

static void test_encode_layout_is_little_endian(void)
{
    in.maneuver = NAV_MANEUVER_U_TURN;
    in.distance_to_turn_m = 0x1234;
    in.total_distance_m = 0x0102 * 10.0f;
    in.speed_kmh = 99.0f;
    in.schematic_point_count = 2;
    in.schematic_points[0] = (nav_point_t){-1.0f, 2.5f};
    in.schematic_points[1] = (nav_point_t){0.0f, 80.0f};
    TEST_ASSERT_EQUAL(19, nav_encode(&in, packet, NAV_MSG_MAX_SIZE, 0xAB));
    const uint8_t expected[19] = {
        0x4E, 0xAB, NAV_MANEUVER_U_TURN, 0x01,
        0x34, 0x12, 0x02, 0x01, 99, 2,
        0xF0, 0xFF, 0x28, 0x00, // (-16, 40)
        0x00, 0x00, 0x00, 0x05, // (0, 1280)
        0};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, packet, sizeof(expected));
}

// ---- Encoder edge cases ----------------------------------------------------

static void test_encode_rejects_small_buffer_and_bad_input(void)
{
    set_route(5, 0.0f, 0.0f);
    size_t needed = nav_encode(&in, packet, NAV_MSG_MAX_SIZE, 0);
    TEST_ASSERT_EQUAL(0, nav_encode(&in, packet, needed - 1, 0));
    TEST_ASSERT_EQUAL(needed, nav_encode(&in, packet, needed, 0));
    TEST_ASSERT_EQUAL(0, nav_encode(NULL, packet, NAV_MSG_MAX_SIZE, 0));
    TEST_ASSERT_EQUAL(0, nav_encode(&in, NULL, NAV_MSG_MAX_SIZE, 0));
    in.maneuver = NAV_MANEUVER_COUNT;
    TEST_ASSERT_EQUAL(0, nav_encode(&in, packet, NAV_MSG_MAX_SIZE, 0));
}

static void test_encode_skips_undrawable_side_roads(void)
{
    set_route(2, 0.0f, 0.0f);
    set_side_roads(3, 4);
    in.side_roads[0].point_count = 1;
    in.side_roads[2].point_count = 0;
    size_t len = nav_encode(&in, packet, NAV_MSG_MAX_SIZE, 0);
    TEST_ASSERT_EQUAL(NAV_DECODE_OK, nav_decode(packet, len, &out, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, out.side_road_count);
    assert_point_near(&in.side_roads[1].points[3], &out.side_roads[0].points[3]);
}

static void test_encode_clamps_counts(void)
{
    set_route(NAV_SCHEMATIC_MAX_POINTS, 0.0f, 0.0f);
    set_side_roads(NAV_SIDE_ROADS_MAX, NAV_SIDE_ROAD_MAX_POINTS);
    in.schematic_point_count = 200;
    in.side_road_count = 200;
    in.side_roads[0].point_count = 9;
    size_t len = nav_encode(&in, packet, NAV_MSG_MAX_SIZE, 0);
    TEST_ASSERT_EQUAL(NAV_MSG_MAX_SIZE, len);
    TEST_ASSERT_EQUAL(NAV_DECODE_OK, nav_decode(packet, len, &out, NULL));
    TEST_ASSERT_EQUAL_UINT8(NAV_SCHEMATIC_MAX_POINTS, out.schematic_point_count);
    TEST_ASSERT_EQUAL_UINT8(NAV_SIDE_ROADS_MAX, out.side_road_count);
    TEST_ASSERT_EQUAL_UINT8(NAV_SIDE_ROAD_MAX_POINTS, out.side_roads[0].point_count);
}

// ---- Malformed packets -----------------------------------------------------

static size_t full_packet(void)
{
    in.maneuver = NAV_MANEUVER_TURN_RIGHT;
    set_route(NAV_SCHEMATIC_MAX_POINTS, -12.0f, -12.0f);
    set_side_roads(NAV_SIDE_ROADS_MAX, NAV_SIDE_ROAD_MAX_POINTS);
    in.side_roads[3].point_count = 2;
    return nav_encode(&in, packet, NAV_MSG_MAX_SIZE, 17);
}

static size_t side_count_offset(void)
{
    return NAV_MSG_HEADER_SIZE + NAV_MSG_POINT_SIZE * packet[9];
}

static void test_decode_rejects_every_truncation(void)
{
    size_t len = full_packet();
    for (size_t cut = 0; cut < len; cut++)
    {
        // A fresh heap copy of exactly `cut` bytes, so reading past it would
        // trip a sanitizer instead of reading the rest of the array.
        uint8_t *copy = malloc(cut ? cut : 1);
        memcpy(copy, packet, cut);
        TEST_ASSERT_EQUAL(NAV_DECODE_ERR_TRUNCATED, nav_decode(copy, cut, &out, NULL));
        free(copy);
    }
}

static void test_decode_rejects_trailing_bytes(void)
{
    size_t len = full_packet();
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_LENGTH, nav_decode(packet, len + 1, &out, NULL));

    memset(&in, 0, sizeof(in));
    len = nav_encode(&in, packet, NAV_MSG_MAX_SIZE, 0);
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_LENGTH, nav_decode(packet, len + 4, &out, NULL));
}

static void test_decode_rejects_unknown_type(void)
{
    size_t len = full_packet();
    packet[0] = 'G';
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_TYPE, nav_decode(packet, len, &out, NULL));

    const char *text = "GPS:-6.2088,106.8456,25.0";
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_TYPE, nav_decode((const uint8_t *)text, strlen(text), &out, NULL));
}

static void test_decode_rejects_oversized_route_count(void)
{
    size_t len = full_packet();
    packet[9] = NAV_SCHEMATIC_MAX_POINTS + 1;
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_ROUTE_COUNT, nav_decode(packet, len, &out, NULL));
    packet[9] = 255;
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_ROUTE_COUNT, nav_decode(packet, sizeof(packet), &out, NULL));
}

static void test_decode_rejects_oversized_side_road_count(void)
{
    size_t len = full_packet();
    packet[side_count_offset()] = NAV_SIDE_ROADS_MAX + 1;
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_SIDE_COUNT, nav_decode(packet, len, &out, NULL));
}

static void test_decode_rejects_bad_side_road_point_count(void)
{
    const uint8_t bad[] = {0, 1, 5, 255};
    for (size_t i = 0; i < sizeof(bad); i++)
    {
        size_t len = full_packet();
        packet[side_count_offset() + 1] = bad[i];
        TEST_ASSERT_EQUAL(NAV_DECODE_ERR_SIDE_POINTS, nav_decode(packet, len, &out, NULL));
    }
}

static void test_decode_rejects_bad_maneuver(void)
{
    size_t len = full_packet();
    packet[2] = NAV_MANEUVER_COUNT;
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_MANEUVER, nav_decode(packet, len, &out, NULL));
    packet[2] = 255;
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_MANEUVER, nav_decode(packet, len, &out, NULL));
}

static void test_decode_checks_route_flag_against_count(void)
{
    size_t len = full_packet();
    packet[3] = 0;
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_FLAGS, nav_decode(packet, len, &out, NULL));

    memset(&in, 0, sizeof(in));
    len = nav_encode(&in, packet, NAV_MSG_MAX_SIZE, 0);
    packet[3] = NAV_MSG_FLAG_ROUTE_VALID;
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_FLAGS, nav_decode(packet, len, &out, NULL));
}

static void test_decode_ignores_reserved_flag_bits(void)
{
    size_t len = full_packet();
    packet[3] |= 0xFE;
    TEST_ASSERT_EQUAL(NAV_DECODE_OK, nav_decode(packet, len, &out, NULL));
}

static void test_decode_leaves_output_untouched_on_error(void)
{
    size_t len = full_packet();
    packet[9] = 99;
    nav_data_t before;
    memset(&out, 0xA5, sizeof(out));
    memcpy(&before, &out, sizeof(out));
    uint8_t seq = 0x5A;
    TEST_ASSERT_NOT_EQUAL(NAV_DECODE_OK, nav_decode(packet, len, &out, &seq));
    TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof(out));
    TEST_ASSERT_EQUAL_HEX8(0x5A, seq);
}

static void test_decode_sets_fields_not_on_the_wire(void)
{
    size_t len = full_packet();
    memset(&out, 0xA5, sizeof(out));
    TEST_ASSERT_EQUAL(NAV_DECODE_OK, nav_decode(packet, len, &out, NULL));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.bearing_deg);
    TEST_ASSERT_FALSE(out.ble_connected);
}

static void test_decode_rejects_null_arguments(void)
{
    size_t len = full_packet();
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_ARG, nav_decode(NULL, len, &out, NULL));
    TEST_ASSERT_EQUAL(NAV_DECODE_ERR_ARG, nav_decode(packet, len, NULL, NULL));
}

// Random corruptions of valid packets: the decoder must never crash, and any
// packet it accepts must be exactly what the encoder writes for the result
// (reserved flag bits aside), so nothing malformed slips through.
static void test_decode_survives_random_corruption(void)
{
    srand(1234);
    uint8_t again[NAV_MSG_MAX_SIZE];
    unsigned accepted = 0;
    for (int iter = 0; iter < 200000; iter++)
    {
        size_t len = full_packet();
        int edits = 1 + rand() % 3;
        for (int e = 0; e < edits; e++)
        {
            packet[rand() % (len + 1)] = (uint8_t)rand();
        }
        size_t try_len = (size_t)((int)len + rand() % 9 - 4);
        uint8_t *copy = malloc(try_len ? try_len : 1);
        memcpy(copy, packet, try_len);
        uint8_t seq = 0;
        if (nav_decode(copy, try_len, &out, &seq) == NAV_DECODE_OK)
        {
            accepted++;
            TEST_ASSERT_EQUAL(try_len, nav_encode(&out, again, sizeof(again), seq));
            copy[3] &= NAV_MSG_FLAG_ROUTE_VALID;
            TEST_ASSERT_EQUAL_HEX8_ARRAY(copy, again, try_len);
        }
        free(copy);
    }
    TEST_ASSERT_GREATER_THAN(0, accepted);
}

static void test_status_strings(void)
{
    for (int s = NAV_DECODE_OK; s <= NAV_DECODE_ERR_SIDE_POINTS; s++)
    {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("unknown", nav_decode_status_str((nav_decode_status_t)s)));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_typical);
    RUN_TEST(test_round_trip_no_route);
    RUN_TEST(test_round_trip_single_point_is_not_a_route);
    RUN_TEST(test_round_trip_max_route_points);
    RUN_TEST(test_round_trip_max_side_roads);
    RUN_TEST(test_round_trip_largest_packet);
    RUN_TEST(test_round_trip_every_maneuver);
    RUN_TEST(test_round_trip_max_values);
    RUN_TEST(test_out_of_range_values_are_clamped);
    RUN_TEST(test_round_trip_negative_coordinates);
    RUN_TEST(test_coordinates_outside_int16_saturate);
    RUN_TEST(test_quantization_error_is_at_most_half_a_step);
    RUN_TEST(test_encode_layout_is_little_endian);
    RUN_TEST(test_encode_rejects_small_buffer_and_bad_input);
    RUN_TEST(test_encode_skips_undrawable_side_roads);
    RUN_TEST(test_encode_clamps_counts);
    RUN_TEST(test_decode_rejects_every_truncation);
    RUN_TEST(test_decode_rejects_trailing_bytes);
    RUN_TEST(test_decode_rejects_unknown_type);
    RUN_TEST(test_decode_rejects_oversized_route_count);
    RUN_TEST(test_decode_rejects_oversized_side_road_count);
    RUN_TEST(test_decode_rejects_bad_side_road_point_count);
    RUN_TEST(test_decode_rejects_bad_maneuver);
    RUN_TEST(test_decode_checks_route_flag_against_count);
    RUN_TEST(test_decode_ignores_reserved_flag_bits);
    RUN_TEST(test_decode_leaves_output_untouched_on_error);
    RUN_TEST(test_decode_sets_fields_not_on_the_wire);
    RUN_TEST(test_decode_rejects_null_arguments);
    RUN_TEST(test_decode_survives_random_corruption);
    RUN_TEST(test_status_strings);
    return UNITY_END();
}
