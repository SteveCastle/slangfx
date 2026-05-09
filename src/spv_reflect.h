/*
 * vf-slang — minimal SPIR-V reflector.
 *
 * Walks a SPIR-V binary to extract just the slang shader contract surface:
 *   - the push-constant block: total size + every member's name + offset
 *   - the UBO at descriptor set 0 binding 0: same
 *   - every sampler declared at descriptor set 0 (binding + name)
 *
 * Sufficient for Phase 7 (parameter offsets) and Phase 5b/c/d/Phase 6
 * (sampler binding discovery → alias resolution + PassFeedback detection).
 *
 * Not a general-purpose reflector. Only opcodes the slang shader contract
 * actually uses are recognized; everything else is skipped.
 *
 * No external dependencies; SPIR-V opcode/decoration constants are inlined.
 */

#ifndef VF_SLANG_SPV_REFLECT_H
#define VF_SLANG_SPV_REFLECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct spv_member {
    char    *name;       /* malloc'd; freed on spv_reflect_free */
    uint32_t offset;     /* byte offset within the block */
};

struct spv_block {
    uint32_t          size;        /* total block size in bytes (best-effort) */
    struct spv_member *members;
    size_t             num_members;
};

struct spv_sampler {
    char    *name;
    uint32_t descriptor_set;
    uint32_t binding;
};

struct spv_reflect_result {
    struct spv_block      push;
    struct spv_block      ubo;
    bool                  has_push;
    bool                  has_ubo;
    struct spv_sampler   *samplers;
    size_t                num_samplers;
};

/* Reflect a SPIR-V binary. words is the number of uint32_t words, not bytes.
 * On error returns -1 and *err_out (if non-NULL) gets a malloc'd message
 * (caller frees). On success the result is owned by the caller and must be
 * freed via spv_reflect_free. */
int spv_reflect(const uint32_t *spv, size_t words,
                struct spv_reflect_result *out, char **err_out);

void spv_reflect_free(struct spv_reflect_result *r);

#endif /* VF_SLANG_SPV_REFLECT_H */
