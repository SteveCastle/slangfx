/*
 * vf-slang — slang shader → SPIR-V compilation via shaderc.
 *
 * The libretro slang format is GLSL with a few preprocessor extensions:
 *   #pragma stage vertex      / #pragma stage fragment
 *   #pragma parameter NAME "DESC" DEFAULT MIN MAX STEP
 *   #pragma format <FORMAT>
 *
 * Algorithm:
 *   1. Read source.
 *   2. Strip and collect `#pragma parameter` and `#pragma format` lines.
 *      Their bodies are metadata for the host, not GLSL.
 *   3. Split the remaining text on `#pragma stage <name>`. Everything
 *      before the first stage pragma is the shared header (uniforms,
 *      structs, helpers visible to both stages).
 *   4. Build per-stage source as `header + body_for_that_stage`.
 *   5. Run each through shaderc → SPIR-V (Vulkan 1.3, SPIR-V 1.5).
 *
 * Reflection (push constant offsets, sampler bindings, parameter→push
 * field mapping) is deferred to Phase 4/7. For Phase 3 the result has
 * the SPIR-V blobs and the parameter declarations; layout discovery
 * happens by host convention until proper reflection is added.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "slang_compile.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shaderc/shaderc.h>

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = (char *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

static char *xstrdup_n(const char *s, size_t n)
{
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static char *err_fmt(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return xstrdup(buf);
}

/* -------------------------------------------------------------------------- */
/* Source scanner: walk lines, dispatch on `#pragma <kind>` directives.       */
/* -------------------------------------------------------------------------- */

enum stage { STAGE_NONE = 0, STAGE_VERTEX, STAGE_FRAGMENT };

struct stage_buf {
    char  *data;
    size_t n;
    size_t cap;
};

static int sb_append(struct stage_buf *b, const char *src, size_t len)
{
    if (b->n + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + len + 1) cap *= 2;
        char *t = (char *)realloc(b->data, cap);
        if (!t) return -1;
        b->data = t;
        b->cap  = cap;
    }
    memcpy(b->data + b->n, src, len);
    b->n += len;
    b->data[b->n] = '\0';
    return 0;
}

/* Returns the directive name and arg span if this line is `#pragma X`.
 * `line_start` points to start-of-line, `line_end` past last char (LF
 * not included). Sets *kind and *args (NULL if no args). Returns true if
 * the line is a recognized #pragma directive of the given kinds; false
 * otherwise. */
static bool is_pragma(const char *line_start, const char *line_end,
                      const char **kind_out, size_t *kind_len_out,
                      const char **args_out, size_t *args_len_out)
{
    const char *p = line_start;
    while (p < line_end && (*p == ' ' || *p == '\t')) ++p;
    if (line_end - p < 7) return false;
    if (memcmp(p, "#pragma", 7) != 0) return false;
    p += 7;
    if (p < line_end && !isspace((unsigned char)*p)) return false;
    while (p < line_end && (*p == ' ' || *p == '\t')) ++p;

    const char *k = p;
    while (p < line_end && !isspace((unsigned char)*p)) ++p;
    *kind_out = k;
    *kind_len_out = (size_t)(p - k);

    while (p < line_end && (*p == ' ' || *p == '\t')) ++p;
    *args_out = p;
    *args_len_out = (size_t)(line_end - p);
    return true;
}

static bool kind_eq(const char *kind, size_t kind_len, const char *want)
{
    return kind_len == strlen(want) && !memcmp(kind, want, kind_len);
}

/* Parse a `#pragma parameter NAME "DESC" DEFAULT MIN MAX STEP` argument
 * tail (without the leading "#pragma parameter "). On success populates
 * *p_out and returns true. Tolerant of missing fields beyond NAME and
 * DEFAULT — fills sensible defaults. */
static bool parse_param_line(const char *args, size_t len,
                             struct slang_param *p_out)
{
    char *line = xstrdup_n(args, len);
    if (!line) return false;

    char *cur = line;
    /* skip leading ws */
    while (*cur && isspace((unsigned char)*cur)) ++cur;

    /* NAME (until ws) */
    char *name = cur;
    while (*cur && !isspace((unsigned char)*cur)) ++cur;
    if (*cur) { *cur++ = '\0'; }
    while (*cur && isspace((unsigned char)*cur)) ++cur;

    /* DESC (quoted) */
    char *desc = "";
    if (*cur == '"') {
        ++cur;
        char *d = cur;
        while (*cur && *cur != '"') ++cur;
        if (*cur) { *cur++ = '\0'; }
        desc = d;
    }
    while (*cur && isspace((unsigned char)*cur)) ++cur;

    /* DEFAULT MIN MAX STEP — take however many tokens are present. */
    float vals[4] = {0.0f, 0.0f, 1.0f, 0.01f};
    int got = 0;
    while (got < 4 && *cur) {
        char *end;
        float v = (float)strtod(cur, &end);
        if (end == cur) break;
        vals[got++] = v;
        cur = end;
        while (*cur && isspace((unsigned char)*cur)) ++cur;
    }

    p_out->name          = xstrdup(name);
    p_out->desc          = xstrdup(desc);
    p_out->default_value = vals[0];
    p_out->min_value     = (got >= 2) ? vals[1] : 0.0f;
    p_out->max_value     = (got >= 3) ? vals[2] : 1.0f;
    p_out->step          = (got >= 4) ? vals[3] : 0.01f;
    p_out->push_offset   = 0;     /* assigned in Phase 7 reflection */

    free(line);
    return p_out->name != NULL;
}

/* Scan source: collect pragmas, build vertex_buf and fragment_buf. */
static int preprocess(const char *src, size_t src_len,
                      struct stage_buf *vert_buf,
                      struct stage_buf *frag_buf,
                      struct slang_module *mod,
                      char **err_out)
{
    enum stage current = STAGE_NONE;
    /* Header is captured separately and prepended to each stage buffer
     * once the first stage pragma is seen. */
    struct stage_buf header = {0};

    size_t param_cap = 0;

    const char *p = src;
    const char *end = src + src_len;
    int line_no = 0;

    while (p < end) {
        ++line_no;
        const char *line = p;
        while (p < end && *p != '\n') ++p;
        const char *line_end_no_lf = p;
        if (line_end_no_lf > line && line_end_no_lf[-1] == '\r') --line_end_no_lf;
        if (p < end) ++p;  /* consume LF */

        const char *kind = NULL, *args = NULL;
        size_t kind_len = 0, args_len = 0;

        if (is_pragma(line, line_end_no_lf, &kind, &kind_len, &args, &args_len)) {
            /* `#pragma stage <name>` — switch buffer */
            if (kind_eq(kind, kind_len, "stage")) {
                /* trim trailing ws of args */
                while (args_len > 0 && isspace((unsigned char)args[args_len - 1])) --args_len;
                if (args_len == 6 && !memcmp(args, "vertex", 6)) {
                    if (current == STAGE_NONE) {
                        /* commit current header into both stage bufs as preamble */
                        if (sb_append(vert_buf, header.data ? header.data : "", header.n) < 0 ||
                            sb_append(frag_buf, header.data ? header.data : "", header.n) < 0) {
                            if (err_out) *err_out = err_fmt("oom while seeding stage buffers");
                            free(header.data);
                            return -1;
                        }
                    }
                    current = STAGE_VERTEX;
                } else if (args_len == 8 && !memcmp(args, "fragment", 8)) {
                    if (current == STAGE_NONE) {
                        if (sb_append(vert_buf, header.data ? header.data : "", header.n) < 0 ||
                            sb_append(frag_buf, header.data ? header.data : "", header.n) < 0) {
                            if (err_out) *err_out = err_fmt("oom while seeding stage buffers");
                            free(header.data);
                            return -1;
                        }
                    }
                    current = STAGE_FRAGMENT;
                } else {
                    if (err_out) *err_out = err_fmt("unknown #pragma stage value at line %d", line_no);
                    free(header.data);
                    return -1;
                }
                /* Don't emit the directive into the output (shaderc would
                 * complain about an unknown pragma). Replace with a blank
                 * line so error line numbers stay close to the source. */
                struct stage_buf *target = (current == STAGE_VERTEX) ? vert_buf : frag_buf;
                if (sb_append(target, "\n", 1) < 0) { /* preserve line numbering */ }
                continue;
            }

            /* `#pragma parameter NAME "DESC" DEFAULT MIN MAX STEP` */
            if (kind_eq(kind, kind_len, "parameter")) {
                if (mod->num_params == param_cap) {
                    param_cap = param_cap ? param_cap * 2 : 4;
                    struct slang_param *t =
                        (struct slang_param *)realloc(mod->params,
                                                       param_cap * sizeof(*mod->params));
                    if (!t) {
                        if (err_out) *err_out = err_fmt("oom (param table)");
                        free(header.data);
                        return -1;
                    }
                    mod->params = t;
                }
                struct slang_param tmp;
                memset(&tmp, 0, sizeof(tmp));
                if (parse_param_line(args, args_len, &tmp)) {
                    mod->params[mod->num_params++] = tmp;
                }
                /* Don't pass the directive to shaderc. */
                if (current == STAGE_VERTEX)        sb_append(vert_buf, "\n", 1);
                else if (current == STAGE_FRAGMENT) sb_append(frag_buf, "\n", 1);
                else                                sb_append(&header,  "\n", 1);
                continue;
            }

            /* `#pragma format <FMT>` */
            if (kind_eq(kind, kind_len, "format")) {
                /* Recognized; collected but not emitted. The host honors it
                 * via slangp_format mapping in slang_pipeline.c. */
                if (current == STAGE_VERTEX)        sb_append(vert_buf, "\n", 1);
                else if (current == STAGE_FRAGMENT) sb_append(frag_buf, "\n", 1);
                else                                sb_append(&header,  "\n", 1);
                continue;
            }

            /* Unknown #pragma — fall through to emit it (shaderc will
             * either accept GL_KHR_shader_subgroup pragmas etc. or warn). */
        }

        /* Append the line including its LF (or virtual LF at EOF). */
        size_t emit_len = (size_t)(p - line);
        struct stage_buf *target = NULL;
        switch (current) {
            case STAGE_VERTEX:   target = vert_buf;  break;
            case STAGE_FRAGMENT: target = frag_buf;  break;
            case STAGE_NONE:
            default:             target = &header;   break;
        }
        if (sb_append(target, line, emit_len) < 0) {
            if (err_out) *err_out = err_fmt("oom while appending line %d", line_no);
            free(header.data);
            return -1;
        }
    }

    free(header.data);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* shaderc invocation                                                         */
/* -------------------------------------------------------------------------- */

static int compile_stage(shaderc_compiler_t compiler,
                         shaderc_compile_options_t options,
                         shaderc_shader_kind kind,
                         const char *source, size_t source_len,
                         const char *path,
                         uint32_t **spv_out, size_t *spv_words_out,
                         char **err_out)
{
    const char *display = path ? path : "<inline>";
    const char *entry   = "main";

    shaderc_compilation_result_t r = shaderc_compile_into_spv(
        compiler, source, source_len, kind, display, entry, options);
    if (!r) {
        if (err_out) *err_out = err_fmt("shaderc returned NULL result");
        return -1;
    }

    shaderc_compilation_status status = shaderc_result_get_compilation_status(r);
    if (status != shaderc_compilation_status_success) {
        const char *msg = shaderc_result_get_error_message(r);
        if (err_out) *err_out = err_fmt("shaderc %s compile failed (%d):\n%s",
                                        kind == shaderc_glsl_vertex_shader ? "vertex" : "fragment",
                                        (int)status, msg ? msg : "(no message)");
        shaderc_result_release(r);
        return -1;
    }

    size_t bytes = shaderc_result_get_length(r);
    const char *blob = shaderc_result_get_bytes(r);
    if (bytes % 4 != 0 || bytes == 0) {
        if (err_out) *err_out = err_fmt("shaderc returned non-word-aligned SPIR-V (%zu bytes)", bytes);
        shaderc_result_release(r);
        return -1;
    }

    uint32_t *out = (uint32_t *)malloc(bytes);
    if (!out) {
        if (err_out) *err_out = err_fmt("oom (spirv blob)");
        shaderc_result_release(r);
        return -1;
    }
    memcpy(out, blob, bytes);
    *spv_out       = out;
    *spv_words_out = bytes / 4;

    shaderc_result_release(r);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

struct slang_module *slang_compile_string(const char *src,
                                          const char *include_dir,
                                          char **err_out)
{
    (void)include_dir;  /* #include resolution is Phase 9 work */
    if (!src) { if (err_out) *err_out = xstrdup("null source"); return NULL; }

    struct slang_module *mod = (struct slang_module *)calloc(1, sizeof(*mod));
    if (!mod) { if (err_out) *err_out = xstrdup("oom"); return NULL; }

    struct stage_buf vert = {0}, frag = {0};
    if (preprocess(src, strlen(src), &vert, &frag, mod, err_out) != 0) {
        free(vert.data); free(frag.data);
        slang_module_free(mod);
        return NULL;
    }

    if (vert.n == 0 || frag.n == 0) {
        if (err_out) *err_out = xstrdup(
            "slang source is missing #pragma stage vertex or fragment");
        free(vert.data); free(frag.data);
        slang_module_free(mod);
        return NULL;
    }

    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    shaderc_compile_options_t options = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(options,
        shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_target_spirv(options, shaderc_spirv_version_1_5);
    shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_performance);

    int rc = compile_stage(compiler, options, shaderc_glsl_vertex_shader,
                           vert.data, vert.n, NULL,
                           &mod->vert_spv, &mod->vert_spv_words, err_out);
    if (rc == 0) {
        rc = compile_stage(compiler, options, shaderc_glsl_fragment_shader,
                           frag.data, frag.n, NULL,
                           &mod->frag_spv, &mod->frag_spv_words, err_out);
    }

    shaderc_compile_options_release(options);
    shaderc_compiler_release(compiler);
    free(vert.data);
    free(frag.data);

    if (rc != 0) {
        slang_module_free(mod);
        return NULL;
    }
    return mod;
}

struct slang_module *slang_compile_file(const char *path, char **err_out)
{
    if (!path) { if (err_out) *err_out = xstrdup("null path"); return NULL; }

    FILE *f = fopen(path, "rb");
    if (!f) { if (err_out) *err_out = err_fmt("open '%s' failed", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); if (err_out) *err_out = err_fmt("tell '%s'", path); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); if (err_out) *err_out = err_fmt("oom"); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[sz] = '\0';

    struct slang_module *m = slang_compile_string(buf, NULL, err_out);
    free(buf);
    if (m) m->path = xstrdup(path);
    return m;
}

void slang_module_free(struct slang_module *m)
{
    if (!m) return;
    free(m->path);
    free(m->vert_spv);
    free(m->frag_spv);
    for (size_t i = 0; i < m->num_push_fields; ++i) free(m->push_fields[i].name);
    free(m->push_fields);
    for (size_t i = 0; i < m->num_ubo_fields; ++i)  free(m->ubo_fields[i].name);
    free(m->ubo_fields);
    for (size_t i = 0; i < m->num_samplers; ++i)    free(m->samplers[i].name);
    free(m->samplers);
    for (size_t i = 0; i < m->num_params; ++i) {
        free(m->params[i].name);
        free(m->params[i].desc);
    }
    free(m->params);
    free(m);
}
