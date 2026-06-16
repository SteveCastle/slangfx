/*
 * slangfx — frame source/sink interfaces (R1).
 *
 * The streaming seam. The engine pulls frames from a `frame_source` and pushes
 * processed frames to a `frame_sink`; it is agnostic to where frames come from
 * or go. The current batch behaviour (raw RGBA8 on stdin/stdout) is just the
 * `stdio_*` implementation; realtime sources/sinks (live capture, network,
 * swapchain display, hardware encode) plug into the same vtables without the
 * frame loop changing.
 *
 * v1 frames are host-side RGBA8 of a fixed size. The struct carries a PTS and
 * monotonic index so realtime pacing (R2) and zero-copy Vulkan frames (R6) can
 * extend it without breaking the interface.
 */
#ifndef FRAME_IO_H
#define FRAME_IO_H

#include <stddef.h>
#include <stdint.h>

struct frame {
    uint8_t *data;   /* `bytes` of RGBA8, valid until the next source call */
    size_t   bytes;
    double   pts;    /* seconds from stream start; <0 = unknown/untimed */
    uint64_t index;  /* monotonic frame number from the source */
};

struct frame_source {
    /* Pull the next frame into the source's internal buffer.
     *   1  = got a frame (*out filled)
     *   0  = clean end of stream
     *  <0  = error
     * Blocking; a realtime source applies its own drop/wait policy. */
    int  (*next)(struct frame_source *s, struct frame *out);
    void (*destroy)(struct frame_source *s);
    void *impl;
};

struct frame_sink {
    /* Push one processed frame (`bytes` of RGBA8). 0 = ok, <0 = error. */
    int  (*put)(struct frame_sink *s, const uint8_t *data, size_t bytes, double pts);
    void (*destroy)(struct frame_sink *s);
    void *impl;
};

/* Raw RGBA8 frames on stdin/stdout — the batch-mode protocol. */
struct frame_source *stdio_source_create(size_t frame_bytes, char **err);
struct frame_sink   *stdio_sink_create(size_t frame_bytes, char **err);

#endif /* FRAME_IO_H */
