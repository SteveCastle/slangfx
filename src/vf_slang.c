/*
 * vfslang — standalone raw-frame filter binary.
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
 *     | vfslang --preset crt/newpixie.slangp --width W --height H \
 *     | ffmpeg -f rawvideo -pix_fmt rgba -s WxH -framerate 30 -i - \
 *             -i in.mp4 -map 0:v -map 1:a -c:v libx264 -c:a copy out.mp4
 *
 * Usage (high-level): see wrappers/vfslang.py.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
  #include <fcntl.h>
  #include <io.h>
#endif

#include "slangp.h"
#include "slang_pipeline.h"

struct args {
    const char *preset_path;
    const char *params;
    int width;
    int height;
    int frame_history;
};

static int parse_args(int argc, char **argv, struct args *a)
{
    a->preset_path = NULL;
    a->params = NULL;
    a->width = 0;
    a->height = 0;
    a->frame_history = 8;

    for (int i = 1; i < argc; ++i) {
        const char *opt = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(opt, "--preset") && val)        { a->preset_path = val; ++i; }
        else if (!strcmp(opt, "--params") && val)   { a->params = val; ++i; }
        else if (!strcmp(opt, "--width") && val)    { a->width = atoi(val); ++i; }
        else if (!strcmp(opt, "--height") && val)   { a->height = atoi(val); ++i; }
        else if (!strcmp(opt, "--frame-history") && val) { a->frame_history = atoi(val); ++i; }
        else if (!strcmp(opt, "-h") || !strcmp(opt, "--help")) {
            fprintf(stderr,
                "vfslang - apply a slang shader chain to raw RGBA video frames.\n"
                "Reads RGBA frames from stdin, writes RGBA frames to stdout.\n"
                "\n"
                "Required:\n"
                "  --preset PATH       Path to a libretro .slangp preset.\n"
                "  --width N           Frame width in pixels.\n"
                "  --height N          Frame height in pixels.\n"
                "Optional:\n"
                "  --params 'k=v,...'  Override #pragma parameter defaults.\n"
                "  --frame-history N   Cap OriginalHistory ring depth (default 8).\n"
                "\n"
                "Example:\n"
                "  ffmpeg -i in.mp4 -f rawvideo -pix_fmt rgba - \\\n"
                "    | vfslang --preset crt/newpixie.slangp --width 1920 --height 1080 \\\n"
                "    | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mp4\n");
            return 1;
        }
        else {
            fprintf(stderr, "vfslang: unknown option '%s' (try --help)\n", opt);
            return -1;
        }
    }

    if (!a->preset_path || a->width <= 0 || a->height <= 0) {
        fprintf(stderr, "vfslang: --preset, --width, and --height are required\n");
        return -1;
    }
    return 0;
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
        fprintf(stderr, "vfslang: failed to load preset '%s': %s\n",
                args.preset_path, err ? err : "(unknown)");
        free(err);
        return 3;
    }

    struct slang_pipeline *pipeline = slang_pipeline_create(
        preset, args.width, args.height, args.width, args.height, &err);
    if (!pipeline) {
        fprintf(stderr, "vfslang: pipeline init failed: %s\n",
                err ? err : "(unknown)");
        free(err);
        slangp_free(preset);
        return 4;
    }

    const size_t frame_bytes = (size_t)args.width * (size_t)args.height * 4;
    unsigned char *frame_in  = malloc(frame_bytes);
    unsigned char *frame_out = malloc(frame_bytes);
    if (!frame_in || !frame_out) {
        fprintf(stderr, "vfslang: out of memory\n");
        free(frame_in); free(frame_out);
        slang_pipeline_destroy(pipeline);
        slangp_free(preset);
        return 5;
    }

    unsigned long long frames = 0;
    while (1) {
        size_t got = fread(frame_in, 1, frame_bytes, stdin);
        if (got == 0) break;                             /* clean EOF */
        if (got != frame_bytes) {
            fprintf(stderr, "vfslang: short read at frame %llu (%zu / %zu bytes)\n",
                    frames, got, frame_bytes);
            break;
        }

        rc = slang_pipeline_run(pipeline, frame_in, frame_out);
        if (rc != 0) {
            fprintf(stderr, "vfslang: pipeline run failed at frame %llu (rc=%d)\n",
                    frames, rc);
            break;
        }

        size_t wrote = fwrite(frame_out, 1, frame_bytes, stdout);
        if (wrote != frame_bytes) {
            fprintf(stderr, "vfslang: short write at frame %llu (%zu / %zu bytes)\n",
                    frames, wrote, frame_bytes);
            break;
        }

        ++frames;
    }
    fflush(stdout);

    free(frame_in);
    free(frame_out);
    slang_pipeline_destroy(pipeline);
    slangp_free(preset);

    fprintf(stderr, "vfslang: processed %llu frames\n", frames);
    return 0;
}
