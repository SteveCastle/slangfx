/*
 * vf-slang — slang shader preset (.slangp) parser.
 *
 * Parses libretro-format INI-style preset files into a struct describing the
 * full multi-pass chain: shader paths, scale rules, sampling/wrap modes,
 * external textures, and runtime parameter overrides.
 *
 * The parser does not load shader source — it only declares what the
 * pipeline should expect. Source loading + slang→SPIR-V compilation lives
 * in slang_compile.h.
 *
 * Lifecycle:
 *
 *     struct slangp_preset *p = slangp_parse_file("crt/newpixie-crt.slangp");
 *     ...
 *     slangp_free(p);
 *
 * On error, slangp_parse_file returns NULL and sets *err_out (caller frees).
 */

#ifndef VF_SLANG_SLANGP_H
#define VF_SLANG_SLANGP_H

#include <stdbool.h>
#include <stddef.h>

enum slangp_scale_type {
    SLANGP_SCALE_SOURCE = 0,
    SLANGP_SCALE_VIEWPORT,
    SLANGP_SCALE_ABSOLUTE,
};

enum slangp_wrap_mode {
    SLANGP_WRAP_CLAMP_TO_BORDER = 0,
    SLANGP_WRAP_CLAMP_TO_EDGE,
    SLANGP_WRAP_REPEAT,
    SLANGP_WRAP_MIRRORED_REPEAT,
};

enum slangp_format {
    SLANGP_FORMAT_DEFAULT = 0,            /* let the shader decide via #pragma format */
    SLANGP_FORMAT_R8_UNORM,
    SLANGP_FORMAT_R8G8_UNORM,
    SLANGP_FORMAT_R8G8B8A8_UNORM,
    SLANGP_FORMAT_R8G8B8A8_SRGB,
    SLANGP_FORMAT_R10G10B10A2_UNORM,
    SLANGP_FORMAT_R16_UNORM,
    SLANGP_FORMAT_R16_SFLOAT,
    SLANGP_FORMAT_R16G16B16A16_UNORM,
    SLANGP_FORMAT_R16G16B16A16_SFLOAT,
    SLANGP_FORMAT_R32_SFLOAT,
    SLANGP_FORMAT_R32G32B32A32_SFLOAT,
};

struct slangp_pass {
    char *path;                      /* absolute or preset-relative .slang path */
    char *alias;                     /* optional alias used by downstream samplers */

    bool  filter_linear;
    bool  mipmap_input;
    enum slangp_wrap_mode wrap_mode;

    /* Per-axis scale rules. If only the uniform `scale_type<i>` was given in
     * the preset, both axes hold the same value. */
    enum slangp_scale_type scale_type_x;
    enum slangp_scale_type scale_type_y;
    float scale_x;
    float scale_y;

    /* fbo_format wins over (srgb_framebuffer | float_framebuffer) when set. */
    bool srgb_framebuffer;
    bool float_framebuffer;
    enum slangp_format fbo_format;

    unsigned frame_count_mod;        /* 0 = pass FrameCount through unchanged */
};

struct slangp_texture {
    char *name;
    char *path;
    bool  filter_linear;
    bool  mipmap;
    enum slangp_wrap_mode wrap_mode;
};

struct slangp_param_override {
    char  *name;
    float  value;
};

struct slangp_preset {
    char *base_dir;                  /* directory of the .slangp; for resolving relative paths */

    struct slangp_pass *passes;
    size_t num_passes;

    struct slangp_texture *textures;
    size_t num_textures;

    struct slangp_param_override *params;
    size_t num_params;
};

/* Parse a .slangp from disk. Returns NULL on failure; if err_out is non-NULL,
 * a malloc'd error string is stored there (caller frees). */
struct slangp_preset *slangp_parse_file(const char *path, char **err_out);

/* Parse from an in-memory buffer. base_dir is used to resolve relative
 * shader/texture paths; if NULL, paths are kept as-is. */
struct slangp_preset *slangp_parse_string(const char *src,
                                          const char *base_dir,
                                          char **err_out);

void slangp_free(struct slangp_preset *p);

/* Debug: print the parsed preset to stderr. */
void slangp_dump(const struct slangp_preset *p);

#endif /* VF_SLANG_SLANGP_H */
