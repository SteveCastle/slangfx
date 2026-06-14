#include "frame_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * stdio source/sink — raw RGBA8 frames on stdin/stdout.
 *
 * Reproduces the original batch-mode IO exactly: fixed-size frames, no PTS,
 * a short read is treated as clean EOF (matching the previous loop), output
 * flushed on teardown. -------------------------------------------------------*/

struct stdio_source {
    uint8_t *buf;
    size_t   bytes;
    uint64_t index;
};

static int stdio_source_next(struct frame_source *s, struct frame *out)
{
    struct stdio_source *st = s->impl;
    size_t got = fread(st->buf, 1, st->bytes, stdin);
    if (got == 0) return 0;                       /* clean EOF */
    if (got != st->bytes) {
        fprintf(stderr, "slangfx: short read at frame %llu (%zu / %zu bytes)\n",
                (unsigned long long)st->index, got, st->bytes);
        return -1;
    }
    out->data  = st->buf;
    out->bytes = st->bytes;
    out->pts   = -1.0;                            /* untimed */
    out->index = st->index++;
    return 1;
}

static void stdio_source_destroy(struct frame_source *s)
{
    if (!s) return;
    struct stdio_source *st = s->impl;
    if (st) { free(st->buf); free(st); }
    free(s);
}

struct frame_source *stdio_source_create(size_t frame_bytes, char **err)
{
    struct frame_source *s = calloc(1, sizeof(*s));
    struct stdio_source *st = calloc(1, sizeof(*st));
    if (!s || !st) goto oom;
    st->buf = malloc(frame_bytes);
    if (!st->buf) goto oom;
    st->bytes = frame_bytes;
    st->index = 0;
    s->impl = st;
    s->next = stdio_source_next;
    s->destroy = stdio_source_destroy;
    return s;
oom:
    if (err) *err = strdup("out of memory (stdio_source)");
    if (st) free(st->buf);
    free(st); free(s);
    return NULL;
}

struct stdio_sink {
    size_t   bytes;
    uint64_t index;
};

static int stdio_sink_put(struct frame_sink *s, const uint8_t *data,
                          size_t bytes, double pts)
{
    (void)pts;
    struct stdio_sink *st = s->impl;
    size_t wrote = fwrite(data, 1, bytes, stdout);
    if (wrote != bytes) {
        fprintf(stderr, "slangfx: short write at frame %llu (%zu / %zu bytes)\n",
                (unsigned long long)st->index, wrote, bytes);
        return -1;
    }
    st->index++;
    return 0;
}

static void stdio_sink_destroy(struct frame_sink *s)
{
    if (!s) return;
    fflush(stdout);
    free(s->impl);
    free(s);
}

struct frame_sink *stdio_sink_create(size_t frame_bytes, char **err)
{
    struct frame_sink *s = calloc(1, sizeof(*s));
    struct stdio_sink *st = calloc(1, sizeof(*st));
    if (!s || !st) {
        if (err) *err = strdup("out of memory (stdio_sink)");
        free(st); free(s);
        return NULL;
    }
    st->bytes = frame_bytes;
    s->impl = st;
    s->put = stdio_sink_put;
    s->destroy = stdio_sink_destroy;
    return s;
}
