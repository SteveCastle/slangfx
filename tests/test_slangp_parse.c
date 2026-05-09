/*
 * vf-slang — unit tests for the .slangp parser.
 *
 * Phase 0: tests for the stub interface only (free of NULL is no-op,
 * unimplemented entry points return error). Phase 2 fills these in
 * with real fixtures.
 *
 * Run via:
 *   meson test -C build slangp\ parser
 */

#include "../src/slangp.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_parse_unimplemented_returns_null(void)
{
    char *err = NULL;
    struct slangp_preset *p = slangp_parse_file("nonexistent.slangp", &err);
    assert(p == NULL);
    assert(err != NULL);
    assert(strstr(err, "Phase 2") != NULL);  /* helpful error during scaffolding */
    free(err);
    return 0;
}

static int test_free_null_safe(void)
{
    slangp_free(NULL);    /* must not crash */
    return 0;
}

static int test_dump_null_safe(void)
{
    slangp_dump(NULL);    /* prints "(null preset)\n" */
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += test_parse_unimplemented_returns_null();
    failures += test_free_null_safe();
    failures += test_dump_null_safe();
    if (failures == 0)
        fprintf(stderr, "OK: 3 tests passed\n");
    return failures == 0 ? 0 : 1;
}
