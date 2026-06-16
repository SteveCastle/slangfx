/*
 * slangfx — Vulkan render pipeline.
 *
 * Owns the Vulkan device + per-pass framebuffers and graphics pipelines.
 * Manages the per-frame chain:
 *   - Each pass writes to its dedicated FBO (sized per slangp scale rules).
 *   - Aliases: pass output bound as Source for next pass + as <alias> for
 *     any later pass that names it.
 *   - Feedback rings: 2-deep ring per pass that has a downstream
 *     PassFeedback consumer; flipped each frame.
 *   - Original history ring: depth determined by max OriginalHistory<N>
 *     reference across all passes.
 *
 * The pipeline itself is image-format-agnostic. Conversion between input
 * AVFrame pixel format and the internal RGB working format is handled by
 * the upload/readback paths in vf_slang.c (Phase 9 will accept Vulkan
 * AVFrames directly).
 */

#ifndef VF_SLANG_PIPELINE_H
#define VF_SLANG_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "slangp.h"
#include "slang_compile.h"

/* Forward declaration; concrete struct is private to slang_pipeline.c. */
struct slang_pipeline;

/* Build a pipeline from a parsed preset. Compiles all referenced shaders,
 * sets up the Vulkan device, allocates framebuffers based on input
 * dimensions, and (Phase 6) sizes the feedback / history rings.
 *
 * input_w/h are the source video dimensions; output_w/h are the final
 * downstream dimensions (used for `viewport`-relative scale rules).
 *
 * Returns NULL on failure; *err_out gets a malloc'd diagnostic. */
struct slang_pipeline *slang_pipeline_create(const struct slangp_preset *preset,
                                             unsigned input_w, unsigned input_h,
                                             unsigned output_w, unsigned output_h,
                                             char **err_out);

/* Apply the pipeline to one frame. Input and output buffers are CPU-side
 * BGRA8 / RGBA8 (Phase 1); Phase 9 adds zero-copy Vulkan AVFrame paths.
 *
 * `src` length must be input_w * input_h * 4 bytes.
 * `dst` length must be output_w * output_h * 4 bytes.
 *
 * `time_sec` populates the `Time` standard field (seconds). Pass a real frame
 * timestamp (PTS) so time-based effects stay correct under realtime pacing and
 * dropped frames; pass a negative value to use an internal wall clock measured
 * from the first frame. */
int slang_pipeline_run(struct slang_pipeline *p,
                       const uint8_t *src, uint8_t *dst, double time_sec);

/* Live-update a #pragma parameter by name. Sets the value (clamped to the
 * param's declared [min,max]) on every pass that declares `name`; the change
 * takes effect on the next slang_pipeline_run with no rebuild, because run()
 * re-reads each param into the push constants every frame.
 *
 * Returns the number of pass-level params updated (0 = name not found in any
 * pass, so callers can warn on typos). Safe to call between frames. */
int slang_pipeline_set_param(struct slang_pipeline *p,
                             const char *name, float value);

void slang_pipeline_destroy(struct slang_pipeline *p);

#endif /* VF_SLANG_PIPELINE_H */
