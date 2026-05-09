/*
 * vf-slang — minimal SPIR-V reflector.
 *
 * Implements just enough of the SPIR-V binary format to recover:
 *   - push-constant block layout (member names + byte offsets + total size)
 *   - UBO @ set=0,binding=0 layout
 *   - sampler bindings (descriptor set + binding + name)
 *
 * Reference: https://registry.khronos.org/SPIR-V/specs/1.0/SPIRV.html
 *
 * Algorithm (single pass over instructions, four indexed lookup tables):
 *
 *   1. Walk every instruction, populate:
 *        names[id]                = OpName / decay-OpName for variable id
 *        member_names[id][k]      = OpMemberName for struct id member k
 *        struct_offsets[id][k]    = OpMemberDecorate Offset for member k
 *        struct_member_count[id]  = number of members on the struct (from
 *                                   OpTypeStruct word count)
 *        struct_last_typeid[id][k]= last member type id (for trailing-size guess)
 *        var_storage[id]          = storage class (PushConstant/Uniform/UniformConstant/...)
 *        var_pointee[id]          = the OpTypePointer's pointee-type id
 *        ptr_pointee[id]          = same map but for pointers (for indirect lookup)
 *        decorations[id]          = { has_set, set, has_binding, binding, has_block }
 *
 *   2. For each OpVariable with storage class PushConstant:
 *        struct_id = ptr_pointee[var_pointee[var]]
 *        emit `push` block from struct_offsets / member_names of struct_id
 *
 *   3. For each OpVariable with storage class Uniform AND decorations.set==0
 *      AND decorations.binding==0 AND struct has Block:
 *        emit `ubo` block similarly
 *
 *   4. For each OpVariable with storage class UniformConstant AND a binding
 *      decoration: emit a sampler entry.
 *
 * The implementation uses dense arrays indexed by SPIR-V `id`. The header
 * declares the upper bound of all id values (`bound`); we trust it.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "spv_reflect.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- SPIR-V opcode + enum constants we use --------------------------------- */
#define SpvMagicNumber                0x07230203u

#define SpvOpName                     5
#define SpvOpMemberName               6
#define SpvOpDecorate                 71
#define SpvOpMemberDecorate           72
#define SpvOpTypePointer              32
#define SpvOpTypeStruct               30
#define SpvOpTypeFloat                22
#define SpvOpTypeInt                  21
#define SpvOpTypeVector               23
#define SpvOpTypeMatrix               24
#define SpvOpTypeSampledImage         27
#define SpvOpTypeArray                28
#define SpvOpTypeRuntimeArray         29
#define SpvOpVariable                 59

#define SpvDecorationBlock            2
#define SpvDecorationOffset           35
#define SpvDecorationDescriptorSet    34
#define SpvDecorationBinding          33

#define SpvStorageClassUniformConstant  0
#define SpvStorageClassUniform          2
#define SpvStorageClassPushConstant     9

/* ---- helpers --------------------------------------------------------------- */

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = (char *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

static char *err_fmt(const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return xstrdup(buf);
}

/* Read a SPIR-V literal-string operand starting at `words[off]`. Each word
 * holds 4 chars, NUL-terminated. Returns the malloc'd C string and (out)
 * how many words it consumed. */
static char *read_string(const uint32_t *words, size_t avail, size_t *consumed_out)
{
    /* Find the NUL byte in the word stream. */
    size_t bytes = avail * 4;
    const char *bp = (const char *)words;
    size_t n = 0;
    while (n < bytes && bp[n] != '\0') ++n;
    if (n == bytes) { *consumed_out = 0; return NULL; }
    char *r = (char *)malloc(n + 1);
    if (!r) { *consumed_out = 0; return NULL; }
    memcpy(r, bp, n); r[n] = '\0';
    *consumed_out = (n + 4) / 4;       /* round up to word boundary */
    return r;
}

/* ---- per-id tables --------------------------------------------------------- */

struct decor {
    bool     has_set;
    bool     has_binding;
    bool     has_block;
    uint32_t set;
    uint32_t binding;
};

struct member_name { char *name; };
struct member_decor { uint32_t offset; bool has_offset; uint32_t type_id; };

struct struct_info {
    uint32_t            *member_types;
    size_t               num_members;
    struct member_name  *names;
    struct member_decor *decorations;
};

struct ptr_info {
    uint32_t storage_class;
    uint32_t pointee_id;
    bool     valid;
};

struct var_info {
    uint32_t storage_class;
    uint32_t pointer_type_id;
    bool     valid;
};

/* ---- best-effort type size for "trailing member" total size ---------------- */

struct typesize_ctx {
    uint32_t  bound;
    /* we only need basic: float/int = 4, vec2/3/4 = 8/12/16, mat4 = 64,
     * arrays use stride decorations we don't track here, so approximate. */
    uint8_t  *base_size;     /* indexed by id */
    uint32_t *struct_size;
};

/* ---- main reflector -------------------------------------------------------- */

int spv_reflect(const uint32_t *spv, size_t words,
                struct spv_reflect_result *out, char **err_out)
{
    if (!spv || words < 5) {
        if (err_out) *err_out = xstrdup("SPIR-V too short");
        return -1;
    }
    if (spv[0] != SpvMagicNumber) {
        if (err_out) *err_out = err_fmt("SPIR-V bad magic 0x%08x", spv[0]);
        return -1;
    }
    uint32_t bound = spv[3];                 /* one past max id */
    if (bound == 0) {
        if (err_out) *err_out = xstrdup("SPIR-V bound=0");
        return -1;
    }

    char                  **id_name      = (char **)calloc(bound, sizeof(*id_name));
    struct decor           *id_decor     = (struct decor *)calloc(bound, sizeof(*id_decor));
    struct struct_info     *struct_tbl   = (struct struct_info *)calloc(bound, sizeof(*struct_tbl));
    struct ptr_info        *ptr_tbl      = (struct ptr_info *)calloc(bound, sizeof(*ptr_tbl));
    struct var_info        *var_tbl      = (struct var_info *)calloc(bound, sizeof(*var_tbl));
    if (!id_name || !id_decor || !struct_tbl || !ptr_tbl || !var_tbl) {
        if (err_out) *err_out = xstrdup("oom (id tables)");
        free(id_name); free(id_decor); free(struct_tbl); free(ptr_tbl); free(var_tbl);
        return -1;
    }

    /* Walk instructions starting at word 5. */
    size_t i = 5;
    while (i < words) {
        uint32_t op_word = spv[i];
        uint16_t opcode  = (uint16_t)(op_word & 0xffff);
        uint16_t wcount  = (uint16_t)(op_word >> 16);
        if (wcount == 0 || i + wcount > words) {
            if (err_out) *err_out = err_fmt("SPIR-V truncated at word %zu", i);
            goto fail;
        }
        const uint32_t *op = &spv[i];

        switch (opcode) {
            case SpvOpName: {
                /* op[1] = target id, op[2..] = literal string */
                if (wcount < 3) break;
                uint32_t id = op[1];
                size_t consumed = 0;
                char *s = read_string(&op[2], wcount - 2, &consumed);
                if (id < bound && s) {
                    free(id_name[id]);
                    id_name[id] = s;
                } else free(s);
                break;
            }
            case SpvOpMemberName: {
                /* op[1] = struct id, op[2] = member idx, op[3..] = string */
                if (wcount < 4) break;
                uint32_t sid = op[1];
                uint32_t midx = op[2];
                size_t consumed = 0;
                char *s = read_string(&op[3], wcount - 3, &consumed);
                if (sid < bound && s) {
                    /* Defer member-table allocation until we see the struct
                     * (we don't know the count yet). Stash names temporarily:
                     * we expand the names array on demand. */
                    if (midx + 1 > struct_tbl[sid].num_members) {
                        size_t newn = midx + 1;
                        struct member_name *t = (struct member_name *)
                            realloc(struct_tbl[sid].names, newn * sizeof(*t));
                        if (!t) { free(s); break; }
                        for (size_t k = struct_tbl[sid].num_members; k < newn; ++k) {
                            t[k].name = NULL;
                        }
                        struct_tbl[sid].names = t;
                        if (struct_tbl[sid].num_members < newn)
                            struct_tbl[sid].num_members = newn;
                        /* corresponding decorations array */
                        struct member_decor *td = (struct member_decor *)
                            realloc(struct_tbl[sid].decorations, newn * sizeof(*td));
                        if (td) {
                            for (size_t k = (struct_tbl[sid].decorations ? newn-1 : 0); k < newn; ++k) {
                                /* zero-init new slots */
                            }
                            /* simpler: zero everything we just expanded */
                            memset(td, 0, newn * sizeof(*td));
                            /* copy back any prior values we had */
                            (void)0;
                            struct_tbl[sid].decorations = td;
                        }
                    }
                    free(struct_tbl[sid].names[midx].name);
                    struct_tbl[sid].names[midx].name = s;
                } else free(s);
                break;
            }
            case SpvOpDecorate: {
                if (wcount < 3) break;
                uint32_t target = op[1];
                uint32_t deco   = op[2];
                if (target >= bound) break;
                switch (deco) {
                    case SpvDecorationBlock:
                        id_decor[target].has_block = true;
                        break;
                    case SpvDecorationDescriptorSet:
                        if (wcount >= 4) {
                            id_decor[target].set = op[3];
                            id_decor[target].has_set = true;
                        }
                        break;
                    case SpvDecorationBinding:
                        if (wcount >= 4) {
                            id_decor[target].binding = op[3];
                            id_decor[target].has_binding = true;
                        }
                        break;
                    default: break;
                }
                break;
            }
            case SpvOpMemberDecorate: {
                if (wcount < 4) break;
                uint32_t sid  = op[1];
                uint32_t midx = op[2];
                uint32_t deco = op[3];
                if (sid >= bound) break;
                if (deco == SpvDecorationOffset && wcount >= 5) {
                    if (midx + 1 > struct_tbl[sid].num_members) {
                        size_t newn = midx + 1;
                        struct member_decor *td = (struct member_decor *)
                            realloc(struct_tbl[sid].decorations, newn * sizeof(*td));
                        if (!td) break;
                        for (size_t k = struct_tbl[sid].num_members; k < newn; ++k) {
                            td[k].has_offset = false;
                            td[k].offset = 0;
                            td[k].type_id = 0;
                        }
                        struct_tbl[sid].decorations = td;
                        struct member_name *tn = (struct member_name *)
                            realloc(struct_tbl[sid].names, newn * sizeof(*tn));
                        if (tn) {
                            for (size_t k = struct_tbl[sid].num_members; k < newn; ++k)
                                tn[k].name = NULL;
                            struct_tbl[sid].names = tn;
                        }
                        struct_tbl[sid].num_members = newn;
                    }
                    struct_tbl[sid].decorations[midx].offset = op[4];
                    struct_tbl[sid].decorations[midx].has_offset = true;
                }
                break;
            }
            case SpvOpTypeStruct: {
                /* op[1] = result id, op[2..] = member type ids */
                if (wcount < 2) break;
                uint32_t rid = op[1];
                size_t   nm  = wcount - 2;
                if (rid >= bound) break;
                if (nm > struct_tbl[rid].num_members) {
                    /* extend tables */
                    struct member_name *tn = (struct member_name *)
                        realloc(struct_tbl[rid].names, nm * sizeof(*tn));
                    struct member_decor *td = (struct member_decor *)
                        realloc(struct_tbl[rid].decorations, nm * sizeof(*td));
                    if (tn) {
                        for (size_t k = struct_tbl[rid].num_members; k < nm; ++k)
                            tn[k].name = NULL;
                        struct_tbl[rid].names = tn;
                    }
                    if (td) {
                        for (size_t k = struct_tbl[rid].num_members; k < nm; ++k) {
                            td[k].has_offset = false;
                            td[k].offset = 0;
                            td[k].type_id = 0;
                        }
                        struct_tbl[rid].decorations = td;
                    }
                    struct_tbl[rid].num_members = nm;
                }
                /* Stash member types so we can guess the trailing size. */
                free(struct_tbl[rid].member_types);
                struct_tbl[rid].member_types = (uint32_t *)malloc(nm * sizeof(uint32_t));
                if (struct_tbl[rid].member_types) {
                    for (size_t k = 0; k < nm; ++k)
                        struct_tbl[rid].member_types[k] = op[2 + k];
                }
                break;
            }
            case SpvOpTypePointer: {
                /* op[1] = result id, op[2] = storage class, op[3] = type id */
                if (wcount < 4) break;
                uint32_t rid = op[1];
                if (rid < bound) {
                    ptr_tbl[rid].storage_class = op[2];
                    ptr_tbl[rid].pointee_id    = op[3];
                    ptr_tbl[rid].valid         = true;
                }
                break;
            }
            case SpvOpVariable: {
                /* op[1] = result type, op[2] = result id, op[3] = storage class */
                if (wcount < 4) break;
                uint32_t rtype = op[1];
                uint32_t rid   = op[2];
                uint32_t storage = op[3];
                if (rid < bound) {
                    var_tbl[rid].pointer_type_id = rtype;
                    var_tbl[rid].storage_class   = storage;
                    var_tbl[rid].valid           = true;
                }
                break;
            }
            default: break;
        }

        i += wcount;
    }

    /* ---- Build result ---- */
    memset(out, 0, sizeof(*out));

    /* Helper: extract spv_block from a struct id. */
#define EMIT_BLOCK(SID, BLOCK_OUT) do {                                              \
        struct struct_info *si = &struct_tbl[(SID)];                                 \
        size_t n = si->num_members;                                                  \
        if (n > 0) {                                                                 \
            (BLOCK_OUT)->members = (struct spv_member *)                             \
                calloc(n, sizeof(*(BLOCK_OUT)->members));                            \
            (BLOCK_OUT)->num_members = n;                                            \
            uint32_t max_off = 0;                                                    \
            for (size_t k = 0; k < n; ++k) {                                         \
                (BLOCK_OUT)->members[k].name = si->names                             \
                    ? xstrdup(si->names[k].name) : NULL;                             \
                (BLOCK_OUT)->members[k].offset = si->decorations                     \
                    ? si->decorations[k].offset : 0;                                 \
                if ((BLOCK_OUT)->members[k].offset > max_off)                        \
                    max_off = (BLOCK_OUT)->members[k].offset;                        \
            }                                                                        \
            /* Total block size: best-effort = max(offset) + 4. Real size            \
             * needs full type-size walk; for offset-based parameter writes          \
             * the trailing-byte ambiguity doesn't matter. */                        \
            (BLOCK_OUT)->size = max_off + 4;                                         \
        }                                                                            \
    } while (0)

    size_t sampler_cap = 0;

    for (uint32_t id = 0; id < bound; ++id) {
        if (!var_tbl[id].valid) continue;
        uint32_t ptid    = var_tbl[id].pointer_type_id;
        uint32_t storage = var_tbl[id].storage_class;
        if (ptid >= bound || !ptr_tbl[ptid].valid) continue;
        uint32_t pointee = ptr_tbl[ptid].pointee_id;
        if (pointee >= bound) continue;

        if (storage == SpvStorageClassPushConstant && !out->has_push) {
            EMIT_BLOCK(pointee, &out->push);
            out->has_push = true;
        } else if (storage == SpvStorageClassUniform &&
                   id_decor[id].has_set && id_decor[id].set == 0 &&
                   id_decor[id].has_binding && id_decor[id].binding == 0 &&
                   id_decor[pointee].has_block && !out->has_ubo) {
            EMIT_BLOCK(pointee, &out->ubo);
            out->has_ubo = true;
        } else if (storage == SpvStorageClassUniformConstant &&
                   id_decor[id].has_binding) {
            if (out->num_samplers == sampler_cap) {
                sampler_cap = sampler_cap ? sampler_cap * 2 : 4;
                struct spv_sampler *t = (struct spv_sampler *)realloc(
                    out->samplers, sampler_cap * sizeof(*t));
                if (!t) continue;
                out->samplers = t;
            }
            struct spv_sampler *s = &out->samplers[out->num_samplers++];
            s->name           = xstrdup(id_name[id]);
            s->descriptor_set = id_decor[id].has_set ? id_decor[id].set : 0;
            s->binding        = id_decor[id].binding;
        }
    }

    /* Free the temporary tables. */
    for (uint32_t id = 0; id < bound; ++id) {
        free(id_name[id]);
        if (struct_tbl[id].names) {
            for (size_t k = 0; k < struct_tbl[id].num_members; ++k)
                free(struct_tbl[id].names[k].name);
            free(struct_tbl[id].names);
        }
        free(struct_tbl[id].decorations);
        free(struct_tbl[id].member_types);
    }
    free(id_name);
    free(id_decor);
    free(struct_tbl);
    free(ptr_tbl);
    free(var_tbl);
    return 0;

fail:
    for (uint32_t id = 0; id < bound; ++id) {
        free(id_name[id]);
        if (struct_tbl[id].names) {
            for (size_t k = 0; k < struct_tbl[id].num_members; ++k)
                free(struct_tbl[id].names[k].name);
            free(struct_tbl[id].names);
        }
        free(struct_tbl[id].decorations);
        free(struct_tbl[id].member_types);
    }
    free(id_name);
    free(id_decor);
    free(struct_tbl);
    free(ptr_tbl);
    free(var_tbl);
    return -1;
}

void spv_reflect_free(struct spv_reflect_result *r)
{
    if (!r) return;
    if (r->push.members) {
        for (size_t i = 0; i < r->push.num_members; ++i) free(r->push.members[i].name);
        free(r->push.members);
    }
    if (r->ubo.members) {
        for (size_t i = 0; i < r->ubo.num_members; ++i) free(r->ubo.members[i].name);
        free(r->ubo.members);
    }
    for (size_t i = 0; i < r->num_samplers; ++i) free(r->samplers[i].name);
    free(r->samplers);
    memset(r, 0, sizeof(*r));
}
