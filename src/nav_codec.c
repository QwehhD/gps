// Implementation of the 'N' message codec declared in nav_codec.h.
#include "nav_codec.h"

#include <math.h>
#include <string.h>

// Rounds v * scale to the nearest integer (halves away from zero), clamped
// to lo..hi. NaN becomes 0.
static int32_t quantize(float v, float scale, int32_t lo, int32_t hi)
{
    if (isnan(v))
    {
        v = 0.0f;
    }
    float scaled = v * scale;
    if (scaled <= (float)lo)
    {
        return lo;
    }
    if (scaled >= (float)hi)
    {
        return hi;
    }
    return (int32_t)roundf(scaled);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint8_t *put_point(uint8_t *p, const nav_point_t *pt)
{
    put_u16(p, (uint16_t)quantize(pt->x, NAV_MSG_COORD_SCALE, INT16_MIN, INT16_MAX));
    put_u16(p + 2, (uint16_t)quantize(pt->y, NAV_MSG_COORD_SCALE, INT16_MIN, INT16_MAX));
    return p + NAV_MSG_POINT_SIZE;
}

static float get_coord(const uint8_t *p)
{
    int32_t v = get_u16(p);
    if (v >= 0x8000)
    {
        v -= 0x10000;
    }
    return (float)v / NAV_MSG_COORD_SCALE;
}

static const uint8_t *get_point(const uint8_t *p, nav_point_t *pt)
{
    pt->x = get_coord(p);
    pt->y = get_coord(p + 2);
    return p + NAV_MSG_POINT_SIZE;
}

static uint8_t clamp_count(uint8_t count, uint8_t max)
{
    return count > max ? max : count;
}

size_t nav_encode(const nav_data_t *nav, uint8_t *buf, size_t cap, uint8_t seq)
{
    if (nav == NULL || buf == NULL || (unsigned)nav->maneuver >= NAV_MANEUVER_COUNT)
    {
        return 0;
    }

    uint8_t n = clamp_count(nav->schematic_point_count, NAV_SCHEMATIC_MAX_POINTS);
    uint8_t side_count = clamp_count(nav->side_road_count, NAV_SIDE_ROADS_MAX);
    size_t size = NAV_MSG_HEADER_SIZE + NAV_MSG_POINT_SIZE * (size_t)n + 1;
    for (uint8_t i = 0; i < side_count; i++)
    {
        uint8_t k = clamp_count(nav->side_roads[i].point_count, NAV_SIDE_ROAD_MAX_POINTS);
        if (k >= 2)
        {
            size += 1 + NAV_MSG_POINT_SIZE * (size_t)k;
        }
    }
    if (size > cap)
    {
        return 0;
    }

    buf[0] = NAV_MSG_TYPE_NAV;
    buf[1] = seq;
    buf[2] = (uint8_t)nav->maneuver;
    buf[3] = n >= 2 ? NAV_MSG_FLAG_ROUTE_VALID : 0;
    put_u16(buf + 4, (uint16_t)quantize(nav->distance_to_turn_m, 1.0f, 0, UINT16_MAX));
    put_u16(buf + 6, (uint16_t)quantize(nav->total_distance_m, 0.1f, 0, UINT16_MAX));
    buf[8] = (uint8_t)quantize(nav->speed_kmh, 1.0f, 0, UINT8_MAX);
    buf[9] = n;

    uint8_t *p = buf + NAV_MSG_HEADER_SIZE;
    for (uint8_t i = 0; i < n; i++)
    {
        p = put_point(p, &nav->schematic_points[i]);
    }

    uint8_t *m = p++;
    *m = 0;
    for (uint8_t i = 0; i < side_count; i++)
    {
        const nav_side_road_t *road = &nav->side_roads[i];
        uint8_t k = clamp_count(road->point_count, NAV_SIDE_ROAD_MAX_POINTS);
        if (k < 2)
        {
            continue; // not drawable, and the decoder would reject it
        }
        *p++ = k;
        for (uint8_t v = 0; v < k; v++)
        {
            p = put_point(p, &road->points[v]);
        }
        (*m)++;
    }
    return size;
}

// Walks the packet's structure without writing anything, so a malformed
// packet is rejected before *out is touched.
static nav_decode_status_t validate(const uint8_t *buf, size_t len)
{
    if (len < 1)
    {
        return NAV_DECODE_ERR_TRUNCATED;
    }
    if (buf[0] != NAV_MSG_TYPE_NAV)
    {
        return NAV_DECODE_ERR_TYPE;
    }
    if (len < NAV_MSG_HEADER_SIZE)
    {
        return NAV_DECODE_ERR_TRUNCATED;
    }
    if (buf[2] >= NAV_MANEUVER_COUNT)
    {
        return NAV_DECODE_ERR_MANEUVER;
    }
    uint8_t n = buf[9];
    if (n > NAV_SCHEMATIC_MAX_POINTS)
    {
        return NAV_DECODE_ERR_ROUTE_COUNT;
    }
    if (((buf[3] & NAV_MSG_FLAG_ROUTE_VALID) != 0) != (n >= 2))
    {
        return NAV_DECODE_ERR_FLAGS;
    }

    // Every count is bounded, so pos stays below NAV_MSG_MAX_SIZE + 1.
    size_t pos = NAV_MSG_HEADER_SIZE + NAV_MSG_POINT_SIZE * (size_t)n;
    if (len < pos + 1)
    {
        return NAV_DECODE_ERR_TRUNCATED;
    }
    uint8_t m = buf[pos++];
    if (m > NAV_SIDE_ROADS_MAX)
    {
        return NAV_DECODE_ERR_SIDE_COUNT;
    }
    for (uint8_t i = 0; i < m; i++)
    {
        if (len < pos + 1)
        {
            return NAV_DECODE_ERR_TRUNCATED;
        }
        uint8_t k = buf[pos++];
        if (k < 2 || k > NAV_SIDE_ROAD_MAX_POINTS)
        {
            return NAV_DECODE_ERR_SIDE_POINTS;
        }
        pos += NAV_MSG_POINT_SIZE * (size_t)k;
        if (len < pos)
        {
            return NAV_DECODE_ERR_TRUNCATED;
        }
    }
    return len == pos ? NAV_DECODE_OK : NAV_DECODE_ERR_LENGTH;
}

nav_decode_status_t nav_decode(const uint8_t *buf, size_t len, nav_data_t *out, uint8_t *seq)
{
    if (buf == NULL || out == NULL)
    {
        return NAV_DECODE_ERR_ARG;
    }
    nav_decode_status_t status = validate(buf, len);
    if (status != NAV_DECODE_OK)
    {
        return status;
    }

    memset(out, 0, sizeof(*out));
    if (seq != NULL)
    {
        *seq = buf[1];
    }
    out->maneuver = (nav_maneuver_t)buf[2];
    out->distance_to_turn_m = (float)get_u16(buf + 4);
    out->total_distance_m = (float)get_u16(buf + 6) * 10.0f;
    out->speed_kmh = (float)buf[8];
    out->schematic_point_count = buf[9];

    const uint8_t *p = buf + NAV_MSG_HEADER_SIZE;
    for (uint8_t i = 0; i < out->schematic_point_count; i++)
    {
        p = get_point(p, &out->schematic_points[i]);
    }
    out->side_road_count = *p++;
    for (uint8_t i = 0; i < out->side_road_count; i++)
    {
        nav_side_road_t *road = &out->side_roads[i];
        road->point_count = *p++;
        for (uint8_t v = 0; v < road->point_count; v++)
        {
            p = get_point(p, &road->points[v]);
        }
    }
    return NAV_DECODE_OK;
}

const char *nav_decode_status_str(nav_decode_status_t status)
{
    switch (status)
    {
    case NAV_DECODE_OK:
        return "ok";
    case NAV_DECODE_ERR_ARG:
        return "null argument";
    case NAV_DECODE_ERR_TRUNCATED:
        return "truncated";
    case NAV_DECODE_ERR_LENGTH:
        return "trailing bytes";
    case NAV_DECODE_ERR_TYPE:
        return "unknown message type";
    case NAV_DECODE_ERR_MANEUVER:
        return "bad maneuver";
    case NAV_DECODE_ERR_FLAGS:
        return "route flag disagrees with point count";
    case NAV_DECODE_ERR_ROUTE_COUNT:
        return "too many route points";
    case NAV_DECODE_ERR_SIDE_COUNT:
        return "too many side roads";
    case NAV_DECODE_ERR_SIDE_POINTS:
        return "bad side road point count";
    }
    return "unknown";
}
