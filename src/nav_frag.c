// Implementation of the BLE framing declared in nav_frag.h.
#include "nav_frag.h"

#include <string.h>

void nav_frag_init(nav_frag_t *f)
{
    memset(f, 0, sizeof(*f));
}

void nav_frag_reset(nav_frag_t *f)
{
    nav_frag_stats_t stats = f->stats;
    nav_frag_init(f);
    f->stats = stats;
}

// Gives up on the message being assembled; its remaining frames are ignored.
static void drop_current(nav_frag_t *f)
{
    f->stats.dropped++;
    f->active = false;
    f->skipping = true;
    f->skip_id = f->id;
}

static nav_frag_result_t append(nav_frag_t *f, const uint8_t *payload, size_t len,
                                const uint8_t **msg, size_t *msg_len)
{
    if (len > sizeof(f->buf) - f->len)
    {
        // Growing past any valid message: the sender is broken, not lossy.
        f->stats.bad_frames++;
        drop_current(f);
        return NAV_FRAG_BAD;
    }
    memcpy(f->buf + f->len, payload, len);
    f->len += len;
    f->next++;
    if (f->next < f->count)
    {
        return NAV_FRAG_PENDING;
    }

    f->active = false;
    f->done_valid = true;
    f->done_id = f->id;
    f->stats.complete++;
    if (msg != NULL)
    {
        *msg = f->buf;
    }
    if (msg_len != NULL)
    {
        *msg_len = f->len;
    }
    return NAV_FRAG_COMPLETE;
}

nav_frag_result_t nav_frag_push(nav_frag_t *f, const uint8_t *frame, size_t len,
                                const uint8_t **msg, size_t *msg_len)
{
    if (frame == NULL || len <= NAV_FRAG_HEADER_SIZE || frame[0] != NAV_FRAG_TYPE)
    {
        f->stats.bad_frames++;
        return NAV_FRAG_BAD;
    }
    uint8_t id = frame[1];
    uint8_t index = frame[2];
    uint8_t count = frame[3];
    const uint8_t *payload = frame + NAV_FRAG_HEADER_SIZE;
    size_t payload_len = len - NAV_FRAG_HEADER_SIZE;
    if (count == 0 || count > NAV_FRAG_MAX_CHUNKS || index >= count || payload_len > NAV_FRAG_MAX_MESSAGE)
    {
        f->stats.bad_frames++;
        return NAV_FRAG_BAD;
    }

    if (f->skipping)
    {
        if (id == f->skip_id)
        {
            return NAV_FRAG_IGNORED;
        }
        // Frames arrive in order, so the dropped message's frames are over
        // (and its id may come round again after wrapping).
        f->skipping = false;
    }

    if (f->active && id == f->id)
    {
        if (count != f->count)
        {
            f->stats.bad_frames++;
            drop_current(f);
            return NAV_FRAG_BAD;
        }
        if (index < f->next)
        {
            f->stats.duplicates++;
            return NAV_FRAG_IGNORED;
        }
        if (index > f->next)
        {
            drop_current(f); // the chunks in between were lost
            return NAV_FRAG_IGNORED;
        }
        return append(f, payload, payload_len, msg, msg_len);
    }

    if (f->done_valid && id == f->done_id)
    {
        // A repeat of a message already delivered: never deliver it twice.
        f->stats.duplicates++;
        return NAV_FRAG_IGNORED;
    }

    // A frame of a different message: the one being assembled can no longer
    // complete.
    if (f->active)
    {
        drop_current(f);
    }
    if (index != 0)
    {
        // Its first chunk was lost, so this message is gone too.
        f->stats.dropped++;
        f->skipping = true;
        f->skip_id = id;
        return NAV_FRAG_IGNORED;
    }

    f->active = true;
    f->id = id;
    f->count = count;
    f->next = 0;
    f->len = 0;
    return append(f, payload, payload_len, msg, msg_len);
}

uint8_t nav_frag_count(size_t msg_len, size_t frame_size)
{
    if (frame_size <= NAV_FRAG_HEADER_SIZE || msg_len == 0 || msg_len > NAV_FRAG_MAX_MESSAGE)
    {
        return 0;
    }
    size_t per_frame = frame_size - NAV_FRAG_HEADER_SIZE;
    size_t count = (msg_len + per_frame - 1) / per_frame;
    return count <= NAV_FRAG_MAX_CHUNKS ? (uint8_t)count : 0;
}

size_t nav_frag_build(const uint8_t *msg, size_t msg_len, uint8_t msg_id, size_t frame_size,
                      uint8_t index, uint8_t *out, size_t cap)
{
    uint8_t count = nav_frag_count(msg_len, frame_size);
    if (msg == NULL || out == NULL || count == 0 || index >= count)
    {
        return 0;
    }
    size_t per_frame = frame_size - NAV_FRAG_HEADER_SIZE;
    size_t offset = (size_t)index * per_frame;
    size_t payload_len = msg_len - offset < per_frame ? msg_len - offset : per_frame;
    if (cap < NAV_FRAG_HEADER_SIZE + payload_len)
    {
        return 0;
    }
    out[0] = NAV_FRAG_TYPE;
    out[1] = msg_id;
    out[2] = index;
    out[3] = count;
    memcpy(out + NAV_FRAG_HEADER_SIZE, msg + offset, payload_len);
    return NAV_FRAG_HEADER_SIZE + payload_len;
}
