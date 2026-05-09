/*
 * vf-slang — slang → SPIR-V compilation (stub).
 *
 * Phase 0 stub. Phase 3 lands the real implementation:
 *   1. Read .slang file.
 *   2. Run libretro slang preprocessor (port subset of libretro/glslang).
 *   3. Split on `#pragma stage vertex` / `#pragma stage fragment`.
 *   4. Feed each half to shaderc_compile_into_spv with target Vulkan 1.3.
 *   5. Reflect on SPIR-V (manual decoration scan, or spirv-cross) to
 *      populate push/ubo/sampler/parameter tables.
 */

#include "slang_compile.h"

#include <stdlib.h>
#include <string.h>

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

struct slang_module *slang_compile_file(const char *path, char **err_out)
{
    (void)path;
    if (err_out) *err_out = xstrdup("slang_compile_file: not yet implemented (Phase 3)");
    return NULL;
}

struct slang_module *slang_compile_string(const char *src,
                                          const char *include_dir,
                                          char **err_out)
{
    (void)src;
    (void)include_dir;
    if (err_out) *err_out = xstrdup("slang_compile_string: not yet implemented (Phase 3)");
    return NULL;
}

void slang_module_free(struct slang_module *m)
{
    if (!m) return;
    free(m->path);
    free(m->vert_spv);
    free(m->frag_spv);
    for (size_t i = 0; i < m->num_push_fields; ++i)
        free(m->push_fields[i].name);
    free(m->push_fields);
    for (size_t i = 0; i < m->num_ubo_fields; ++i)
        free(m->ubo_fields[i].name);
    free(m->ubo_fields);
    for (size_t i = 0; i < m->num_samplers; ++i)
        free(m->samplers[i].name);
    free(m->samplers);
    for (size_t i = 0; i < m->num_params; ++i) {
        free(m->params[i].name);
        free(m->params[i].desc);
    }
    free(m->params);
    free(m);
}
