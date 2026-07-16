/*
 * slangfx — standalone raw-frame filter binary.
 *
 * Reads RGBA8 frames from stdin, applies a slang shader chain, writes
 * processed RGBA8 frames to stdout. Frame dimensions are passed via
 * command-line args (we don't probe them out of the stream — keeps the
 * IO format trivial and matches ffmpeg's `-f rawvideo` convention).
 *
 * Why not a libavfilter plugin? ffmpeg has no stable plugin ABI; external
 * filters cannot link against the internal ff_* helpers, AVFILTER_DEFINE_CLASS,
 * FILTER_INPUTS, etc. The only routes to a real `vf_slang` libavfilter
 * filter are (a) fork ffmpeg or (b) submit upstream patches. We do (b)
 * eventually (Phase 10); meanwhile this stdin/stdout binary plus the
 * wrappers/ scripts give the same end-user experience without the fork.
 *
 * Usage (raw):
 *   ffmpeg -i in.mp4 -f rawvideo -pix_fmt rgba - \
 *     | slangfx --preset crt/newpixie.slangp --width W --height H \
 *     | ffmpeg -f rawvideo -pix_fmt rgba -s WxH -framerate 30 -i - \
 *             -i in.mp4 -map 0:v -map 1:a -c:v libx264 -c:a copy out.mp4
 *
 * Usage (high-level): see wrappers/slangfx.py.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
  #include <fcntl.h>
  #include <io.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define closesock closesocket
  #define strtok_r strtok_s
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  typedef int sock_t;
  #define SOCK_INVALID (-1)
  #define closesock close
#endif

#include "slangp.h"
#include "slang_pipeline.h"
#include "slang_metrics.h"
#include "frame_io.h"

#define MAX_PRESETS 16

struct args {
    const char *preset_path[MAX_PRESETS];   /* layer stack, applied in order */
    const char *params[MAX_PRESETS];        /* params[i] pairs with preset[i] */
    size_t num_presets;
    int width;
    int height;
    int frame_history;
    int control_port;
};

static int parse_args(int argc, char **argv, struct args *a)
{
    memset(a, 0, sizeof(*a));
    a->frame_history = 8;

    for (int i = 1; i < argc; ++i) {
        const char *opt = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(opt, "--preset") && val) {
            if (a->num_presets >= MAX_PRESETS) {
                fprintf(stderr, "slangfx: too many --preset (max %d)\n", MAX_PRESETS);
                return -1;
            }
            a->preset_path[a->num_presets++] = val; ++i;
        }
        else if (!strcmp(opt, "--params") && val) {
            /* Pairs with the most recent --preset, so the natural CLI
             * "--preset A --params 'x=1' --preset B" binds x=1 to A. */
            if (a->num_presets == 0) {
                fprintf(stderr, "slangfx: --params before any --preset\n");
                return -1;
            }
            a->params[a->num_presets - 1] = val; ++i;
        }
        else if (!strcmp(opt, "--width") && val)    { a->width = atoi(val); ++i; }
        else if (!strcmp(opt, "--height") && val)   { a->height = atoi(val); ++i; }
        else if (!strcmp(opt, "--frame-history") && val) { a->frame_history = atoi(val); ++i; }
        else if (!strcmp(opt, "--control-port") && val) { a->control_port = atoi(val); ++i; }
        else if (!strcmp(opt, "-h") || !strcmp(opt, "--help")) {
            fprintf(stderr,
                "slangfx - apply a slang shader chain to raw RGBA video frames.\n"
                "Reads RGBA frames from stdin, writes RGBA frames to stdout.\n"
                "\n"
                "Required:\n"
                "  --preset PATH       Path to a libretro .slangp preset. Repeatable:\n"
                "                      each preset is a layer, applied in order,\n"
                "                      chained on the GPU (no intermediate readback).\n"
                "  --width N           Frame width in pixels.\n"
                "  --height N          Frame height in pixels.\n"
                "Optional:\n"
                "  --params 'k=v,...'  Override #pragma parameter defaults for the\n"
                "                      most recent --preset.\n"
                "  --frame-history N   Cap OriginalHistory ring depth (default 8).\n"
                "  --control-port N    Listen on udp://127.0.0.1:N for live\n"
                "                      'name=value' param updates (newest wins).\n"
                "                      'i:name=value' targets layer i only;\n"
                "                      un-prefixed names update every layer.\n"
                "\n"
                "Example:\n"
                "  ffmpeg -i in.mp4 -f rawvideo -pix_fmt rgba - \\\n"
                "    | slangfx --preset crt/newpixie.slangp --width 1920 --height 1080 \\\n"
                "    | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mp4\n");
            return 1;
        }
        else {
            fprintf(stderr, "slangfx: unknown option '%s' (try --help)\n", opt);
            return -1;
        }
    }

    if (a->num_presets == 0 || a->width <= 0 || a->height <= 0) {
        fprintf(stderr, "slangfx: --preset, --width, and --height are required\n");
        return -1;
    }
    return 0;
}

/* Apply a 'name=value,name=value' string to the live layer stack. Separators
 * between pairs: comma, semicolon, or newline (so one UDP datagram may carry
 * several updates). A 'i:name=value' pair targets layer i only; un-prefixed
 * names update every layer that declares them. When `warn` is set, an unknown
 * name is reported (used for --params at startup; suppressed for the noisy
 * live control path). */
static void apply_params_string(struct slang_pipeline **pipes, size_t n,
                                const char *str, int warn)
{
    if (!str) return;
    size_t len = strlen(str);
    char *buf = (char *)malloc(len + 1);
    if (!buf) return;
    memcpy(buf, str, len + 1);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",;\r\n", &save);
         tok; tok = strtok_r(NULL, ",;\r\n", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        char *name = tok;
        const char *vals = eq + 1;
        while (*name == ' ' || *name == '\t') ++name;          /* ltrim */
        size_t ln = strlen(name);
        while (ln > 0 && (name[ln-1] == ' ' || name[ln-1] == '\t'))
            name[--ln] = '\0';                                  /* rtrim */
        if (!*name) continue;

        /* Optional 'i:' layer routing prefix (digits then ':'). */
        size_t lo = 0, hi = n;
        char *colon = strchr(name, ':');
        if (colon && colon != name) {
            int digits = 1;
            for (const char *c = name; c < colon; ++c)
                if (*c < '0' || *c > '9') { digits = 0; break; }
            if (digits) {
                size_t idx = (size_t)atoi(name);
                if (idx >= n) continue;            /* stale layer index */
                lo = idx; hi = idx + 1;
                name = colon + 1;
                if (!*name) continue;
            }
        }

        int hits = 0;
        for (size_t i = lo; i < hi; ++i)
            hits += slang_pipeline_set_param(pipes[i], name, (float)atof(vals));
        if (warn && hits == 0)
            fprintf(stderr, "slangfx: --params: unknown parameter '%s'\n", name);
    }
    free(buf);
}

/* Bind a non-blocking UDP socket on 127.0.0.1:port. SOCK_INVALID on failure. */
static sock_t control_socket_open(int port)
{
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == SOCK_INVALID) return SOCK_INVALID;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* localhost only */
    addr.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesock(s);
        return SOCK_INVALID;
    }
#if defined(_WIN32)
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
    return s;
}

/* Drain all pending datagrams and apply them; non-blocking, returns at once
 * when the socket is empty. Newest value per name wins (last write). */
static void control_drain(sock_t s, struct slang_pipeline **pipes, size_t np)
{
    char buf[4096];
    for (;;) {
        int n = (int)recvfrom(s, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n <= 0) break;          /* EWOULDBLOCK / empty -> done this frame */
        buf[n] = '\0';
        apply_params_string(pipes, np, buf, 0);
    }
}

int main(int argc, char **argv)
{
    struct args args;
    int rc = parse_args(argc, argv, &args);
    if (rc != 0) return rc < 0 ? 2 : 0;

#if defined(_WIN32)
    /* On Windows, stdin/stdout default to text mode; raw video frames must
     * not be subjected to CRLF translation. */
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /* Build the layer stack: one pipeline per --preset, all sharing the
     * first pipeline's Vulkan device so slang_chain_run can hand frames
     * between them on the GPU (no intermediate readback/upload). */
    char *err = NULL;
    struct slangp_preset  *presets[MAX_PRESETS] = {0};
    struct slang_pipeline *pipes[MAX_PRESETS]   = {0};
    size_t np = args.num_presets;

    for (size_t i = 0; i < np; ++i) {
        presets[i] = slangp_parse_file(args.preset_path[i], &err);
        if (!presets[i]) {
            fprintf(stderr, "slangfx: failed to load preset '%s': %s\n",
                    args.preset_path[i], err ? err : "(unknown)");
            free(err);
            for (size_t k = i; k-- > 0;) {   /* sharers before the device owner */
                slang_pipeline_destroy(pipes[k]);
                slangp_free(presets[k]);
            }
            return 3;
        }
        pipes[i] = slang_pipeline_create_shared(
            presets[i], args.width, args.height, args.width, args.height,
            i ? pipes[0] : NULL, &err);
        if (!pipes[i]) {
            fprintf(stderr, "slangfx: pipeline init failed (%s): %s\n",
                    args.preset_path[i], err ? err : "(unknown)");
            free(err);
            slangp_free(presets[i]);
            for (size_t k = i; k-- > 0;) {
                slang_pipeline_destroy(pipes[k]);
                slangp_free(presets[k]);
            }
            return 4;
        }
        /* Startup --params overrides for this layer. */
        if (args.params[i])
            apply_params_string(&pipes[i], 1, args.params[i], 1);
    }

    /* Optional live control plane: udp://127.0.0.1:control_port, 'name=value'. */
    sock_t ctl = SOCK_INVALID;
#if defined(_WIN32)
    int wsa_ok = 0;
    if (args.control_port > 0) {
        WSADATA wsad;
        if (WSAStartup(MAKEWORD(2, 2), &wsad) == 0) wsa_ok = 1;
    }
#endif
    if (args.control_port > 0) {
        ctl = control_socket_open(args.control_port);
        if (ctl == SOCK_INVALID)
            fprintf(stderr, "slangfx: warning: could not open control port %d\n",
                    args.control_port);
        else
            fprintf(stderr, "slangfx: live control on udp/%d (name=value)\n",
                    args.control_port);
    }

    const size_t frame_bytes = (size_t)args.width * (size_t)args.height * 4;
    unsigned char *frame_out = malloc(frame_bytes);
    struct frame_source *source = stdio_source_create(frame_bytes, &err);
    struct frame_sink   *sink   = NULL;
    if (frame_out && source) sink = stdio_sink_create(frame_bytes, &err);
    if (!frame_out || !source || !sink) {
        fprintf(stderr, "slangfx: IO init failed: %s\n", err ? err : "out of memory");
        free(err); free(frame_out);
        if (source) source->destroy(source);
        if (sink) sink->destroy(sink);
        for (size_t k = np; k-- > 0;) {
            slang_pipeline_destroy(pipes[k]);
            slangp_free(presets[k]);
        }
        return 5;
    }

    struct slang_metrics *metrics = slang_metrics_create();   /* NULL if disabled */

    unsigned long long frames = 0;
    struct frame in;
    while (1) {
        /* Pull any pending live param updates before this frame is rendered. */
        if (ctl != SOCK_INVALID) control_drain(ctl, pipes, np);

        double t0 = metrics ? slang_now_ms() : 0.0;
        int got = source->next(source, &in);
        if (got == 0) break;                             /* clean EOS */
        if (got < 0) break;                              /* source diagnosed it */

        double t1 = metrics ? slang_now_ms() : 0.0;
        rc = slang_chain_run(pipes, np, in.data, frame_out, in.pts);
        if (rc != 0) {
            fprintf(stderr, "slangfx: pipeline run failed at frame %llu (rc=%d)\n",
                    frames, rc);
            break;
        }

        double t2 = metrics ? slang_now_ms() : 0.0;
        if (sink->put(sink, frame_out, frame_bytes, in.pts) != 0)
            break;                                       /* sink diagnosed it */

        if (metrics)
            slang_metrics_record(metrics, t1 - t0, t2 - t1, slang_now_ms() - t2);

        ++frames;
    }

    slang_metrics_report(metrics);
    slang_metrics_destroy(metrics);

    if (ctl != SOCK_INVALID) closesock(ctl);
#if defined(_WIN32)
    if (wsa_ok) WSACleanup();
#endif

    source->destroy(source);
    sink->destroy(sink);
    free(frame_out);
    for (size_t k = np; k-- > 0;) {          /* sharers before the device owner */
        slang_pipeline_destroy(pipes[k]);
        slangp_free(presets[k]);
    }

    fprintf(stderr, "slangfx: processed %llu frames\n", frames);
    return 0;
}
