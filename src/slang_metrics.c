#include "slang_metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <time.h>
#endif

struct stage {
    double *samples;
    size_t  count;
    size_t  cap;
    double  sum;
    double  min;
    double  max;
};

struct slang_metrics {
    struct stage read;
    struct stage render;
    struct stage write;
};

bool slang_metrics_enabled(void)
{
    const char *v = getenv("SLANGFX_METRICS");
    return v && v[0] && strcmp(v, "0") != 0;
}

double slang_now_ms(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static int have_freq = 0;
    LARGE_INTEGER now;
    if (!have_freq) { QueryPerformanceFrequency(&freq); have_freq = 1; }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

static void stage_init(struct stage *s)
{
    s->samples = NULL;
    s->count = s->cap = 0;
    s->sum = 0.0;
    s->min = 1.0e30;
    s->max = 0.0;
}

static void stage_push(struct stage *s, double v)
{
    if (s->count == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 1024;
        double *n = realloc(s->samples, ncap * sizeof(double));
        if (!n) return;            /* drop sample under OOM rather than crash */
        s->samples = n;
        s->cap = ncap;
    }
    s->samples[s->count++] = v;
    s->sum += v;
    if (v < s->min) s->min = v;
    if (v > s->max) s->max = v;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pct(const struct stage *s, double q)
{
    if (s->count == 0) return 0.0;
    size_t idx = (size_t)(q * (double)s->count);
    if (idx >= s->count) idx = s->count - 1;
    return s->samples[idx];
}

static void stage_report(const char *name, struct stage *s)
{
    if (s->count == 0) {
        fprintf(stderr, "  %-7s (no samples)\n", name);
        return;
    }
    qsort(s->samples, s->count, sizeof(double), cmp_double);
    fprintf(stderr,
            "  %-7s mean %7.3f  p50 %7.3f  p95 %7.3f  p99 %7.3f  "
            "min %7.3f  max %7.3f  (n=%zu)\n",
            name, s->sum / (double)s->count, pct(s, 0.50), pct(s, 0.95),
            pct(s, 0.99), s->min, s->max, s->count);
}

struct slang_metrics *slang_metrics_create(void)
{
    if (!slang_metrics_enabled()) return NULL;
    struct slang_metrics *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    stage_init(&m->read);
    stage_init(&m->render);
    stage_init(&m->write);
    return m;
}

void slang_metrics_record(struct slang_metrics *m,
                          double read_ms, double render_ms, double write_ms)
{
    if (!m) return;
    stage_push(&m->read, read_ms);
    stage_push(&m->render, render_ms);
    stage_push(&m->write, write_ms);
}

void slang_metrics_report(struct slang_metrics *m)
{
    if (!m) return;
    double total_render = m->render.sum;
    fprintf(stderr, "slangfx metrics (ms per frame):\n");
    stage_report("read",   &m->read);
    stage_report("render", &m->render);
    stage_report("write",  &m->write);
    if (m->render.count > 0 && total_render > 0.0)
        fprintf(stderr, "  render-only sustained: %.1f fps\n",
                1000.0 * (double)m->render.count / total_render);
}

void slang_metrics_destroy(struct slang_metrics *m)
{
    if (!m) return;
    free(m->read.samples);
    free(m->render.samples);
    free(m->write.samples);
    free(m);
}
