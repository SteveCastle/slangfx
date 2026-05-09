/*
 * vf-slang — libavfilter integration.
 *
 * Registers a `slang` video filter that loads a libretro-format .slangp
 * preset and applies the resulting multi-pass shader chain to each frame
 * via Vulkan + shaderc.
 *
 * Phase 0: filter registers and is callable from `ffmpeg -vf slang=...`,
 * but the pipeline is an identity copy (the work happens in later phases).
 *
 * Usage:
 *   ffmpeg -i in.mp4 -vf "slang=preset=path/to/preset.slangp" out.mp4
 *
 * Optional params:
 *   preset=PATH           Required. Path to .slangp file.
 *   params=K1=V1,K2=V2    Override #pragma parameter defaults.
 *   frame_history=N       Cap the OriginalHistory ring depth (default 8).
 */

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avstring.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#include "slangp.h"
#include "slang_pipeline.h"

typedef struct VfSlangContext {
    const AVClass *class;

    /* Options */
    char    *preset_path;
    char    *params_str;
    int      frame_history;

    /* Runtime */
    struct slangp_preset  *preset;
    struct slang_pipeline *pipeline;
    int                    cfg_input_w;
    int                    cfg_input_h;
} VfSlangContext;

#define OFFSET(x) offsetof(VfSlangContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption slang_options[] = {
    { "preset",        "Path to .slangp preset file",       OFFSET(preset_path),
      AV_OPT_TYPE_STRING, { .str = NULL },                  0, 0, FLAGS },
    { "params",        "Comma-separated parameter overrides (k=v,k=v)",
      OFFSET(params_str), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "frame_history", "Maximum OriginalHistory ring depth", OFFSET(frame_history),
      AV_OPT_TYPE_INT,    { .i64 = 8 },                     0, 64, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(vf_slang);

static av_cold int slang_init(AVFilterContext *ctx)
{
    VfSlangContext *s = ctx->priv;

    if (!s->preset_path || !*s->preset_path) {
        av_log(ctx, AV_LOG_ERROR,
               "preset= option is required (path to a .slangp file)\n");
        return AVERROR(EINVAL);
    }

    char *err = NULL;
    s->preset = slangp_parse_file(s->preset_path, &err);
    if (!s->preset) {
        av_log(ctx, AV_LOG_ERROR, "failed to load preset '%s': %s\n",
               s->preset_path, err ? err : "unknown error");
        av_freep(&err);
        return AVERROR_INVALIDDATA;
    }

    /* Pipeline isn't fully created until config_input — we need the input
     * dimensions first. Phase 1 delays this; Phase 0 leaves it null. */
    return 0;
}

static av_cold void slang_uninit(AVFilterContext *ctx)
{
    VfSlangContext *s = ctx->priv;
    slang_pipeline_destroy(s->pipeline);
    s->pipeline = NULL;
    slangp_free(s->preset);
    s->preset = NULL;
}

static int slang_query_formats(AVFilterContext *ctx)
{
    /* Phase 1: RGBA only (host-managed upload/readback).
     * Phase 9: also accept AV_PIX_FMT_VULKAN, AV_PIX_FMT_NV12, etc. */
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_BGRA,
        AV_PIX_FMT_NONE,
    };
    AVFilterFormats *fmts = ff_make_format_list(pix_fmts);
    if (!fmts)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts);
}

static int slang_config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    VfSlangContext  *s   = ctx->priv;

    s->cfg_input_w = inlink->w;
    s->cfg_input_h = inlink->h;

    char *err = NULL;
    s->pipeline = slang_pipeline_create(s->preset,
                                        inlink->w, inlink->h,
                                        inlink->w, inlink->h,
                                        &err);
    if (!s->pipeline) {
        av_log(ctx, AV_LOG_ERROR, "pipeline init failed: %s\n",
               err ? err : "unknown error");
        av_freep(&err);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int slang_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx     = inlink->dst;
    VfSlangContext  *s       = ctx->priv;
    AVFilterLink    *outlink = ctx->outputs[0];

    AVFrame *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    /* Phase 0: identity. Phase 1+: actual pipeline run.
     * We handle the row stride mismatch later; for now assume tightly
     * packed input matching the pipeline's expectation. */
    if (s->pipeline)
        slang_pipeline_run(s->pipeline, in->data[0], out->data[0]);

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static const AVFilterPad slang_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = slang_config_input,
        .filter_frame = slang_filter_frame,
    },
};

static const AVFilterPad slang_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

const AVFilter ff_vf_slang = {
    .name          = "slang",
    .description   = NULL_IF_CONFIG_SMALL(
        "Apply libretro-format slang shaders (.slangp presets) to video."),
    .priv_size     = sizeof(VfSlangContext),
    .priv_class    = &vf_slang_class,
    .init          = slang_init,
    .uninit        = slang_uninit,
    FILTER_INPUTS(slang_inputs),
    FILTER_OUTPUTS(slang_outputs),
    FILTER_QUERY_FUNC(slang_query_formats),
    .flags         = AVFILTER_FLAG_SLICE_THREADS,
};
