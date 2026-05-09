/*
 * vf-slang — Vulkan render pipeline (stub).
 *
 * Phase 0 stub. Phase 1 lands the device init + single-pass dispatch;
 * Phase 5 the multi-pass chain; Phase 6 the feedback rings.
 */

#include "slang_pipeline.h"

#include <stdlib.h>
#include <string.h>

struct slang_pipeline {
    unsigned input_w, input_h;
    unsigned output_w, output_h;
    /* Phase 1+ adds:
     *   VkInstance, VkDevice, VkQueue
     *   VkCommandPool
     *   VkImage / VkImageView for each pass's framebuffer + each ring slot
     *   VkPipeline / VkPipelineLayout per pass
     *   VkDescriptorSetLayout / pool
     *   shaderc_compiler_t
     *   slang_module *modules[num_passes]
     */
};

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

struct slang_pipeline *slang_pipeline_create(const struct slangp_preset *preset,
                                             unsigned input_w, unsigned input_h,
                                             unsigned output_w, unsigned output_h,
                                             char **err_out)
{
    (void)preset;
    if (err_out) *err_out = xstrdup("slang_pipeline_create: not yet implemented (Phase 1)");

    struct slang_pipeline *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->input_w = input_w;
    p->input_h = input_h;
    p->output_w = output_w;
    p->output_h = output_h;
    return p;
}

int slang_pipeline_run(struct slang_pipeline *p,
                       const uint8_t *src, uint8_t *dst)
{
    if (!p || !src || !dst) return -1;
    /* Stub: identity copy (no shader applied). Lets vf_slang.c's
     * filter_frame loop be testable end-to-end before any GPU code lands. */
    size_t bytes = (size_t)p->output_w * p->output_h * 4;
    memcpy(dst, src, bytes);
    return 0;
}

void slang_pipeline_destroy(struct slang_pipeline *p)
{
    free(p);
}
