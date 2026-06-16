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

struct args {
    const char *preset_path;
    const char *params;
    int width;
    int height;
    int frame_history;
    int control_port;
};

static int parse_args(int argc, char **argv, struct args *a)
{
    a->preset_path = NULL;
    a->params = NULL;
    a->width = 0;
    a->height = 0;
    a->frame_history = 8;
    a->control_port = 0;

    for (int i = 1; i < argc; ++i) {
        const char *opt = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(opt, "--preset") && val)        { a->preset_path = val; ++i; }
        else if (!strcmp(opt, "--params") && val)   { a->params = val; ++i; }
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
                "  --preset PATH       Path to a libretro .slangp preset.\n"
                "  --width N           Frame width in pixels.\n"
                "  --height N          Frame height in pixels.\n"
                "Optional:\n"
                "  --params 'k=v,...'  Override #pragma parameter defaults.\n"
                "  --frame-history N   Cap OriginalHistory ring depth (default 8).\n"
                "  --control-port N    Listen on udp://127.0.0.1:N for live\n"
                "                      'name=value' param updates (newest wins).\n"
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

    if (!a->preset_path || a->width <= 0 || a->height <= 0) {
        fprintf(stderr, "slangfx: --preset, --width, and --height are required\n");
        return -1;
    }
    return 0;
}

/* Apply a 'name=value,name=value' string to the live pipeline. Separators
 * between pairs: comma, semicolon, or newline (so one UDP datagram may carry
 * several updates). When `warn` is set, an unknown name is reported (used for
 * --params at startup; suppressed for the noisy live control path). */
static void apply_params_string(struct slang_pipeline *p, const char *str, int warn)
{
    if (!str) return;
    size_t n = strlen(str);
    char *buf = (char *)malloc(n + 1);
    if (!buf) return;
    memcpy(buf, str, n + 1);

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
        int hits = slang_pipeline_set_param(p, name, (float)atof(vals));
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
static void control_drain(sock_t s, struct slang_pipeline *p)
{
    char buf[4096];
    for (;;) {
        int n = (int)recvfrom(s, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n <= 0) break;          /* EWOULDBLOCK / empty -> done this frame */
        buf[n] = '\0';
        apply_params_string(p, buf, 0);
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

    char *err = NULL;
    struct slangp_preset *preset = slangp_parse_file(args.preset_path, &err);
    if (!preset) {
        fprintf(stderr, "slangfx: failed to load preset '%s': %s\n",
                args.preset_path, err ? err : "(unknown)");
        free(err);
        return 3;
    }

    struct slang_pipeline *pipeline = slang_pipeline_create(
        preset, args.width, args.height, args.width, args.height, &err);
    if (!pipeline) {
        fprintf(stderr, "slangfx: pipeline init failed: %s\n",
                err ? err : "(unknown)");
        free(err);
        slangp_free(preset);
        return 4;
    }

    /* Apply startup --params overrides. (Until now this flag was parsed but
     * never applied — a no-op; this wires it through the live param path.) */
    if (args.params)
        apply_params_string(pipeline, args.params, 1);

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
        slang_pipeline_destroy(pipeline);
        slangp_free(preset);
        return 5;
    }

    struct slang_metrics *metrics = slang_metrics_create();   /* NULL if disabled */

    unsigned long long frames = 0;
    struct frame in;
    while (1) {
        /* Pull any pending live param updates before this frame is rendered. */
        if (ctl != SOCK_INVALID) control_drain(ctl, pipeline);

        double t0 = metrics ? slang_now_ms() : 0.0;
        int got = source->next(source, &in);
        if (got == 0) break;                             /* clean EOS */
        if (got < 0) break;                              /* source diagnosed it */

        double t1 = metrics ? slang_now_ms() : 0.0;
        rc = slang_pipeline_run(pipeline, in.data, frame_out, in.pts);
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
    slang_pipeline_destroy(pipeline);
    slangp_free(preset);

    fprintf(stderr, "slangfx: processed %llu frames\n", frames);
    return 0;
}
