/*
 * vf-slang — slang preset (.slangp) parser.
 *
 * Hand-written INI-style parser. Sufficient for the libretro slang-shaders
 * corpus; no external INI dependency.
 *
 * Format reference: see docs/slang_format.md.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "slangp.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Allocation + small-string helpers                                          */
/* ------------------------------------------------------------------------- */

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
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return xstrdup(buf);
}

/* ------------------------------------------------------------------------- */
/* Line / token utilities                                                     */
/* ------------------------------------------------------------------------- */

/* Trim ASCII whitespace in place (modifies *s). Returns pointer into s. */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) { *e = '\0'; --e; }
    return s;
}

/* Strip surrounding quotes if both present. Modifies in place. */
static char *unquote(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n-1] == '"') ||
                   (s[0] == '\'' && s[n-1] == '\''))) {
        s[n-1] = '\0';
        s++;
    }
    return s;
}

/* Tokenize semicolon-separated list inside an already-unquoted string.
 * Allocates *items (caller frees each + the array). Returns count. */
static size_t split_semi(const char *src, char ***items_out)
{
    *items_out = NULL;
    if (!src || !*src) return 0;

    size_t cap = 4, n = 0;
    char **items = (char **)malloc(cap * sizeof(*items));
    if (!items) return 0;

    const char *p = src;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) ++p;
        const char *start = p;
        while (*p && *p != ';') ++p;
        const char *end = p;
        while (end > start && isspace((unsigned char)end[-1])) --end;
        if (end > start) {
            if (n == cap) {
                cap *= 2;
                char **t = (char **)realloc(items, cap * sizeof(*items));
                if (!t) { /* OOM: free what we have */
                    for (size_t i = 0; i < n; ++i) free(items[i]);
                    free(items);
                    return 0;
                }
                items = t;
            }
            items[n++] = xstrdup_n(start, (size_t)(end - start));
        }
        if (*p == ';') ++p;
    }
    *items_out = items;
    return n;
}

/* True iff `key` ends in a string of digits; emits the trailing index and
 * a malloc'd copy of the prefix (without the digits). The prefix may end
 * in '_' (we keep it; callers normalize as needed). */
static bool split_indexed(const char *key, char **prefix_out, int *index_out)
{
    size_t len = strlen(key);
    size_t i = len;
    while (i > 0 && isdigit((unsigned char)key[i - 1])) --i;
    if (i == len) return false;
    *index_out = atoi(key + i);
    *prefix_out = xstrdup_n(key, i);
    return true;
}

/* Some presets write `scale_<i>` instead of the canonical `scale<i>`.
 * Strip a single trailing underscore for tolerant matching. */
static void strip_trailing_underscore(char *s)
{
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '_') s[n - 1] = '\0';
}

static bool parse_bool(const char *s, bool *out)
{
    if (!s) return false;
    if (!strcasecmp(s, "true")  || !strcasecmp(s, "yes") ||
        !strcasecmp(s, "on")    || !strcmp(s, "1"))   { *out = true;  return true; }
    if (!strcasecmp(s, "false") || !strcasecmp(s, "no") ||
        !strcasecmp(s, "off")   || !strcmp(s, "0"))   { *out = false; return true; }
    return false;
}

static bool parse_float(const char *s, float *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return false;
    *out = (float)v;
    return true;
}

static bool parse_uint(const char *s, unsigned *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s) return false;
    *out = (unsigned)v;
    return true;
}

static bool parse_scale_type(const char *s, enum slangp_scale_type *out)
{
    if (!s) return false;
    if (!strcasecmp(s, "source"))   { *out = SLANGP_SCALE_SOURCE;   return true; }
    if (!strcasecmp(s, "viewport")) { *out = SLANGP_SCALE_VIEWPORT; return true; }
    if (!strcasecmp(s, "absolute")) { *out = SLANGP_SCALE_ABSOLUTE; return true; }
    return false;
}

static bool parse_wrap_mode(const char *s, enum slangp_wrap_mode *out)
{
    if (!s) return false;
    if (!strcasecmp(s, "clamp_to_border")) { *out = SLANGP_WRAP_CLAMP_TO_BORDER; return true; }
    if (!strcasecmp(s, "clamp_to_edge"))   { *out = SLANGP_WRAP_CLAMP_TO_EDGE;   return true; }
    if (!strcasecmp(s, "repeat"))          { *out = SLANGP_WRAP_REPEAT;          return true; }
    if (!strcasecmp(s, "mirrored_repeat")) { *out = SLANGP_WRAP_MIRRORED_REPEAT; return true; }
    return false;
}

static bool parse_format(const char *s, enum slangp_format *out)
{
    if (!s) return false;
    static const struct { const char *n; enum slangp_format v; } table[] = {
        { "R8_UNORM",            SLANGP_FORMAT_R8_UNORM },
        { "R8G8_UNORM",          SLANGP_FORMAT_R8G8_UNORM },
        { "R8G8B8A8_UNORM",      SLANGP_FORMAT_R8G8B8A8_UNORM },
        { "R8G8B8A8_SRGB",       SLANGP_FORMAT_R8G8B8A8_SRGB },
        { "R10G10B10A2_UNORM",   SLANGP_FORMAT_R10G10B10A2_UNORM },
        { "R16_UNORM",           SLANGP_FORMAT_R16_UNORM },
        { "R16_SFLOAT",          SLANGP_FORMAT_R16_SFLOAT },
        { "R16G16B16A16_UNORM",  SLANGP_FORMAT_R16G16B16A16_UNORM },
        { "R16G16B16A16_SFLOAT", SLANGP_FORMAT_R16G16B16A16_SFLOAT },
        { "R32_SFLOAT",          SLANGP_FORMAT_R32_SFLOAT },
        { "R32G32B32A32_SFLOAT", SLANGP_FORMAT_R32G32B32A32_SFLOAT },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i) {
        if (!strcasecmp(s, table[i].n)) { *out = table[i].v; return true; }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Pair list (a flat array of key=value rows produced by the lexer)           */
/* ------------------------------------------------------------------------- */

struct kv { char *key; char *value; };

struct kv_list {
    struct kv *items;
    size_t     n;
    size_t     cap;
};

static void kv_list_free(struct kv_list *l)
{
    for (size_t i = 0; i < l->n; ++i) {
        free(l->items[i].key);
        free(l->items[i].value);
    }
    free(l->items);
    l->items = NULL; l->n = 0; l->cap = 0;
}

static int kv_list_push(struct kv_list *l, const char *key, const char *value)
{
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        struct kv *t = (struct kv *)realloc(l->items, cap * sizeof(*t));
        if (!t) return -1;
        l->items = t;
        l->cap = cap;
    }
    l->items[l->n].key   = xstrdup(key);
    l->items[l->n].value = xstrdup(value ? value : "");
    if (!l->items[l->n].key || !l->items[l->n].value) return -1;
    ++l->n;
    return 0;
}

static const char *kv_find(const struct kv_list *l, const char *key)
{
    for (size_t i = 0; i < l->n; ++i)
        if (!strcmp(l->items[i].key, key))
            return l->items[i].value;
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Lexer                                                                      */
/* ------------------------------------------------------------------------- */

/* Read entire file into a heap buffer; returns NULL on error (sets *err). */
static char *slurp_file(const char *path, char **err_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { if (err_out) *err_out = err_fmt("open '%s' failed", path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); if (err_out) *err_out = err_fmt("seek '%s'", path); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); if (err_out) *err_out = err_fmt("tell '%s'", path); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); if (err_out) *err_out = err_fmt("oom reading '%s'", path); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* Tokenize an INI-style buffer into a list of (key, value) pairs.
 * Lines starting with `#` (after whitespace) are comments. Trailing inline
 * comments are not stripped — # inside values is rare in slang-shaders and
 * RetroArch keeps it; we follow suit. */
static int lex_pairs(const char *src, struct kv_list *out, char **err)
{
    const char *p = src;
    int line_no = 0;
    while (*p) {
        ++line_no;
        const char *line_start = p;
        while (*p && *p != '\n' && *p != '\r') ++p;
        size_t len = (size_t)(p - line_start);
        while (*p == '\n' || *p == '\r') ++p;

        char *line = xstrdup_n(line_start, len);
        if (!line) { if (err) *err = err_fmt("oom"); return -1; }

        char *t = trim(line);
        if (!*t || *t == '#') { free(line); continue; }

        char *eq = strchr(t, '=');
        if (!eq) {
            /* Lines without '=' are treated as bare flags with empty value
             * (rare in slang-shaders; we tolerate). */
            kv_list_push(out, t, "");
            free(line);
            continue;
        }
        *eq = '\0';
        char *key = trim(t);
        char *val = trim(eq + 1);
        val = unquote(val);

        if (!*key) { free(line); continue; }

        if (kv_list_push(out, key, val) < 0) {
            if (err) *err = err_fmt("oom");
            free(line);
            return -1;
        }
        free(line);
    }
    (void)line_no;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Pass / texture / parameter assembly                                        */
/* ------------------------------------------------------------------------- */

static void pass_init(struct slangp_pass *p)
{
    memset(p, 0, sizeof(*p));
    p->scale_x = 1.0f;
    p->scale_y = 1.0f;
    p->scale_type_x = SLANGP_SCALE_SOURCE;
    p->scale_type_y = SLANGP_SCALE_SOURCE;
    p->filter_linear = false;
    p->mipmap_input  = false;
    p->wrap_mode     = SLANGP_WRAP_CLAMP_TO_BORDER;
    p->fbo_format    = SLANGP_FORMAT_DEFAULT;
}

/* Try to resolve `key` as an indexed pass attribute; on match, populate
 * the right field of passes[i]. Returns: 1 = consumed, 0 = not a pass key,
 * -1 = error. */
static int apply_pass_field(struct slangp_pass *passes, size_t n_passes,
                            const char *key, const char *value)
{
    char *prefix = NULL;
    int idx = 0;
    if (!split_indexed(key, &prefix, &idx)) return 0;
    if (idx < 0 || (size_t)idx >= n_passes) { free(prefix); return 0; }

    /* Tolerate the `scale_<i>` typo by stripping a trailing '_' on prefix
     * when it doesn't otherwise match a per-axis variant. */
    size_t plen = strlen(prefix);
    if (plen > 0 && prefix[plen - 1] == '_') {
        if (strcmp(prefix, "scale_") == 0 ||
            strcmp(prefix, "filter_linear_") == 0 ||
            strcmp(prefix, "mipmap_input_") == 0 ||
            strcmp(prefix, "wrap_mode_") == 0 ||
            strcmp(prefix, "frame_count_mod_") == 0 ||
            strcmp(prefix, "fbo_format_") == 0 ||
            strcmp(prefix, "shader_") == 0 ||
            strcmp(prefix, "alias_") == 0 ||
            strcmp(prefix, "srgb_framebuffer_") == 0 ||
            strcmp(prefix, "float_framebuffer_") == 0)
            strip_trailing_underscore(prefix);
    }

    int rc = 1;
    struct slangp_pass *ps = &passes[idx];

    if      (!strcmp(prefix, "shader"))             { free(ps->path);  ps->path  = xstrdup(value); }
    else if (!strcmp(prefix, "alias"))              { free(ps->alias); ps->alias = xstrdup(value); }
    else if (!strcmp(prefix, "filter_linear"))      { parse_bool(value, &ps->filter_linear); }
    else if (!strcmp(prefix, "mipmap_input"))       { parse_bool(value, &ps->mipmap_input); }
    else if (!strcmp(prefix, "wrap_mode"))          { parse_wrap_mode(value, &ps->wrap_mode); }
    else if (!strcmp(prefix, "frame_count_mod"))    { parse_uint(value, &ps->frame_count_mod); }
    else if (!strcmp(prefix, "srgb_framebuffer"))   { parse_bool(value, &ps->srgb_framebuffer); }
    else if (!strcmp(prefix, "float_framebuffer"))  { parse_bool(value, &ps->float_framebuffer); }
    else if (!strcmp(prefix, "fbo_format"))         { parse_format(value, &ps->fbo_format); }
    else if (!strcmp(prefix, "scale")) {
        float v = 0; if (parse_float(value, &v)) { ps->scale_x = v; ps->scale_y = v; }
    }
    else if (!strcmp(prefix, "scale_x"))            { parse_float(value, &ps->scale_x); }
    else if (!strcmp(prefix, "scale_y"))            { parse_float(value, &ps->scale_y); }
    else if (!strcmp(prefix, "scale_type")) {
        enum slangp_scale_type t;
        if (parse_scale_type(value, &t)) { ps->scale_type_x = t; ps->scale_type_y = t; }
    }
    else if (!strcmp(prefix, "scale_type_x"))       { parse_scale_type(value, &ps->scale_type_x); }
    else if (!strcmp(prefix, "scale_type_y"))       { parse_scale_type(value, &ps->scale_type_y); }
    else { rc = 0; }

    free(prefix);
    return rc;
}

/* Resolve `path` against `base_dir` if relative. Caller frees. */
static char *resolve_path(const char *base_dir, const char *path)
{
    if (!path) return NULL;
    if (!base_dir) return xstrdup(path);

    /* Absolute on POSIX or Windows? */
    bool abs = (path[0] == '/' || path[0] == '\\') ||
               (isalpha((unsigned char)path[0]) && path[1] == ':');
    if (abs) return xstrdup(path);

    size_t base_len = strlen(base_dir);
    size_t need = base_len + 1 + strlen(path) + 1;
    char *out = (char *)malloc(need);
    if (!out) return NULL;
    memcpy(out, base_dir, base_len);
    char sep = '/';
    if (base_len && (base_dir[base_len - 1] == '/' || base_dir[base_len - 1] == '\\')) {
        out[base_len] = '\0';
    } else {
        out[base_len] = sep;
        out[base_len + 1] = '\0';
    }
    strcat(out, path);
    return out;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

struct slangp_preset *slangp_parse_string(const char *src,
                                          const char *base_dir,
                                          char **err_out)
{
    if (!src) { if (err_out) *err_out = xstrdup("null source"); return NULL; }

    struct kv_list pairs = {0};
    if (lex_pairs(src, &pairs, err_out) != 0) {
        kv_list_free(&pairs);
        return NULL;
    }

    /* shaders = N */
    const char *shaders_str = kv_find(&pairs, "shaders");
    unsigned n_passes = 0;
    if (!shaders_str || !parse_uint(shaders_str, &n_passes)) {
        if (err_out) *err_out = xstrdup("missing or invalid 'shaders' count");
        kv_list_free(&pairs);
        return NULL;
    }

    struct slangp_preset *p = (struct slangp_preset *)calloc(1, sizeof(*p));
    if (!p) { if (err_out) *err_out = xstrdup("oom"); kv_list_free(&pairs); return NULL; }
    p->base_dir = base_dir ? xstrdup(base_dir) : NULL;

    if (n_passes > 0) {
        p->passes = (struct slangp_pass *)calloc(n_passes, sizeof(*p->passes));
        if (!p->passes) {
            if (err_out) *err_out = xstrdup("oom");
            slangp_free(p);
            kv_list_free(&pairs);
            return NULL;
        }
        p->num_passes = n_passes;
        for (size_t i = 0; i < n_passes; ++i) pass_init(&p->passes[i]);
    }

    /* Distribute indexed pass keys. */
    for (size_t i = 0; i < pairs.n; ++i)
        apply_pass_field(p->passes, p->num_passes, pairs.items[i].key, pairs.items[i].value);

    /* Resolve relative shader paths. */
    if (p->base_dir) {
        for (size_t i = 0; i < p->num_passes; ++i) {
            char *r = resolve_path(p->base_dir, p->passes[i].path);
            free(p->passes[i].path);
            p->passes[i].path = r;
        }
    }

    /* External textures. */
    const char *tex_list = kv_find(&pairs, "textures");
    if (tex_list && *tex_list) {
        char **names = NULL;
        size_t nn = split_semi(tex_list, &names);
        if (nn > 0) {
            p->textures = (struct slangp_texture *)calloc(nn, sizeof(*p->textures));
            if (p->textures) {
                p->num_textures = nn;
                for (size_t i = 0; i < nn; ++i) {
                    struct slangp_texture *t = &p->textures[i];
                    t->name        = names[i];               /* take ownership */
                    t->wrap_mode   = SLANGP_WRAP_CLAMP_TO_BORDER;
                    t->filter_linear = false;
                    t->mipmap      = false;

                    const char *path = kv_find(&pairs, t->name);
                    t->path = p->base_dir ? resolve_path(p->base_dir, path) : xstrdup(path ? path : "");

                    char buf[256];
                    snprintf(buf, sizeof(buf), "%s_linear", t->name);
                    const char *v;
                    if ((v = kv_find(&pairs, buf))) parse_bool(v, &t->filter_linear);
                    snprintf(buf, sizeof(buf), "%s_mipmap", t->name);
                    if ((v = kv_find(&pairs, buf))) parse_bool(v, &t->mipmap);
                    snprintf(buf, sizeof(buf), "%s_wrap_mode", t->name);
                    if ((v = kv_find(&pairs, buf))) parse_wrap_mode(v, &t->wrap_mode);
                }
            } else {
                for (size_t i = 0; i < nn; ++i) free(names[i]);
            }
            free(names);
        }
    }

    /* Parameter overrides. */
    const char *param_list = kv_find(&pairs, "parameters");
    if (param_list && *param_list) {
        char **names = NULL;
        size_t nn = split_semi(param_list, &names);
        if (nn > 0) {
            p->params = (struct slangp_param_override *)calloc(nn, sizeof(*p->params));
            if (p->params) {
                size_t kept = 0;
                for (size_t i = 0; i < nn; ++i) {
                    const char *v = kv_find(&pairs, names[i]);
                    float fv = 0.0f;
                    if (v && parse_float(v, &fv)) {
                        p->params[kept].name  = names[i];   /* take ownership */
                        p->params[kept].value = fv;
                        ++kept;
                    } else {
                        free(names[i]);
                    }
                }
                p->num_params = kept;
            } else {
                for (size_t i = 0; i < nn; ++i) free(names[i]);
            }
            free(names);
        }
    }

    kv_list_free(&pairs);
    return p;
}

/* Compute base_dir from a file path (everything up to the last '/' or '\').
 * Returns malloc'd string; "." if no separator found. */
static char *dirname_of(const char *path)
{
    const char *last = NULL;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') last = p;
    if (!last) return xstrdup(".");
    return xstrdup_n(path, (size_t)(last - path));
}

struct slangp_preset *slangp_parse_file(const char *path, char **err_out)
{
    char *src = slurp_file(path, err_out);
    if (!src) return NULL;
    char *base = dirname_of(path);
    struct slangp_preset *p = slangp_parse_string(src, base, err_out);
    free(src);
    free(base);
    return p;
}

void slangp_free(struct slangp_preset *p)
{
    if (!p) return;
    free(p->base_dir);
    for (size_t i = 0; i < p->num_passes; ++i) {
        free(p->passes[i].path);
        free(p->passes[i].alias);
    }
    free(p->passes);
    for (size_t i = 0; i < p->num_textures; ++i) {
        free(p->textures[i].name);
        free(p->textures[i].path);
    }
    free(p->textures);
    for (size_t i = 0; i < p->num_params; ++i) {
        free(p->params[i].name);
    }
    free(p->params);
    free(p);
}

void slangp_dump(const struct slangp_preset *p)
{
    if (!p) {
        fprintf(stderr, "(null preset)\n");
        return;
    }
    fprintf(stderr, "preset (base_dir=%s, %zu passes, %zu textures, %zu param overrides):\n",
            p->base_dir ? p->base_dir : "(none)",
            p->num_passes, p->num_textures, p->num_params);
    for (size_t i = 0; i < p->num_passes; ++i) {
        const struct slangp_pass *ps = &p->passes[i];
        fprintf(stderr, "  pass[%zu] %s\n",       i, ps->path ? ps->path : "(no path)");
        fprintf(stderr, "          alias=%s\n",       ps->alias ? ps->alias : "-");
        fprintf(stderr, "          scale=%.3f x %.3f, type=(%d,%d)\n",
                (double)ps->scale_x, (double)ps->scale_y,
                ps->scale_type_x, ps->scale_type_y);
        fprintf(stderr, "          linear=%d mipmap=%d wrap=%d frame_mod=%u fmt=%d\n",
                ps->filter_linear, ps->mipmap_input, ps->wrap_mode,
                ps->frame_count_mod, ps->fbo_format);
    }
    for (size_t i = 0; i < p->num_textures; ++i) {
        const struct slangp_texture *t = &p->textures[i];
        fprintf(stderr, "  texture %s = %s (linear=%d wrap=%d mipmap=%d)\n",
                t->name, t->path, t->filter_linear, t->wrap_mode, t->mipmap);
    }
    for (size_t i = 0; i < p->num_params; ++i)
        fprintf(stderr, "  param  %s = %.6f\n",
                p->params[i].name, (double)p->params[i].value);
}
