/*
 * vf-slang — slang preset parser.
 *
 * Phase 0 stub: declares the entry points. Real implementation lands in
 * Phase 2 (see docs/roadmap.md). The parser is small enough to be
 * hand-written; we deliberately avoid pulling in an INI library so the
 * filter has minimal external deps.
 */

#include "slangp.h"

#include <stdio.h>
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

struct slangp_preset *slangp_parse_file(const char *path, char **err_out)
{
    (void)path;
    if (err_out) *err_out = xstrdup("slangp_parse_file: not yet implemented (Phase 2)");
    return NULL;
}

struct slangp_preset *slangp_parse_string(const char *src,
                                          const char *base_dir,
                                          char **err_out)
{
    (void)src;
    (void)base_dir;
    if (err_out) *err_out = xstrdup("slangp_parse_string: not yet implemented (Phase 2)");
    return NULL;
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
        fprintf(stderr, "  pass[%zu] %s (alias=%s) scale=%g/%g linear=%d wrap=%d\n",
                i, ps->path,
                ps->alias ? ps->alias : "-",
                (double)ps->scale_x, (double)ps->scale_y,
                ps->filter_linear, ps->wrap_mode);
    }
}
