/*
 * vf-slang — slang shader → SPIR-V compilation.
 *
 * Each .slang file is a single source containing both vertex and fragment
 * stages, separated by `#pragma stage <name>`. We split it, run each half
 * through shaderc to produce SPIR-V, then reflect on the SPIR-V to extract
 * push-constant layout, UBO layout, sampler bindings, and parameter
 * declarations. The output structs are everything slang_pipeline needs to
 * build the corresponding Vulkan graphics pipeline.
 */

#ifndef VF_SLANG_COMPILE_H
#define VF_SLANG_COMPILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "slangp.h"  /* for enum slangp_format */

/* A parameter declared via `#pragma parameter`. */
struct slang_param {
    char  *name;
    char  *desc;
    float  default_value;
    float  min_value;
    float  max_value;
    float  step;
    /* Offset within the Push block (bytes). Filled at reflection time. */
    uint32_t push_offset;
};

/* One sampler binding declared in the shader (e.g. `sampler2D Source`). */
struct slang_sampler {
    char    *name;
    uint32_t descriptor_set;
    uint32_t binding;
};

/* A scalar field in the Push block we recognize as one of the standard
 * shader-system inputs. Custom #pragma parameter fields are listed in
 * `params` instead. */
struct slang_push_field {
    char    *name;            /* e.g. "SourceSize", "FrameCount" */
    uint32_t offset;          /* byte offset in the Push struct */
    uint32_t size;            /* in bytes */
};

struct slang_module {
    /* Source path (resolved). */
    char *path;

    /* SPIR-V bytecode. Owned; freed by slang_module_free. */
    uint32_t *vert_spv;
    size_t    vert_spv_words;
    uint32_t *frag_spv;
    size_t    frag_spv_words;

    /* Push constant block layout. */
    uint32_t push_total_size;
    struct slang_push_field *push_fields;
    size_t                   num_push_fields;

    /* UBO at set=0, binding=0. May be empty in trivial shaders. */
    uint32_t ubo_total_size;
    struct slang_push_field *ubo_fields;  /* same struct shape */
    size_t                   num_ubo_fields;

    /* Sampler bindings. */
    struct slang_sampler *samplers;
    size_t                num_samplers;

    /* Runtime-tunable parameters declared by this shader. */
    struct slang_param *params;
    size_t              num_params;

    /* Optional override of the framebuffer format (from #pragma format). */
    enum slangp_format fbo_format_pragma;
};

/* Compile a .slang source file. The path is used for #include resolution
 * relative to its directory.
 *
 * Returns NULL on failure; *err_out (if non-NULL) gets a malloc'd error
 * message containing the shaderc/glslang diagnostic. */
struct slang_module *slang_compile_file(const char *path, char **err_out);

/* Compile from an in-memory source. include_dir is searched for #include
 * resolution; pass NULL to disable includes. */
struct slang_module *slang_compile_string(const char *src,
                                          const char *include_dir,
                                          char **err_out);

void slang_module_free(struct slang_module *m);

#endif /* VF_SLANG_COMPILE_H */
