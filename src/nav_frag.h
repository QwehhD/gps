// BLE transport framing for navigation messages.
//
// A 'N' message (nav_codec.h) can be up to 275 bytes, more than one BLE
// write carries at small MTUs (20 bytes at the BLE minimum, ~182 on iOS). So
// every message travels as one or more frames, each one write:
//
//   offset  size  field
//   0       1     frame type, NAV_FRAG_TYPE (0xCE)
//   1       1     message id (the sender's counter, wraps 255 -> 0)
//   2       1     chunk index, 0..count-1
//   3       1     chunk count, 1..NAV_FRAG_MAX_CHUNKS
//   4       1+    the next bytes of the message
//
// The message is the chunks' bytes concatenated in index order. 0xCE has
// the high bit set, so it never starts an ASCII text message ("GPS:",
// "ROUTE:"); it is 0x80 | 'N'.
//
// The receiver reassembles in order into a fixed buffer. BLE delivers writes
// in order or not at all, so a gap means a chunk was lost: that message is
// dropped (there is no retransmit) and the next one starts clean. A new
// message arriving before the current one is complete also drops it.
//
// Plain C with no Arduino/LVGL dependency, so the host unit tests build it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nav_codec.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define NAV_FRAG_TYPE 0xCE
#define NAV_FRAG_HEADER_SIZE 4

// Largest message the receiver's buffer holds.
#define NAV_FRAG_MAX_MESSAGE NAV_MSG_MAX_SIZE

// The smallest BLE write (ATT MTU 23 - 3) leaves 16 payload bytes per frame,
// so the largest message needs 18 frames; anything above this limit is a
// corrupt header.
#define NAV_FRAG_MAX_CHUNKS 32

// Write size every BLE link supports.
#define NAV_FRAG_MIN_FRAME 20

    typedef enum
    {
        NAV_FRAG_PENDING,  // frame stored, message not complete yet
        NAV_FRAG_COMPLETE, // frame stored and the message is complete
        NAV_FRAG_IGNORED,  // duplicate, or part of a message already dropped
        NAV_FRAG_BAD,      // header makes no sense; frame discarded
    } nav_frag_result_t;

    typedef struct
    {
        uint32_t complete;   // messages reassembled
        uint32_t dropped;    // messages given up: a chunk went missing
        uint32_t bad_frames; // frames with a nonsensical header
        uint32_t duplicates; // chunks received twice (ignored)
    } nav_frag_stats_t;

    typedef struct
    {
        uint8_t buf[NAV_FRAG_MAX_MESSAGE];
        size_t len;
        bool active; // a message is being assembled
        uint8_t id;
        uint8_t count;
        uint8_t next; // index of the chunk expected next

        bool skipping; // frames of skip_id belong to a dropped message
        uint8_t skip_id;
        bool done_valid; // done_id is the last message completed
        uint8_t done_id;

        nav_frag_stats_t stats;
    } nav_frag_t;

    // Clears everything, statistics included.
    void nav_frag_init(nav_frag_t *f);

    // Forgets any partly assembled message (e.g. on a new connection) but
    // keeps the statistics.
    void nav_frag_reset(nav_frag_t *f);

    // Feeds one received frame. On NAV_FRAG_COMPLETE, *msg and *msg_len give
    // the reassembled message, valid until the next call. The frame is
    // validated as strictly as nav_decode() checks a message: wrong type, no
    // payload, count 0 or above NAV_FRAG_MAX_CHUNKS, index >= count, a count
    // that changes within a message, or a message outgrowing
    // NAV_FRAG_MAX_MESSAGE all give NAV_FRAG_BAD.
    nav_frag_result_t nav_frag_push(nav_frag_t *f, const uint8_t *frame, size_t len,
                                    const uint8_t **msg, size_t *msg_len);

    // Number of frames needed to send msg_len bytes in writes of at most
    // frame_size bytes, or 0 if it cannot be done (no room for payload,
    // empty or oversized message, or more than NAV_FRAG_MAX_CHUNKS frames).
    uint8_t nav_frag_count(size_t msg_len, size_t frame_size);

    // Builds frame `index` of the message into out. Returns its length (at
    // most frame_size), or 0 on bad arguments.
    size_t nav_frag_build(const uint8_t *msg, size_t msg_len, uint8_t msg_id, size_t frame_size,
                          uint8_t index, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
