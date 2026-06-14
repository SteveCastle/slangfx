/*
 * slangfx — per-frame timing metrics (R0).
 *
 * Opt-in via the SLANGFX_METRICS env var (set to anything but empty/"0").
 * The frame loop records the wall-clock cost of each stage (read input,
 * run pipeline, write output); on shutdown a per-stage distribution
 * (mean / p50 / p95 / p99 / max) is printed to stderr.
 *
 * This isolates the GPU dispatch+upload+readback cost (the "render" stage)
 * from host IO, which the external pipe harness cannot separate. It is the
 * in-process baseline the realtime phases optimise against.
 */
#ifndef SLANG_METRICS_H
#define SLANG_METRICS_H

#include <stdbool.h>

struct slang_metrics;

/* True if SLANGFX_METRICS is set to a non-empty, non-"0" value. */
bool slang_metrics_enabled(void);

/* Monotonic high-resolution clock, milliseconds. Always available. */
double slang_now_ms(void);

/* Returns NULL when metrics are disabled — callers treat NULL as a no-op
 * handle, so every other entry point tolerates a NULL m. */
struct slang_metrics *slang_metrics_create(void);

/* Append one frame's per-stage timings (milliseconds). NULL-safe. */
void slang_metrics_record(struct slang_metrics *m,
                          double read_ms, double render_ms, double write_ms);

/* Print the per-stage distribution to stderr. NULL-safe. */
void slang_metrics_report(struct slang_metrics *m);

void slang_metrics_destroy(struct slang_metrics *m);

#endif /* SLANG_METRICS_H */
