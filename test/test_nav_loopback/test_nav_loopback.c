// Host test for NAV_SOURCE_LOOPBACK: drives the real simulator through the
// codec the way main.cpp does and checks that what reaches the UI matches
// the simulator output within the codec's rounding.
//   pio test -e native
#include <unity.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nav_codec.h"
#include "nav_sim.h"

#define FRAME_MS 25
#define SIM_FRAMES 40000 // 1000 s of riding, roughly 15 km at the sim's speeds

void setUp(void)
{
}

void tearDown(void)
{
}

static float worst_point_error(const nav_point_t *a, const nav_point_t *b)
{
    return fmaxf(fabsf(a->x - b->x), fabsf(a->y - b->y));
}

static void test_simulator_survives_the_codec(void)
{
    static uint8_t packet[NAV_MSG_MAX_SIZE];
    static nav_data_t decoded;
    nav_sim_init();

    float worst_coord = 0.0f;
    float worst_distance = 0.0f;
    float worst_total = 0.0f;
    float worst_speed = 0.0f;
    size_t max_len = 0;
    double sum_len = 0.0;

    for (uint32_t frame = 1; frame <= SIM_FRAMES; frame++)
    {
        nav_sim_update(frame * FRAME_MS);
        const nav_data_t *sim = nav_sim_get_data();
        uint8_t seq = (uint8_t)frame;
        size_t len = nav_encode(sim, packet, sizeof(packet), seq);
        TEST_ASSERT_GREATER_THAN(0, len);

        uint8_t rx_seq = 0;
        TEST_ASSERT_EQUAL(NAV_DECODE_OK, nav_decode(packet, len, &decoded, &rx_seq));
        TEST_ASSERT_EQUAL_UINT8(seq, rx_seq);
        TEST_ASSERT_EQUAL(sim->maneuver, decoded.maneuver);
        TEST_ASSERT_EQUAL_UINT8(sim->schematic_point_count, decoded.schematic_point_count);
        TEST_ASSERT_EQUAL_UINT8(sim->side_road_count, decoded.side_road_count);

        for (uint8_t i = 0; i < sim->schematic_point_count; i++)
        {
            worst_coord = fmaxf(worst_coord, worst_point_error(&sim->schematic_points[i], &decoded.schematic_points[i]));
        }
        for (uint8_t i = 0; i < sim->side_road_count; i++)
        {
            TEST_ASSERT_EQUAL_UINT8(sim->side_roads[i].point_count, decoded.side_roads[i].point_count);
            for (uint8_t v = 0; v < sim->side_roads[i].point_count; v++)
            {
                worst_coord = fmaxf(worst_coord, worst_point_error(&sim->side_roads[i].points[v], &decoded.side_roads[i].points[v]));
            }
        }
        worst_distance = fmaxf(worst_distance, fabsf(sim->distance_to_turn_m - decoded.distance_to_turn_m));
        worst_total = fmaxf(worst_total, fabsf(sim->total_distance_m - decoded.total_distance_m));
        worst_speed = fmaxf(worst_speed, fabsf(sim->speed_kmh - decoded.speed_kmh));

        sum_len += len;
        if (len > max_len)
        {
            max_len = len;
        }
    }

    char msg[160];
    snprintf(msg, sizeof(msg),
             "%d packets: avg %.1f B, max %u B; worst error: coord %.4f unit, distance %.2f m, total %.2f m, speed %.2f km/h",
             SIM_FRAMES, sum_len / SIM_FRAMES, (unsigned)max_len, worst_coord, worst_distance, worst_total, worst_speed);
    TEST_MESSAGE(msg);

    TEST_ASSERT_TRUE(worst_coord <= 1.0f / 32.0f + 1e-4f);
    TEST_ASSERT_TRUE(worst_distance <= 0.5f + 1e-3f);
    TEST_ASSERT_TRUE(worst_total <= 5.0f + 1e-3f);
    TEST_ASSERT_TRUE(worst_speed <= 0.5f + 1e-3f);
    TEST_ASSERT_LESS_OR_EQUAL(NAV_MSG_MAX_SIZE, max_len);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_simulator_survives_the_codec);
    return UNITY_END();
}
