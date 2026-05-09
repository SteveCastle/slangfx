/*
 * vf-slang — .slangp parser tests.
 *
 * Run via:  meson test -C build slangp\ parser
 */

#define _CRT_SECURE_NO_WARNINGS
#include "../src/slangp.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); return 1; } \
} while (0)

static int test_minimal(void)
{
    char *err = NULL;
    struct slangp_preset *p = slangp_parse_string("shaders = 1\nshader0 = a.slang\n", NULL, &err);
    EXPECT(p, "parse failed");
    EXPECT(p->num_passes == 1, "expected 1 pass");
    EXPECT(p->passes[0].path && !strcmp(p->passes[0].path, "a.slang"), "shader0 path mismatch");
    EXPECT(p->passes[0].scale_x == 1.0f, "default scale_x");
    EXPECT(p->passes[0].scale_type_x == SLANGP_SCALE_SOURCE, "default scale_type_x");
    EXPECT(p->passes[0].filter_linear == false, "default filter_linear");
    slangp_free(p);
    free(err);
    return 0;
}

static int test_missing_count_errors(void)
{
    char *err = NULL;
    struct slangp_preset *p = slangp_parse_string("# nothing useful\n", NULL, &err);
    EXPECT(!p, "should fail without shaders=");
    EXPECT(err && strstr(err, "shaders") != NULL, "error mentions shaders");
    free(err);
    return 0;
}

static int test_newpixie_shape(void)
{
    /* Mirrors crt/newpixie-crt.slangp. Verifies the full feature set:
     * pass count, indexed keys, alias, scale_type, scale, filter_linear,
     * external textures with sub-options, scale_<i> typo tolerance. */
    static const char *src =
        "shaders = 4\n"
        "\n"
        "shader0 = shaders/newpixie/accumulate.slang\n"
        "alias0 = accum1\n"
        "scale0 = 1.0\n"
        "scale_type0 = source\n"
        "filter_linear0 = true\n"
        "\n"
        "shader1 = shaders/newpixie/blur_horiz.slang\n"
        "alias1 = blur1\n"
        "scale_1 = 1.0\n"  /* note the underscore typo we tolerate */
        "scale_type1 = source\n"
        "filter_linear1 = true\n"
        "\n"
        "shader2 = shaders/newpixie/blur_vert.slang\n"
        "alias2 = blur2\n"
        "scale_2 = 1.0\n"
        "scale_type2 = source\n"
        "filter_linear2 = true\n"
        "\n"
        "shader3 = shaders/newpixie/newpixie-crt.slang\n"
        "filter_linear3 = true\n"
        "scale_type3 = viewport\n"
        "\n"
        "textures = \"frametexture\"\n"
        "frametexture = shaders/newpixie/crtframe.png\n"
        "frametexture_linear = true\n"
        "frametexture_wrap_mode = clamp_to_border\n";

    char *err = NULL;
    struct slangp_preset *p = slangp_parse_string(src, "/tmp/preset_dir", &err);
    EXPECT(p, "parse failed");
    EXPECT(p->num_passes == 4, "expected 4 passes");

    EXPECT(!strcmp(p->passes[0].alias, "accum1"), "pass0 alias");
    EXPECT(p->passes[0].filter_linear, "pass0 linear");
    EXPECT(p->passes[0].scale_x == 1.0f && p->passes[0].scale_y == 1.0f, "pass0 scale");
    EXPECT(p->passes[0].scale_type_x == SLANGP_SCALE_SOURCE, "pass0 scale_type x");
    EXPECT(p->passes[0].scale_type_y == SLANGP_SCALE_SOURCE, "pass0 scale_type y");

    EXPECT(!strcmp(p->passes[1].alias, "blur1"), "pass1 alias");
    EXPECT(p->passes[1].scale_x == 1.0f, "pass1 scale (with underscore typo) parsed");

    EXPECT(p->passes[3].alias == NULL, "pass3 has no alias");
    EXPECT(p->passes[3].scale_type_x == SLANGP_SCALE_VIEWPORT, "pass3 viewport scale type");

    /* Path resolution against base_dir */
    EXPECT(p->passes[0].path != NULL, "pass0 path resolved");
    EXPECT(strstr(p->passes[0].path, "shaders/newpixie/accumulate.slang") != NULL,
           "pass0 path contains relative tail");
    EXPECT(strstr(p->passes[0].path, "/tmp/preset_dir") == p->passes[0].path,
           "pass0 path begins with base_dir");

    /* Texture */
    EXPECT(p->num_textures == 1, "one texture declared");
    EXPECT(!strcmp(p->textures[0].name, "frametexture"), "texture name");
    EXPECT(p->textures[0].path && strstr(p->textures[0].path, "crtframe.png") != NULL,
           "texture path");
    EXPECT(p->textures[0].filter_linear, "texture filter_linear");
    EXPECT(p->textures[0].wrap_mode == SLANGP_WRAP_CLAMP_TO_BORDER, "texture wrap");

    slangp_free(p);
    free(err);
    return 0;
}

static int test_per_axis_scale_and_format(void)
{
    static const char *src =
        "shaders = 1\n"
        "shader0 = a.slang\n"
        "scale_x0 = 0.5\n"
        "scale_y0 = 2.0\n"
        "scale_type_x0 = absolute\n"
        "scale_type_y0 = viewport\n"
        "fbo_format0 = R16G16B16A16_SFLOAT\n"
        "wrap_mode0 = mirrored_repeat\n"
        "frame_count_mod0 = 3\n"
        "mipmap_input0 = true\n";
    char *err = NULL;
    struct slangp_preset *p = slangp_parse_string(src, NULL, &err);
    EXPECT(p, "parse");
    EXPECT(p->passes[0].scale_x == 0.5f, "scale_x");
    EXPECT(p->passes[0].scale_y == 2.0f, "scale_y");
    EXPECT(p->passes[0].scale_type_x == SLANGP_SCALE_ABSOLUTE, "scale_type_x");
    EXPECT(p->passes[0].scale_type_y == SLANGP_SCALE_VIEWPORT, "scale_type_y");
    EXPECT(p->passes[0].fbo_format == SLANGP_FORMAT_R16G16B16A16_SFLOAT, "fbo_format");
    EXPECT(p->passes[0].wrap_mode == SLANGP_WRAP_MIRRORED_REPEAT, "wrap_mode");
    EXPECT(p->passes[0].frame_count_mod == 3, "frame_count_mod");
    EXPECT(p->passes[0].mipmap_input == true, "mipmap_input");
    slangp_free(p);
    free(err);
    return 0;
}

static int test_parameters(void)
{
    static const char *src =
        "shaders = 1\n"
        "shader0 = a.slang\n"
        "parameters = \"alpha;beta;missing_value_skipped\"\n"
        "alpha = 0.5\n"
        "beta = 1.5\n";
    char *err = NULL;
    struct slangp_preset *p = slangp_parse_string(src, NULL, &err);
    EXPECT(p, "parse");
    EXPECT(p->num_params == 2, "two parameters retained (skipped one with no value)");
    EXPECT(!strcmp(p->params[0].name, "alpha") && p->params[0].value == 0.5f, "alpha");
    EXPECT(!strcmp(p->params[1].name, "beta")  && p->params[1].value == 1.5f, "beta");
    slangp_free(p);
    free(err);
    return 0;
}

static int test_comments_and_whitespace(void)
{
    static const char *src =
        "  # comment\n"
        "shaders = 2  \n"
        "  shader0  =  shaders/a.slang  \n"
        "shader1=shaders/b.slang\n"
        "# trailing comment\n";
    char *err = NULL;
    struct slangp_preset *p = slangp_parse_string(src, NULL, &err);
    EXPECT(p, "parse");
    EXPECT(p->num_passes == 2, "2 passes");
    EXPECT(!strcmp(p->passes[0].path, "shaders/a.slang"), "pass 0");
    EXPECT(!strcmp(p->passes[1].path, "shaders/b.slang"), "pass 1");
    slangp_free(p);
    free(err);
    return 0;
}

static int test_free_null_safe(void)
{
    slangp_free(NULL);
    slangp_dump(NULL);
    return 0;
}

int main(void)
{
    int (*tests[])(void) = {
        test_minimal,
        test_missing_count_errors,
        test_newpixie_shape,
        test_per_axis_scale_and_format,
        test_parameters,
        test_comments_and_whitespace,
        test_free_null_safe,
    };
    int total = (int)(sizeof(tests) / sizeof(*tests));
    int failures = 0;
    for (int i = 0; i < total; ++i) failures += tests[i]();
    if (failures == 0) fprintf(stderr, "OK: %d tests passed\n", total);
    else               fprintf(stderr, "FAIL: %d/%d tests failed\n", failures, total);
    return failures == 0 ? 0 : 1;
}
