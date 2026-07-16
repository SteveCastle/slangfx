/*
 * slangfx — Vulkan offscreen render pipeline.
 *
 * Phase 1: VkInstance/VkDevice + offscreen render-to-texture (single pass).
 * Phase 4: pipeline driven by a real slang shader from a .slangp preset.
 * Phase 5: multi-pass. Each .slangp pass becomes a `pass_state` holding its
 *          own framebuffer image, render pass, graphics pipeline, and
 *          descriptor set. Per frame, passes are dispatched in order; each
 *          pass's `Source` sampler reads the previous pass's output (or
 *          the original input for pass 0).
 *
 * Slang binding contract (per pass):
 *   set=0, binding=0   UBO (mat4 MVP, currently identity)
 *   set=0, binding=2   sampler2D Source (previous pass output / input)
 *   push_constant      slang_push { vec4 SourceSize, OriginalSize, OutputSize;
 *                                    uint FrameCount; ...params... }
 *   vertex             location 0 = vec4 Position, location 1 = vec2 TexCoord
 *
 * Phase 5 deferred (incrementally):
 *   - aliases (`<alias>` sampler references)             — Phase 5b
 *   - `Pass<n>` numeric pass refs                         — Phase 5c
 *   - `Original` / `OriginalSize`                         — Phase 5d
 *
 * Phase 6 adds PassFeedback ring buffers; Phase 7 reflection so push
 * constants honor shader-declared parameter offsets; Phase 8 external
 * textures from `textures = "..."`; Phase 9 YUV input/output.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "slang_pipeline.h"
#include "slang_compile.h"
#include "slang_metrics.h"   /* slang_now_ms() for the Time wall-clock fallback */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>
#include <shaderc/shaderc.h>

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

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
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return xstrdup(buf);
}

#define VK_CHECK(expr, label) do {                                          \
    VkResult _r = (expr);                                                   \
    if (_r != VK_SUCCESS) {                                                 \
        if (err_out) *err_out = err_fmt("%s failed (VkResult=%d)", label, _r); \
        goto fail;                                                          \
    }                                                                       \
} while (0)

static const char *BUILTIN_SLANG =
    "#version 450\n"
    "layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;\n"
    "layout(push_constant) uniform Push {\n"
    "    vec4 SourceSize; vec4 OriginalSize; vec4 OutputSize; uint FrameCount;\n"
    "} params;\n"
    "#pragma stage vertex\n"
    "layout(location=0) in vec4 Position; layout(location=1) in vec2 TexCoord;\n"
    "layout(location=0) out vec2 vUV;\n"
    "void main() { gl_Position = global.MVP * Position; vUV = TexCoord; }\n"
    "#pragma stage fragment\n"
    "layout(location=0) in vec2 vUV; layout(location=0) out vec4 FragColor;\n"
    "layout(set=0, binding=2) uniform sampler2D Source;\n"
    "void main() { FragColor = texture(Source, vUV); }\n";

struct slang_push {
    float    source_size  [4];
    float    original_size[4];
    float    output_size  [4];
    uint32_t frame_count;
    uint32_t _pad0;
    float    reserved     [16];
};

struct slang_ubo { float MVP[16]; };

struct vertex { float pos[4]; float uv[2]; };
static const struct vertex QUAD_VERTS[4] = {
    { { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
    { {  1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
    { {  1.0f,  1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
    { { -1.0f,  1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
};
static const uint16_t QUAD_INDICES[6] = { 0, 1, 2,  0, 2, 3 };

/* -------------------------------------------------------------------------- */
/* Per-pass state                                                             */
/* -------------------------------------------------------------------------- */

struct pass_state {
    struct slang_module *mod;
    uint32_t out_w, out_h;
    VkFormat format;        /* per-pass output / framebuffer format */

    /* The pass's framebuffer image (= this pass's output, = next pass's
     * Source). Always created with COLOR_ATTACHMENT + SAMPLED + TRANSFER_SRC
     * so it can serve all three roles (write target, sampled by next pass,
     * copied to readback for the final pass).
     *
     * When a downstream pass declares `mipmap_input<n> = true`, the producer
     * needs a full mip chain. `mip_levels` is then > 1, `out_view` covers
     * the full chain (for downstream sampling), and `fbo_view` is a
     * single-mip-0 view used as the framebuffer attachment (Vulkan rejects
     * multi-level views as render targets). When mip_levels == 1 the two
     * aliases the same handle. */
    VkImage        out_img;
    VkDeviceMemory out_mem;
    VkImageView    out_view;     /* sampled view: covers all mip levels */
    VkImageView    fbo_view;     /* render-target view: mip 0 only */
    uint32_t       mip_levels;
    VkSampler      sampler;

    /* Phase 6: PassFeedback. If any later pass references this pass's output
     * as `PassFeedback<i>` or `<alias>Feedback`, we keep a snapshot of the
     * previous frame's output here. After each frame's pipeline run, the
     * current out_img is blitted into feedback_img so next frame consumers
     * can sample "previous-frame pass<i>". */
    bool           is_feedback_producer;
    VkImage        feedback_img;
    VkDeviceMemory feedback_mem;
    VkImageView    feedback_view;

    VkRenderPass   render_pass;
    VkFramebuffer  framebuffer;

    /* Per-pass UBO. Many slang shaders put SourceSize / OriginalSize /
     * OutputSize and even runtime parameters in the std140 UBO instead of
     * the push-constant block. We allocate one host-visible UBO buffer per
     * pass and re-populate it each frame at the offsets reflection found. */
    VkBuffer       ubo_buf;
    VkDeviceMemory ubo_mem;
    void          *ubo_ptr;
    size_t         ubo_size;       /* allocated bytes */

    VkDescriptorSetLayout dset_layout;
    VkPipelineLayout      pipe_layout;
    VkPipeline            pipeline;
    VkDescriptorSet       dset;
};

/* -------------------------------------------------------------------------- */
/* Pipeline state                                                             */
/* -------------------------------------------------------------------------- */

struct slang_pipeline {
    /* Core */
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         dev;
    uint32_t         queue_family;
    VkQueue          queue;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkCommandPool   cmd_pool;
    VkCommandBuffer cmd;
    VkFence         fence;

    /* I/O */
    unsigned input_w, input_h;
    unsigned output_w, output_h;

    VkImage        in_img;
    VkDeviceMemory in_mem;
    VkImageView    in_view;
    VkSampler      in_sampler;       /* sampler used for the original input */

    VkBuffer       stg_upload;
    VkDeviceMemory stg_upload_mem;
    void          *stg_upload_ptr;
    VkBuffer       stg_readback;
    VkDeviceMemory stg_readback_mem;
    void          *stg_readback_ptr;
    bool           stg_readback_coherent;  /* false => invalidate before read */

    /* Shared resources */
    VkBuffer       vbuf;
    VkDeviceMemory vbuf_mem;
    VkBuffer       ibuf;
    VkDeviceMemory ibuf_mem;
    VkBuffer       ubo;
    VkDeviceMemory ubo_mem;
    void          *ubo_ptr;
    VkDescriptorPool dpool;

    /* Multi-pass chain */
    struct pass_state *passes;
    size_t             num_passes;

    /* Phase 8: external textures from `textures = "..."`. Loaded once at
     * setup, bound into descriptor sets of any pass whose shader references
     * the texture by name. */
    struct ext_texture {
        char          *name;
        VkImage        img;
        VkDeviceMemory mem;
        VkImageView    view;
        VkSampler      sampler;
    } *ext_textures;
    size_t num_ext_textures;

    /* Alias resolution map (Phase 5b). For every preset pass that declared
     * an `alias<i> = name`, we record (name, pass_idx) so downstream passes
     * can bind it as a sampler by name. */
    struct alias_entry {
        char  *name;
        size_t pass_idx;
    } *aliases;
    size_t num_aliases;

    uint32_t frame_count;

    /* Wall-clock baseline for the `Time` standard field, used when the caller
     * does not supply a frame timestamp (realtime sinks pass real PTS). */
    double   clock_start_ms;
    bool     clock_started;

    /* False when this pipeline borrows another's instance/device (created
     * via slang_pipeline_create_shared): teardown then skips destroying the
     * shared Vulkan core objects. */
    bool     owns_device;
};

/* -------------------------------------------------------------------------- */
/* Vulkan helpers                                                             */
/* -------------------------------------------------------------------------- */

static uint32_t find_memtype(const VkPhysicalDeviceMemoryProperties *mp,
                             uint32_t req, VkMemoryPropertyFlags props)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; ++i)
        if ((req & (1u << i)) &&
            (mp->memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

static int init_instance_and_device(struct slang_pipeline *p, char **err_out)
{
    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "slangfx", .applicationVersion = 1,
        .pEngineName = "slangfx", .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai,
    };
    VK_CHECK(vkCreateInstance(&ici, NULL, &p->instance), "vkCreateInstance");

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(p->instance, &n, NULL);
    if (n == 0) {
        if (err_out) *err_out = xstrdup("no Vulkan physical devices");
        goto fail;
    }
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    vkEnumeratePhysicalDevices(p->instance, &n, devs);
    p->phys = devs[0];
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(devs[i], &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            p->phys = devs[i]; break;
        }
    }

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p->phys, &qn, NULL);
    VkQueueFamilyProperties qp[16]; if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(p->phys, &qn, qp);
    p->queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qn; ++i)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            p->queue_family = i; break;
        }
    if (p->queue_family == UINT32_MAX) {
        if (err_out) *err_out = xstrdup("no graphics queue family");
        goto fail;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = p->queue_family,
        .queueCount = 1, .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
    };
    VK_CHECK(vkCreateDevice(p->phys, &dci, NULL, &p->dev), "vkCreateDevice");
    vkGetDeviceQueue(p->dev, p->queue_family, 0, &p->queue);
    vkGetPhysicalDeviceMemoryProperties(p->phys, &p->mem_props);
    return 0;
fail: return -1;
}

static int create_image(struct slang_pipeline *p, uint32_t w, uint32_t h,
                        VkFormat fmt, VkImageUsageFlags usage,
                        uint32_t mip_levels,
                        VkImage *img_out, VkDeviceMemory *mem_out,
                        char **err_out)
{
    if (mip_levels == 0) mip_levels = 1;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { w, h, 1 }, .mipLevels = mip_levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(p->dev, &ici, NULL, img_out), "vkCreateImage");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(p->dev, *img_out, &mr);
    uint32_t mt = find_memtype(&p->mem_props, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { if (err_out) *err_out = xstrdup("no DEVICE_LOCAL memory"); goto fail; }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = mt,
    };
    VK_CHECK(vkAllocateMemory(p->dev, &mai, NULL, mem_out), "vkAllocateMemory(image)");
    VK_CHECK(vkBindImageMemory(p->dev, *img_out, *mem_out, 0), "vkBindImageMemory");
    return 0;
fail: return -1;
}

static int create_buffer(struct slang_pipeline *p, VkDeviceSize size,
                         VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                         VkBuffer *buf_out, VkDeviceMemory *mem_out,
                         void **mapped_out, char **err_out)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(p->dev, &bci, NULL, buf_out), "vkCreateBuffer");
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(p->dev, *buf_out, &mr);
    uint32_t mt = find_memtype(&p->mem_props, mr.memoryTypeBits, props);
    if (mt == UINT32_MAX) { if (err_out) *err_out = xstrdup("no compatible buffer memory"); goto fail; }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = mt,
    };
    VK_CHECK(vkAllocateMemory(p->dev, &mai, NULL, mem_out), "vkAllocateMemory(buf)");
    VK_CHECK(vkBindBufferMemory(p->dev, *buf_out, *mem_out, 0), "vkBindBufferMemory");
    if (mapped_out)
        VK_CHECK(vkMapMemory(p->dev, *mem_out, 0, size, 0, mapped_out), "vkMapMemory");
    return 0;
fail: return -1;
}

/* Readback staging buffer. Prefers HOST_CACHED memory: the readback path
 * memcpy's *out* of this buffer every frame, and CPU reads from the usual
 * HOST_COHERENT (write-combined) memory are pathologically slow — at 720p
 * that single uncached read dominated frame time (~175 MiB/s ceiling). Cached
 * host memory makes the read fast; we then invalidate it before each read so
 * the CPU sees the GPU's writes (a no-op when the type also happens to be
 * coherent). Falls back to COHERENT when no cached type is available. */
static int create_readback_buffer(struct slang_pipeline *p, VkDeviceSize size,
                                  char **err_out)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(p->dev, &bci, NULL, &p->stg_readback), "vkCreateBuffer(readback)");
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(p->dev, p->stg_readback, &mr);

    uint32_t mt = find_memtype(&p->mem_props, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mt == UINT32_MAX)
        mt = find_memtype(&p->mem_props, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        if (err_out) *err_out = xstrdup("no host-visible readback memory");
        goto fail;
    }
    p->stg_readback_coherent =
        (p->mem_props.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = mt,
    };
    VK_CHECK(vkAllocateMemory(p->dev, &mai, NULL, &p->stg_readback_mem), "vkAllocateMemory(readback)");
    VK_CHECK(vkBindBufferMemory(p->dev, p->stg_readback, p->stg_readback_mem, 0), "vkBindBufferMemory(readback)");
    VK_CHECK(vkMapMemory(p->dev, p->stg_readback_mem, 0, size, 0, &p->stg_readback_ptr), "vkMapMemory(readback)");
    return 0;
fail: return -1;
}

static VkImageView create_view_mip(VkDevice d, VkImage img, VkFormat fmt,
                                   uint32_t base_mip, uint32_t mip_count)
{
    if (mip_count == 0) mip_count = 1;
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, base_mip, mip_count, 0, 1 },
    };
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(d, &ivci, NULL, &v);
    return v;
}

static VkImageView create_view(VkDevice d, VkImage img, VkFormat fmt)
{
    return create_view_mip(d, img, fmt, 0, 1);
}

static VkShaderModule create_shader(VkDevice d, const uint32_t *spv, size_t words)
{
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = words * 4, .pCode = spv,
    };
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(d, &smci, NULL, &m);
    return m;
}

static VkSamplerAddressMode wrap_to_vk(enum slangp_wrap_mode w)
{
    switch (w) {
        case SLANGP_WRAP_CLAMP_TO_EDGE:   return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case SLANGP_WRAP_REPEAT:          return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case SLANGP_WRAP_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case SLANGP_WRAP_CLAMP_TO_BORDER:
        default:                          return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
}

/* Resolve a pass's framebuffer format from its slangp directives.
 * Precedence: explicit fbo_format > srgb_framebuffer / float_framebuffer
 * flags > default (R8G8B8A8_UNORM). Float framebuffers are critical for
 * shaders like ntsc-pass1 that produce signals with negative AC components
 * — without them the modulated signal clamps at 0 and downstream
 * demodulation produces black. */
static VkFormat resolve_pass_format(const struct slangp_pass *pp)
{
    if (!pp) return VK_FORMAT_R8G8B8A8_UNORM;
    if (pp->fbo_format != SLANGP_FORMAT_DEFAULT) {
        switch (pp->fbo_format) {
            case SLANGP_FORMAT_R8_UNORM:            return VK_FORMAT_R8_UNORM;
            case SLANGP_FORMAT_R8G8_UNORM:          return VK_FORMAT_R8G8_UNORM;
            case SLANGP_FORMAT_R8G8B8A8_UNORM:      return VK_FORMAT_R8G8B8A8_UNORM;
            case SLANGP_FORMAT_R8G8B8A8_SRGB:       return VK_FORMAT_R8G8B8A8_SRGB;
            case SLANGP_FORMAT_R10G10B10A2_UNORM:   return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case SLANGP_FORMAT_R16_UNORM:           return VK_FORMAT_R16_UNORM;
            case SLANGP_FORMAT_R16_SFLOAT:          return VK_FORMAT_R16_SFLOAT;
            case SLANGP_FORMAT_R16G16B16A16_UNORM:  return VK_FORMAT_R16G16B16A16_UNORM;
            case SLANGP_FORMAT_R16G16B16A16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case SLANGP_FORMAT_R32_SFLOAT:          return VK_FORMAT_R32_SFLOAT;
            case SLANGP_FORMAT_R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
            default: break;
        }
    }
    if (pp->float_framebuffer) return VK_FORMAT_R16G16B16A16_SFLOAT;
    if (pp->srgb_framebuffer)  return VK_FORMAT_R8G8B8A8_SRGB;
    return VK_FORMAT_R8G8B8A8_UNORM;
}

/* -------------------------------------------------------------------------- */
/* Sampler resolution (Phase 5b/c/d + Phase 6 + Phase 8)                       */
/* -------------------------------------------------------------------------- */

/* Match `name` against alias table. Returns pass index or -1. */
static int find_alias(struct slang_pipeline *p, const char *name)
{
    if (!name) return -1;
    for (size_t i = 0; i < p->num_aliases; ++i)
        if (p->aliases[i].name && strcmp(p->aliases[i].name, name) == 0)
            return (int)p->aliases[i].pass_idx;
    return -1;
}

static int find_external_texture(struct slang_pipeline *p, const char *name)
{
    if (!name) return -1;
    for (size_t i = 0; i < p->num_ext_textures; ++i)
        if (p->ext_textures[i].name && strcmp(p->ext_textures[i].name, name) == 0)
            return (int)i;
    return -1;
}

/* True if `s` starts with `prefix`. */
static bool startswith(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

/* True if `s` ends with `suffix`. */
static bool endswith(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return m <= n && strcmp(s + n - m, suffix) == 0;
}

struct resolved_binding {
    VkImageView view;
    VkSampler   sampler;
    bool        is_feedback;
    int         feedback_pass;   /* producer pass index */
};

/* Map a `<TexName>Size` push/UBO field name to (w, h) of the texture
 * `TexName` refers to. Handles aliases, `Pass<n>`, `PassFeedback<n>`,
 * `<alias>Feedback`, and `OriginalHistory<n>`. Standard fields
 * (SourceSize, OriginalSize, OutputSize, FinalViewportSize) are handled
 * by the explicit per-frame writes; this helper returns false for those.
 * Returns true and fills *w/h on match. */
static bool lookup_size_field(struct slang_pipeline *p,
                              const char *field_name,
                              uint32_t *w_out, uint32_t *h_out)
{
    if (!field_name) return false;
    size_t n = strlen(field_name);
    if (n <= 4 || strcmp(field_name + n - 4, "Size") != 0) return false;

    char base[256];
    size_t bn = n - 4;
    if (bn >= sizeof(base)) return false;
    memcpy(base, field_name, bn); base[bn] = '\0';

    /* Standard fields: handled by the caller's explicit WRITE_FIELDs. */
    if (!strcmp(base, "Source") || !strcmp(base, "Original") ||
        !strcmp(base, "Output") || !strcmp(base, "FinalViewport"))
        return false;

    /* OriginalHistory<n>: every depth shares the original input dims for
     * now (Phase 9 will track per-depth dims if non-square scaling lands). */
    if (startswith(base, "OriginalHistory")) {
        *w_out = p->input_w; *h_out = p->input_h;
        return true;
    }
    /* PassFeedback<n>Size: same dims as that producer's output. */
    if (startswith(base, "PassFeedback")) {
        const char *digits = base + strlen("PassFeedback");
        if (*digits >= '0' && *digits <= '9') {
            int idx = atoi(digits);
            if (idx >= 0 && (size_t)idx < p->num_passes) {
                *w_out = p->passes[idx].out_w;
                *h_out = p->passes[idx].out_h;
                return true;
            }
        }
        return false;
    }
    /* Pass<n>Size: that pass's output dims. */
    if (startswith(base, "Pass")) {
        const char *digits = base + 4;
        if (*digits >= '0' && *digits <= '9') {
            int idx = atoi(digits);
            if (idx >= 0 && (size_t)idx < p->num_passes) {
                *w_out = p->passes[idx].out_w;
                *h_out = p->passes[idx].out_h;
                return true;
            }
            return false;
        }
        /* Falls through to alias lookup (e.g. an alias literally named "Pass*"). */
    }
    /* <alias>FeedbackSize. */
    if (bn > 8 && strcmp(base + bn - 8, "Feedback") == 0) {
        char alias[256];
        size_t an = bn - 8;
        if (an < sizeof(alias)) {
            memcpy(alias, base, an); alias[an] = '\0';
            for (size_t i = 0; i < p->num_aliases; ++i) {
                if (p->aliases[i].name && strcmp(p->aliases[i].name, alias) == 0) {
                    size_t pi = p->aliases[i].pass_idx;
                    *w_out = p->passes[pi].out_w;
                    *h_out = p->passes[pi].out_h;
                    return true;
                }
            }
        }
        return false;
    }
    /* <alias>Size. */
    for (size_t i = 0; i < p->num_aliases; ++i) {
        if (p->aliases[i].name && strcmp(p->aliases[i].name, base) == 0) {
            size_t pi = p->aliases[i].pass_idx;
            *w_out = p->passes[pi].out_w;
            *h_out = p->passes[pi].out_h;
            return true;
        }
    }
    return false;
}

/* Resolve a sampler name to a (view, sampler) pair, marking feedback if
 * applicable. `prev_view`/`prev_sampler` are the previous pass's output (or
 * the input image, for pass 0). Returns 0 on success, -1 if unresolved. */
static int resolve_sampler_name(struct slang_pipeline *p,
                                size_t current_pass_idx,
                                const char *name,
                                VkImageView prev_view, VkSampler prev_sampler,
                                struct resolved_binding *out)
{
    if (!name) return -1;
    out->view = VK_NULL_HANDLE;
    out->sampler = VK_NULL_HANDLE;
    out->is_feedback = false;
    out->feedback_pass = -1;

    /* Source = previous pass output (or input for pass 0). */
    if (strcmp(name, "Source") == 0) {
        out->view = prev_view; out->sampler = prev_sampler;
        return 0;
    }
    /* Original / OriginalHistory0 = the original input frame. Higher history
     * (OriginalHistory1+) is Phase 9 work; treat as Original for now. */
    if (strcmp(name, "Original") == 0 || startswith(name, "OriginalHistory")) {
        out->view = p->in_view; out->sampler = p->in_sampler;
        return 0;
    }
    /* PassFeedback<n> — feedback ring of pass n. */
    if (startswith(name, "PassFeedback")) {
        int n = atoi(name + strlen("PassFeedback"));
        if (n >= 0 && (size_t)n < p->num_passes) {
            p->passes[n].is_feedback_producer = true;
            out->is_feedback = true;
            out->feedback_pass = n;
            /* view set later, after feedback image is allocated */
            return 0;
        }
        return -1;
    }
    /* <alias>Feedback — feedback of an aliased pass. */
    if (endswith(name, "Feedback") && strlen(name) > 8) {
        char alias[256];
        size_t n = strlen(name) - 8;
        if (n >= sizeof(alias)) return -1;
        memcpy(alias, name, n); alias[n] = '\0';
        int pi = find_alias(p, alias);
        if (pi >= 0) {
            p->passes[pi].is_feedback_producer = true;
            out->is_feedback = true;
            out->feedback_pass = pi;
            return 0;
        }
        return -1;
    }
    /* Pass<n> (numeric, this frame). */
    if (startswith(name, "Pass")) {
        const char *digits = name + 4;
        if (*digits >= '0' && *digits <= '9') {
            int n = atoi(digits);
            if (n >= 0 && (size_t)n < p->num_passes) {
                /* Self-reference would deadlock; downstream passes only. */
                if ((size_t)n < current_pass_idx) {
                    out->view = p->passes[n].out_view;
                    out->sampler = p->passes[n].sampler;
                    return 0;
                }
            }
        }
    }
    /* alias name → that pass's output. */
    int pi = find_alias(p, name);
    if (pi >= 0 && (size_t)pi < current_pass_idx) {
        out->view = p->passes[pi].out_view;
        out->sampler = p->passes[pi].sampler;
        return 0;
    }
    /* external texture (Phase 8). */
    int ti = find_external_texture(p, name);
    if (ti >= 0) {
        out->view = p->ext_textures[ti].view;
        out->sampler = p->ext_textures[ti].sampler;
        return 0;
    }
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Per-pass setup                                                             */
/* -------------------------------------------------------------------------- */

/* Resolve a pass's output dimensions from its slangp scale rules.
 * `prev_w/h` is the previous pass's output (or the input frame for pass 0).
 * `final_w/h` is the final viewport (downstream output of slangfx).
 *
 * For per-axis rules we honor scale_type_x/scale_type_y separately. */
static void resolve_pass_dims(const struct slangp_pass *ps,
                              uint32_t prev_w, uint32_t prev_h,
                              uint32_t final_w, uint32_t final_h,
                              uint32_t *w_out, uint32_t *h_out)
{
    float fx = 0, fy = 0;
    switch (ps->scale_type_x) {
        case SLANGP_SCALE_VIEWPORT: fx = (float)final_w * ps->scale_x; break;
        case SLANGP_SCALE_ABSOLUTE: fx = ps->scale_x;                   break;
        case SLANGP_SCALE_SOURCE:
        default:                    fx = (float)prev_w  * ps->scale_x; break;
    }
    switch (ps->scale_type_y) {
        case SLANGP_SCALE_VIEWPORT: fy = (float)final_h * ps->scale_y; break;
        case SLANGP_SCALE_ABSOLUTE: fy = ps->scale_y;                   break;
        case SLANGP_SCALE_SOURCE:
        default:                    fy = (float)prev_h  * ps->scale_y; break;
    }
    if (fx < 1.0f) fx = 1.0f;
    if (fy < 1.0f) fy = 1.0f;
    *w_out = (uint32_t)(fx + 0.5f);
    *h_out = (uint32_t)(fy + 0.5f);
}

/* Compute mipLevels for a w×h image: 1 + floor(log2(max(w, h))). */
static uint32_t mip_levels_for(uint32_t w, uint32_t h)
{
    uint32_t m = w > h ? w : h;
    uint32_t lvls = 1;
    while (m > 1) { m >>= 1; ++lvls; }
    return lvls;
}

/* Phase A: allocate the pass's output framebuffer image + its sampler.
 * `mip_levels` > 1 means a downstream pass declared `mipmap_input = true`
 * for this pass's output; the image gets a full mip chain (generated each
 * frame after rendering via vkCmdBlitImage), the `out_view` covers the
 * whole chain (for sampling), and `fbo_view` is a single-mip-0 view used
 * as the framebuffer attachment. */
static int alloc_pass_image(struct slang_pipeline *p, struct pass_state *ps,
                            const struct slangp_pass *ppass,
                            uint32_t out_w, uint32_t out_h,
                            VkFormat format, uint32_t mip_levels,
                            char **err_out)
{
    if (mip_levels == 0) mip_levels = 1;
    ps->out_w = out_w;
    ps->out_h = out_h;
    ps->format = format;
    ps->mip_levels = mip_levels;
    if (create_image(p, out_w, out_h, format,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     mip_levels,
                     &ps->out_img, &ps->out_mem, err_out) != 0) goto fail;
    ps->out_view = create_view_mip(p->dev, ps->out_img, format, 0, mip_levels);
    if (!ps->out_view) { if (err_out) *err_out = xstrdup("vkCreateImageView (pass out)"); goto fail; }
    if (mip_levels > 1) {
        ps->fbo_view = create_view_mip(p->dev, ps->out_img, format, 0, 1);
        if (!ps->fbo_view) { if (err_out) *err_out = xstrdup("vkCreateImageView (fbo)"); goto fail; }
    } else {
        ps->fbo_view = ps->out_view;
    }

    /* maxLod = mip_levels lets shaders sample any LOD via textureLod. With
     * only 1 level the clamp is harmless (Vulkan clamps to image's own
     * maxLod). LINEAR mipmap mode interpolates between mips for shaders
     * that request fractional LOD. */
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (ppass && ppass->filter_linear) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (ppass && ppass->filter_linear) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = wrap_to_vk(ppass ? ppass->wrap_mode : SLANGP_WRAP_CLAMP_TO_EDGE),
        .addressModeV = wrap_to_vk(ppass ? ppass->wrap_mode : SLANGP_WRAP_CLAMP_TO_EDGE),
        .addressModeW = wrap_to_vk(ppass ? ppass->wrap_mode : SLANGP_WRAP_CLAMP_TO_EDGE),
        .minLod = 0.0f,
        .maxLod = (float)mip_levels,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    VK_CHECK(vkCreateSampler(p->dev, &sci, NULL, &ps->sampler), "vkCreateSampler (pass)");
    return 0;
fail: return -1;
}

/* Phase D: allocate the feedback snapshot image for a pass that has at
 * least one downstream PassFeedback consumer. The image starts in
 * UNDEFINED layout — first frame's `PassFeedback` reads will see whatever
 * the implementation returns (we explicitly clear to black before first
 * dispatch so the result is deterministic). */
static int alloc_feedback_image(struct slang_pipeline *p, struct pass_state *ps,
                                char **err_out)
{
    /* Match the producer's output format so vkCmdCopyImage at end-of-frame
     * works without format-mismatch validation errors. */
    VkFormat fmt = ps->format ? ps->format : VK_FORMAT_R8G8B8A8_UNORM;
    if (create_image(p, ps->out_w, ps->out_h, fmt,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     1,
                     &ps->feedback_img, &ps->feedback_mem, err_out) != 0) return -1;
    ps->feedback_view = create_view(p->dev, ps->feedback_img, fmt);
    if (!ps->feedback_view) { if (err_out) *err_out = xstrdup("vkCreateImageView (feedback)"); return -1; }
    return 0;
}

/* Phase F: build render pass + framebuffer + dynamic descriptor set layout
 * + pipeline + descriptor set, using the pre-resolved sampler bindings. */
static int build_pass_pipeline(struct slang_pipeline *p, struct pass_state *ps,
                               const struct resolved_binding *bindings,
                               size_t num_bindings, char **err_out)
{
    const VkFormat FMT = ps->format ? ps->format : VK_FORMAT_R8G8B8A8_UNORM;

    /* Render pass. */
    VkAttachmentDescription att = {
        .format = FMT, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &cref,
    };
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = 1, .pDependencies = &dep,
    };
    VK_CHECK(vkCreateRenderPass(p->dev, &rpci, NULL, &ps->render_pass),
             "vkCreateRenderPass (pass)");

    /* Framebuffer attaches the single-mip-0 view; multi-mip out_view is
     * for sampling, not rendering. */
    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = ps->render_pass,
        .attachmentCount = 1, .pAttachments = &ps->fbo_view,
        .width = ps->out_w, .height = ps->out_h, .layers = 1,
    };
    VK_CHECK(vkCreateFramebuffer(p->dev, &fbci, NULL, &ps->framebuffer),
             "vkCreateFramebuffer (pass)");

    /* Dynamic descriptor set layout: UBO@0 + every reflected sampler at
     * its declared binding number. */
    size_t total_bindings = 1 + num_bindings;
    VkDescriptorSetLayoutBinding *dsl_bindings = (VkDescriptorSetLayoutBinding *)
        calloc(total_bindings, sizeof(*dsl_bindings));
    if (!dsl_bindings) { if (err_out) *err_out = xstrdup("oom (dsl bindings)"); goto fail; }
    dsl_bindings[0].binding = 0;
    dsl_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    dsl_bindings[0].descriptorCount = 1;
    dsl_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (size_t k = 0; k < num_bindings; ++k) {
        dsl_bindings[1 + k].binding         = ps->mod->samplers[k].binding;
        dsl_bindings[1 + k].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dsl_bindings[1 + k].descriptorCount = 1;
        dsl_bindings[1 + k].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = (uint32_t)total_bindings, .pBindings = dsl_bindings,
    };
    VkResult dlr = vkCreateDescriptorSetLayout(p->dev, &dlci, NULL, &ps->dset_layout);
    free(dsl_bindings);
    if (dlr != VK_SUCCESS) {
        if (err_out) *err_out = err_fmt("vkCreateDescriptorSetLayout VkResult=%d", dlr);
        goto fail;
    }

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0, .size = 256,    /* slang push range; we push <=256 bytes */
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &ps->dset_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
    };
    VK_CHECK(vkCreatePipelineLayout(p->dev, &plci, NULL, &ps->pipe_layout),
             "vkCreatePipelineLayout (pass)");

    VkShaderModule vmod = create_shader(p->dev, ps->mod->vert_spv, ps->mod->vert_spv_words);
    VkShaderModule fmod = create_shader(p->dev, ps->mod->frag_spv, ps->mod->frag_spv_words);
    if (!vmod || !fmod) {
        if (err_out) *err_out = xstrdup("vkCreateShaderModule failed (pass)");
        if (vmod) vkDestroyShaderModule(p->dev, vmod, NULL);
        if (fmod) vkDestroyShaderModule(p->dev, fmod, NULL);
        goto fail;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = vmod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fmod, .pName = "main" },
    };
    VkVertexInputBindingDescription vib = {
        .binding = 0, .stride = sizeof(struct vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription via[2] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
          .offset = offsetof(struct vertex, pos) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
          .offset = offsetof(struct vertex, uv) },
    };
    VkPipelineVertexInputStateCreateInfo vis = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1, .pVertexBindingDescriptions   = &vib,
        .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = via,
    };
    VkPipelineInputAssemblyStateCreateInfo ias = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport vp = { 0, 0, (float)ps->out_w, (float)ps->out_h, 0, 1 };
    VkRect2D sc = { {0,0}, { ps->out_w, ps->out_h } };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp,
        .scissorCount  = 1, .pScissors  = &sc,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cbs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vis, .pInputAssemblyState = &ias,
        .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cbs,
        .layout = ps->pipe_layout, .renderPass = ps->render_pass, .subpass = 0,
    };
    VkResult vr = vkCreateGraphicsPipelines(p->dev, VK_NULL_HANDLE, 1, &gpci, NULL, &ps->pipeline);
    vkDestroyShaderModule(p->dev, vmod, NULL);
    vkDestroyShaderModule(p->dev, fmod, NULL);
    if (vr != VK_SUCCESS) {
        if (err_out) *err_out = err_fmt("vkCreateGraphicsPipelines VkResult=%d", vr);
        goto fail;
    }

    /* Allocate per-pass UBO. Sized to the max of (reflected UBO size,
     * sizeof(slang_ubo)) so we always have at least the legacy MVP slot
     * available, and rounded up to multiples of 16 for std140 safety. */
    {
        size_t need = ps->mod ? ps->mod->ubo_total_size : 0;
        if (need < sizeof(struct slang_ubo)) need = sizeof(struct slang_ubo);
        if (need < 256) need = 256;       /* baseline; some shaders push more */
        need = (need + 15) & ~(size_t)15;
        ps->ubo_size = need;
        if (create_buffer(p, need, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &ps->ubo_buf, &ps->ubo_mem, &ps->ubo_ptr,
                          err_out) != 0) goto fail;
        /* Default contents: identity MVP for shaders that need only that.
         * The per-frame loop overwrites with reflected layout when present. */
        memset(ps->ubo_ptr, 0, need);
        struct slang_ubo identity = { .MVP = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 } };
        memcpy(ps->ubo_ptr, &identity, sizeof(identity));
    }

    /* Allocate descriptor set + write. */
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p->dpool, .descriptorSetCount = 1,
        .pSetLayouts = &ps->dset_layout,
    };
    VK_CHECK(vkAllocateDescriptorSets(p->dev, &dsai, &ps->dset),
             "vkAllocateDescriptorSets (pass)");

    /* Descriptor writes: UBO + each sampler. We use a heap array because the
     * per-image descriptor info structs need to outlive the call. */
    VkDescriptorBufferInfo dbi = { .buffer = ps->ubo_buf, .offset = 0, .range = ps->ubo_size };
    VkDescriptorImageInfo *dii = (VkDescriptorImageInfo *)calloc(num_bindings, sizeof(*dii));
    VkWriteDescriptorSet  *writes = (VkWriteDescriptorSet *)calloc(1 + num_bindings, sizeof(*writes));
    if (!writes || (num_bindings > 0 && !dii)) {
        if (err_out) *err_out = xstrdup("oom (descriptor writes)");
        free(dii); free(writes);
        goto fail;
    }
    writes[0] = (VkWriteDescriptorSet) {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ps->dset,
        .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi,
    };
    size_t valid_writes = 1;
    for (size_t k = 0; k < num_bindings; ++k) {
        if (!bindings[k].view) {
            /* Unresolved sampler — skip. The shader read may produce
             * undefined results, but the dispatch won't crash if the
             * binding is in the layout but no descriptor is written.
             * (Vulkan validation will warn; debug builds catch it.) */
            continue;
        }
        dii[k].sampler     = bindings[k].sampler;
        dii[k].imageView   = bindings[k].view;
        dii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[valid_writes] = (VkWriteDescriptorSet) {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ps->dset,
            .dstBinding = ps->mod->samplers[k].binding, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &dii[k],
        };
        ++valid_writes;
    }
    vkUpdateDescriptorSets(p->dev, (uint32_t)valid_writes, writes, 0, NULL);
    free(dii); free(writes);
    return 0;
fail: return -1;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

struct slang_pipeline *slang_pipeline_create(const struct slangp_preset *preset,
                                             unsigned input_w, unsigned input_h,
                                             unsigned output_w, unsigned output_h,
                                             char **err_out)
{
    return slang_pipeline_create_shared(preset, input_w, input_h,
                                        output_w, output_h, NULL, err_out);
}

struct slang_pipeline *slang_pipeline_create_shared(const struct slangp_preset *preset,
                                                    unsigned input_w, unsigned input_h,
                                                    unsigned output_w, unsigned output_h,
                                                    struct slang_pipeline *share,
                                                    char **err_out)
{
    struct slang_pipeline *p = (struct slang_pipeline *)calloc(1, sizeof(*p));
    if (!p) { if (err_out) *err_out = xstrdup("oom"); return NULL; }
    p->input_w = input_w;   p->input_h  = input_h;
    p->output_w = output_w; p->output_h = output_h;

    if (share) {
        p->instance     = share->instance;
        p->phys         = share->phys;
        p->dev          = share->dev;
        p->queue_family = share->queue_family;
        p->queue        = share->queue;
        p->mem_props    = share->mem_props;
        p->owns_device  = false;
    } else {
        if (init_instance_and_device(p, err_out) != 0) goto fail;
        p->owns_device = true;
    }

    /* Shared resources: input image + sampler, staging buffers, vertex/index
     * buffers, UBO, descriptor pool. */
    const VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
    if (create_image(p, input_w, input_h, FMT,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     1,
                     &p->in_img, &p->in_mem, err_out) != 0) goto fail;
    p->in_view = create_view(p->dev, p->in_img, FMT);
    {
        VkSamplerCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .unnormalizedCoordinates = VK_FALSE,
        };
        VK_CHECK(vkCreateSampler(p->dev, &sci, NULL, &p->in_sampler), "vkCreateSampler (in)");
    }

    VkDeviceSize ubytes = (VkDeviceSize)input_w  * input_h  * 4;
    VkDeviceSize rbytes = (VkDeviceSize)output_w * output_h * 4;
    if (create_buffer(p, ubytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &p->stg_upload, &p->stg_upload_mem, &p->stg_upload_ptr, err_out) != 0) goto fail;
    if (create_readback_buffer(p, rbytes, err_out) != 0) goto fail;

    if (create_buffer(p, sizeof(QUAD_VERTS), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &p->vbuf, &p->vbuf_mem, NULL, err_out) != 0) goto fail;
    { void *m; VK_CHECK(vkMapMemory(p->dev, p->vbuf_mem, 0, sizeof(QUAD_VERTS), 0, &m), "map vbuf");
      memcpy(m, QUAD_VERTS, sizeof(QUAD_VERTS)); vkUnmapMemory(p->dev, p->vbuf_mem); }
    if (create_buffer(p, sizeof(QUAD_INDICES), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &p->ibuf, &p->ibuf_mem, NULL, err_out) != 0) goto fail;
    { void *m; VK_CHECK(vkMapMemory(p->dev, p->ibuf_mem, 0, sizeof(QUAD_INDICES), 0, &m), "map ibuf");
      memcpy(m, QUAD_INDICES, sizeof(QUAD_INDICES)); vkUnmapMemory(p->dev, p->ibuf_mem); }

    if (create_buffer(p, sizeof(struct slang_ubo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &p->ubo, &p->ubo_mem, &p->ubo_ptr, err_out) != 0) goto fail;
    {
        struct slang_ubo u = {0};
        u.MVP[0] = 1.0f; u.MVP[5] = 1.0f; u.MVP[10] = 1.0f; u.MVP[15] = 1.0f;
        memcpy(p->ubo_ptr, &u, sizeof(u));
    }

    /* Determine pass count (built-in passthrough if 0). */
    size_t n_passes = (preset && preset->num_passes > 0) ? preset->num_passes : 1;
    p->passes = (struct pass_state *)calloc(n_passes, sizeof(*p->passes));
    if (!p->passes) { if (err_out) *err_out = xstrdup("oom (passes)"); goto fail; }
    p->num_passes = n_passes;

    /* Descriptor pool: enough for all passes + future feedback bindings. */
    VkDescriptorPoolSize pool_sizes[2] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,        (uint32_t)n_passes },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,(uint32_t)n_passes },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = (uint32_t)n_passes,
        .poolSizeCount = 2, .pPoolSizes = pool_sizes,
    };
    VK_CHECK(vkCreateDescriptorPool(p->dev, &dpci, NULL, &p->dpool), "vkCreateDescriptorPool");

    /* ---- Phase A: compile shaders + allocate output images per pass.    */
    /* No descriptor sets, no pipelines yet — those need feedback         */
    /* detection (Phase B/C) which itself needs all shaders compiled.     */
    {
        uint32_t prev_w = input_w, prev_h = input_h;
        for (size_t i = 0; i < n_passes; ++i) {
            struct pass_state *ps = &p->passes[i];
            const struct slangp_pass *ppass = (preset && preset->num_passes > 0)
                                              ? &preset->passes[i] : NULL;
            char *cerr = NULL;
            if (ppass && ppass->path) ps->mod = slang_compile_file(ppass->path, &cerr);
            else                       ps->mod = slang_compile_string(BUILTIN_SLANG, NULL, &cerr);
            if (!ps->mod) {
                if (err_out) *err_out = err_fmt("compiling pass %zu (%s) failed: %s",
                                                i, (ppass && ppass->path) ? ppass->path : "<built-in>",
                                                cerr ? cerr : "(unknown)");
                free(cerr);
                goto fail;
            }
            if (preset && preset->num_params > 0) {
                for (size_t k = 0; k < ps->mod->num_params; ++k)
                    for (size_t m = 0; m < preset->num_params; ++m)
                        if (preset->params[m].name && ps->mod->params[k].name &&
                            strcmp(preset->params[m].name, ps->mod->params[k].name) == 0)
                            ps->mod->params[k].default_value = preset->params[m].value;
            }

            uint32_t pw, ph;
            if (ppass) resolve_pass_dims(ppass, prev_w, prev_h, output_w, output_h, &pw, &ph);
            else       { pw = output_w; ph = output_h; }
            if (i == n_passes - 1) { pw = output_w; ph = output_h; }

            /* Format from preset directives. The final pass is forced to
             * R8G8B8A8_UNORM because the readback path memcpy's the buffer
             * straight to host RGBA bytes — float / 16-bit formats would
             * produce garbage on stdout. */
            VkFormat pfmt = (i == n_passes - 1) ? VK_FORMAT_R8G8B8A8_UNORM
                                                 : resolve_pass_format(ppass);
            if (getenv("SLANGFX_DEBUG_FORMAT")) {
                fprintf(stderr,
                        "[pass %zu] %ux%u  format=%d  float_fb=%d  srgb_fb=%d  fbo_fmt=%d\n",
                        i, pw, ph, (int)pfmt,
                        ppass ? ppass->float_framebuffer : 0,
                        ppass ? ppass->srgb_framebuffer  : 0,
                        ppass ? (int)ppass->fbo_format    : 0);
            }
            /* Mip chain on this pass's output is needed iff a downstream
             * pass declared `mipmap_input<n> = true` (which targets that
             * pass's Source = pass n-1's output). */
            uint32_t mips = 1;
            if (preset && preset->num_passes > 0 && i + 1 < preset->num_passes &&
                preset->passes[i + 1].mipmap_input) {
                mips = mip_levels_for(pw, ph);
            }
            if (alloc_pass_image(p, ps, ppass, pw, ph, pfmt, mips, err_out) != 0) goto fail;
            if (getenv("SLANGFX_DEBUG_FORMAT") && mips > 1)
                fprintf(stderr, "[pass %zu] mip_levels=%u\n", i, mips);

            prev_w = pw; prev_h = ph;
        }
    }

    /* ---- Phase B: build alias table from preset. ---- */
    if (preset && preset->num_passes > 0) {
        size_t na = 0;
        for (size_t i = 0; i < preset->num_passes; ++i)
            if (preset->passes[i].alias && *preset->passes[i].alias) ++na;
        if (na > 0) {
            p->aliases = (struct alias_entry *)calloc(na, sizeof(*p->aliases));
            if (!p->aliases) { if (err_out) *err_out = xstrdup("oom (aliases)"); goto fail; }
            p->num_aliases = na;
            size_t ai = 0;
            for (size_t i = 0; i < preset->num_passes; ++i) {
                if (preset->passes[i].alias && *preset->passes[i].alias) {
                    p->aliases[ai].name = xstrdup(preset->passes[i].alias);
                    p->aliases[ai].pass_idx = i;
                    ++ai;
                }
            }
        }
    }

    /* ---- Phase C: resolve every reflected sampler in every pass.        */
    /* This populates a per-pass `resolved_binding` array AND marks any   */
    /* pass referenced via PassFeedback<i> / <alias>Feedback as a         */
    /* feedback producer (so we know to allocate its feedback image).    */
    struct resolved_binding **all_bindings = (struct resolved_binding **)
        calloc(n_passes, sizeof(*all_bindings));
    if (!all_bindings) { if (err_out) *err_out = xstrdup("oom (bindings)"); goto fail; }
    {
        VkImageView prev_view    = p->in_view;
        VkSampler   prev_sampler = p->in_sampler;
        for (size_t i = 0; i < n_passes; ++i) {
            struct pass_state *ps = &p->passes[i];
            size_t ns = ps->mod ? ps->mod->num_samplers : 0;
            if (ns > 0) {
                all_bindings[i] = (struct resolved_binding *)
                    calloc(ns, sizeof(*all_bindings[i]));
                if (!all_bindings[i]) {
                    if (err_out) *err_out = xstrdup("oom (per-pass bindings)");
                    goto cleanup_bindings;
                }
                for (size_t s = 0; s < ns; ++s) {
                    const char *nm = ps->mod->samplers[s].name;
                    if (resolve_sampler_name(p, i, nm,
                                             prev_view, prev_sampler,
                                             &all_bindings[i][s]) != 0) {
                        /* Unresolved: leave view/sampler NULL — the
                         * descriptor write step skips it. */
                        if (err_out && !*err_out) {
                            /* don't overwrite earlier errors */
                        }
                    }
                }
            }
            prev_view    = ps->out_view;
            prev_sampler = ps->sampler;
        }
    }

    /* ---- Phase D: allocate feedback images for marked producers. ---- */
    for (size_t i = 0; i < n_passes; ++i) {
        if (p->passes[i].is_feedback_producer) {
            if (alloc_feedback_image(p, &p->passes[i], err_out) != 0) goto cleanup_bindings;
        }
    }

    /* ---- Phase E: patch feedback bindings with the now-allocated views. ---- */
    for (size_t i = 0; i < n_passes; ++i) {
        for (size_t s = 0; s < (p->passes[i].mod ? p->passes[i].mod->num_samplers : 0); ++s) {
            struct resolved_binding *b = &all_bindings[i][s];
            if (b->is_feedback && b->feedback_pass >= 0 &&
                (size_t)b->feedback_pass < n_passes) {
                b->view    = p->passes[b->feedback_pass].feedback_view;
                b->sampler = p->passes[b->feedback_pass].sampler;
            }
        }
    }

    /* ---- Phase F: descriptor pool sized for all passes' bindings. ---- */
    {
        uint32_t total_ubo = (uint32_t)n_passes;
        uint32_t total_samplers = 0;
        for (size_t i = 0; i < n_passes; ++i)
            if (p->passes[i].mod) total_samplers += (uint32_t)p->passes[i].mod->num_samplers;
        if (total_samplers == 0) total_samplers = 1;  /* pool can't be empty */
        VkDescriptorPoolSize ps_arr[2] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, total_ubo },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, total_samplers },
        };
        VkDescriptorPoolCreateInfo dpci2 = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = total_ubo,
            .poolSizeCount = 2, .pPoolSizes = ps_arr,
        };
        if (p->dpool) vkDestroyDescriptorPool(p->dev, p->dpool, NULL);
        if (vkCreateDescriptorPool(p->dev, &dpci2, NULL, &p->dpool) != VK_SUCCESS) {
            if (err_out) *err_out = xstrdup("vkCreateDescriptorPool (resized)");
            goto cleanup_bindings;
        }
    }

    /* ---- Phase F: build per-pass render pass + framebuffer + pipeline. ---- */
    for (size_t i = 0; i < n_passes; ++i) {
        struct pass_state *ps = &p->passes[i];
        size_t ns = ps->mod ? ps->mod->num_samplers : 0;
        if (build_pass_pipeline(p, ps, all_bindings[i], ns, err_out) != 0)
            goto cleanup_bindings;
    }

    /* Free the per-pass bindings scratch array. */
    for (size_t i = 0; i < n_passes; ++i) free(all_bindings[i]);
    free(all_bindings);
    all_bindings = NULL;

    /* Command pool/buffer + fence. */
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = p->queue_family,
    };
    VK_CHECK(vkCreateCommandPool(p->dev, &cpci, NULL, &p->cmd_pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p->cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_CHECK(vkAllocateCommandBuffers(p->dev, &cbai, &p->cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(vkCreateFence(p->dev, &fci, NULL, &p->fence), "vkCreateFence");

    return p;
cleanup_bindings:
    if (all_bindings) {
        for (size_t i = 0; i < n_passes; ++i) free(all_bindings[i]);
        free(all_bindings);
    }
fail:
    slang_pipeline_destroy(p);
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Per-frame dispatch                                                         */
/* -------------------------------------------------------------------------- */

static void barrier_range(VkCommandBuffer cmd, VkImage img,
                          uint32_t base_mip, uint32_t mip_count,
                          VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags src_s, VkPipelineStageFlags dst_s,
                          VkAccessFlags src_a, VkAccessFlags dst_a)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_a, .dstAccessMask = dst_a,
        .oldLayout = from, .newLayout = to, .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, base_mip, mip_count, 0, 1 },
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    };
    vkCmdPipelineBarrier(cmd, src_s, dst_s, 0, 0, NULL, 0, NULL, 1, &b);
}

static void barrier(VkCommandBuffer cmd, VkImage img,
                    VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags src_s, VkPipelineStageFlags dst_s,
                    VkAccessFlags src_a, VkAccessFlags dst_a)
{
    barrier_range(cmd, img, 0, 1, from, to, src_s, dst_s, src_a, dst_a);
}

/* Generate a full mip chain for `img` via a vkCmdBlitImage cascade.
 * Precondition: mip 0 is in SHADER_READ_ONLY layout (the renderpass's
 * finalLayout); mip 1+ are in UNDEFINED. Postcondition: every level in
 * SHADER_READ_ONLY, ready for downstream sampling. */
static void generate_mipmaps(VkCommandBuffer cmd, VkImage img,
                             uint32_t w, uint32_t h, uint32_t mip_levels)
{
    if (mip_levels <= 1) return;

    /* mip 0: SHADER_READ_ONLY -> TRANSFER_SRC. */
    barrier_range(cmd, img, 0, 1,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    int32_t mw = (int32_t)w, mh = (int32_t)h;
    for (uint32_t lvl = 1; lvl < mip_levels; ++lvl) {
        int32_t nw = mw > 1 ? mw / 2 : 1;
        int32_t nh = mh > 1 ? mh / 2 : 1;

        /* dst: UNDEFINED -> TRANSFER_DST. */
        barrier_range(cmd, img, lvl, 1,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT);

        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, lvl - 1, 0, 1 },
            .srcOffsets = { {0,0,0}, { mw, mh, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, lvl, 0, 1 },
            .dstOffsets = { {0,0,0}, { nw, nh, 1 } },
        };
        vkCmdBlitImage(cmd,
                       img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        /* dst: TRANSFER_DST -> TRANSFER_SRC (so next iteration can read from it). */
        barrier_range(cmd, img, lvl, 1,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

        mw = nw; mh = nh;
    }

    /* All levels: TRANSFER_SRC -> SHADER_READ_ONLY. */
    barrier_range(cmd, img, 0, mip_levels,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
}

int slang_pipeline_set_param(struct slang_pipeline *p,
                             const char *name, float value)
{
    if (!p || !name) return 0;
    int hits = 0;
    for (size_t i = 0; i < p->num_passes; ++i) {
        struct slang_module *mod = p->passes[i].mod;
        if (!mod) continue;
        for (size_t k = 0; k < mod->num_params; ++k) {
            struct slang_param *pp = &mod->params[k];
            if (!pp->name || strcmp(pp->name, name) != 0) continue;
            float v = value;
            /* Clamp to the declared range when it's a real interval. */
            if (pp->max_value > pp->min_value) {
                if (v < pp->min_value) v = pp->min_value;
                if (v > pp->max_value) v = pp->max_value;
            }
            pp->default_value = v;   /* run() re-reads this every frame */
            ++hits;
        }
    }
    return hits;
}

/* Record one pipeline's frame — first-frame feedback init, all passes, and
 * the end-of-frame feedback snapshots — into `cmd`. Precondition: the
 * pipeline's input image is populated (staging upload or GPU blit) and in
 * SHADER_READ_ONLY layout. Postcondition: the final pass image holds the
 * result, in SHADER_READ_ONLY layout unless the final pass is a feedback
 * producer (the snapshot pass leaves that one in TRANSFER_SRC; the caller
 * transitions/reads it either way). */
static void record_frame(struct slang_pipeline *p, VkCommandBuffer cmd,
                         double time_sec)
{
    /* Resolve the `Time` standard field (seconds). A realtime source supplies
     * a real timestamp (PTS) so time-based effects stay correct under pacing
     * and dropped frames; when it is unknown (time_sec < 0, e.g. the untimed
     * stdio source) we fall back to a wall clock from the first frame. */
    if (time_sec < 0.0) {
        double now = slang_now_ms();
        if (!p->clock_started) { p->clock_start_ms = now; p->clock_started = true; }
        time_sec = (now - p->clock_start_ms) / 1000.0;
    }
    float time_val = (float)time_sec;

    /* Push constants block (same SourceSize/OutputSize/FrameCount applied
     *    to all passes for now; per-pass dim-specific values are populated
     *    lazily inside the loop). */
    struct slang_push pc = {0};
    pc.original_size[0] = (float)p->input_w;
    pc.original_size[1] = (float)p->input_h;
    pc.original_size[2] = 1.0f / (float)p->input_w;
    pc.original_size[3] = 1.0f / (float)p->input_h;
    pc.frame_count      = p->frame_count++;

    /* Phase 6: First-frame feedback initialization. Each producer's
     * feedback image starts in UNDEFINED layout with undefined contents;
     * clear to opaque-black and transition to SHADER_READ_ONLY so the
     * frame-0 PassFeedback samples return deterministic black. */
    if (p->frame_count == 1) {
        for (size_t i = 0; i < p->num_passes; ++i) {
            if (!p->passes[i].is_feedback_producer) continue;
            barrier(cmd, p->passes[i].feedback_img,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkClearColorValue cc = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
            VkImageSubresourceRange srr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cmd, p->passes[i].feedback_img,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &srr);
            barrier(cmd, p->passes[i].feedback_img,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
    }

    /* Iterate passes. */
    uint32_t prev_w = p->input_w, prev_h = p->input_h;
    /* Reflected push-constant blob, sized per pass to match the shader's
     * declared layout (Phase 7). 256 bytes is the Vulkan-required minimum
     * push range; well beyond what slang shaders use. */
    uint8_t  push_blob[256];
    uint32_t push_size_used;
    for (size_t i = 0; i < p->num_passes; ++i) {
        struct pass_state *ps = &p->passes[i];

        /* Per-pass push constants. */
        pc.source_size[0] = (float)prev_w; pc.source_size[1] = (float)prev_h;
        pc.source_size[2] = 1.0f / (float)prev_w;
        pc.source_size[3] = 1.0f / (float)prev_h;
        pc.output_size[0] = (float)ps->out_w; pc.output_size[1] = (float)ps->out_h;
        pc.output_size[2] = 1.0f / (float)ps->out_w;
        pc.output_size[3] = 1.0f / (float)ps->out_h;

        /* Helper: write `value` (`size` bytes) into `blob` at the offset
         * where `name` was found in `fields`. The `is_push` flag tracks the
         * high-water mark of bytes written so we can size the
         * vkCmdPushConstants call. No-op when name not found. */
        #define WRITE_FIELD(blob, blob_size, fields, n_fields, is_push, name_str, value_ptr, value_size) \
            do {                                                                                          \
                for (size_t _i = 0; _i < (n_fields); ++_i) {                                              \
                    const struct slang_push_field *_f = &(fields)[_i];                                    \
                    if (_f->name && strcmp(_f->name, (name_str)) == 0) {                                  \
                        if ((size_t)_f->offset + (value_size) <= (blob_size))                             \
                            memcpy((blob) + _f->offset, (value_ptr), (value_size));                       \
                        if ((is_push) && (size_t)_f->offset + (value_size) > push_size_used)              \
                            push_size_used = _f->offset + (value_size);                                   \
                        break;                                                                            \
                    }                                                                                    \
                }                                                                                        \
            } while (0)

        /* Standard slang field values for this pass. */
        int32_t  frame_dir = 1;
        uint32_t rot       = 0;

        /* Build a push-constant blob AND write the per-pass UBO contents
         * honoring this shader's reflected layout. Each field is written to
         * push OR ubo depending on which reflected struct declared it
         * (slang shaders are free to put SourceSize, FrameCount,
         * parameters, etc. in either; ntsc-pass2 uses the UBO). */
        memset(push_blob, 0, sizeof(push_blob));
        push_size_used = 0;

        /* Reset the per-pass UBO to identity-MVP defaults each frame. */
        memset(ps->ubo_ptr, 0, ps->ubo_size);
        {
            struct slang_ubo identity = { .MVP = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 } };
            memcpy(ps->ubo_ptr, &identity, sizeof(identity));
        }

        if (ps->mod && (ps->mod->num_push_fields > 0 || ps->mod->num_ubo_fields > 0)) {
            /* Populate push */
            WRITE_FIELD(push_blob, sizeof(push_blob),
                        ps->mod->push_fields, ps->mod->num_push_fields, true,
                        "SourceSize",         pc.source_size,   16);
            WRITE_FIELD(push_blob, sizeof(push_blob),
                        ps->mod->push_fields, ps->mod->num_push_fields, true,
                        "OriginalSize",       pc.original_size, 16);
            WRITE_FIELD(push_blob, sizeof(push_blob),
                        ps->mod->push_fields, ps->mod->num_push_fields, true,
                        "OutputSize",         pc.output_size,   16);
            WRITE_FIELD(push_blob, sizeof(push_blob),
                        ps->mod->push_fields, ps->mod->num_push_fields, true,
                        "FinalViewportSize",  pc.output_size,   16);
            WRITE_FIELD(push_blob, sizeof(push_blob),
                        ps->mod->push_fields, ps->mod->num_push_fields, true,
                        "FrameCount",         &pc.frame_count,   4);
            WRITE_FIELD(push_blob, sizeof(push_blob),
                        ps->mod->push_fields, ps->mod->num_push_fields, true,
                        "FrameDirection",     &frame_dir,        4);
            WRITE_FIELD(push_blob, sizeof(push_blob),
                        ps->mod->push_fields, ps->mod->num_push_fields, true,
                        "Rotation",           &rot,              4);
            WRITE_FIELD(push_blob, sizeof(push_blob),
                        ps->mod->push_fields, ps->mod->num_push_fields, true,
                        "Time",               &time_val,         4);

            /* Populate UBO with the same standard fields if the shader
             * declared them there. */
            uint8_t *ubo = (uint8_t *)ps->ubo_ptr;
            WRITE_FIELD(ubo, ps->ubo_size,
                        ps->mod->ubo_fields, ps->mod->num_ubo_fields, false,
                        "SourceSize",         pc.source_size,   16);
            WRITE_FIELD(ubo, ps->ubo_size,
                        ps->mod->ubo_fields, ps->mod->num_ubo_fields, false,
                        "OriginalSize",       pc.original_size, 16);
            WRITE_FIELD(ubo, ps->ubo_size,
                        ps->mod->ubo_fields, ps->mod->num_ubo_fields, false,
                        "OutputSize",         pc.output_size,   16);
            WRITE_FIELD(ubo, ps->ubo_size,
                        ps->mod->ubo_fields, ps->mod->num_ubo_fields, false,
                        "FinalViewportSize",  pc.output_size,   16);
            WRITE_FIELD(ubo, ps->ubo_size,
                        ps->mod->ubo_fields, ps->mod->num_ubo_fields, false,
                        "FrameCount",         &pc.frame_count,   4);
            WRITE_FIELD(ubo, ps->ubo_size,
                        ps->mod->ubo_fields, ps->mod->num_ubo_fields, false,
                        "FrameDirection",     &frame_dir,        4);
            WRITE_FIELD(ubo, ps->ubo_size,
                        ps->mod->ubo_fields, ps->mod->num_ubo_fields, false,
                        "Rotation",           &rot,              4);
            WRITE_FIELD(ubo, ps->ubo_size,
                        ps->mod->ubo_fields, ps->mod->num_ubo_fields, false,
                        "Time",               &time_val,         4);

            /* Aliased / numbered / feedback `<TexName>Size` fields: the
             * libretro slang spec lets shaders declare a vec4 size for any
             * sampled texture (Pass<n>, alias, PassFeedback<n>, etc.).
             * bloom_blend.slang and friends rely on this for sub-pixel
             * snapping. We scan every reflected field and populate any
             * `<X>Size` whose X resolves to a known pass. */
            for (size_t k = 0; k < ps->mod->num_push_fields; ++k) {
                uint32_t fw, fh;
                if (lookup_size_field(p, ps->mod->push_fields[k].name, &fw, &fh)) {
                    float sz[4] = { (float)fw, (float)fh,
                                    1.0f / (float)fw, 1.0f / (float)fh };
                    uint32_t off = ps->mod->push_fields[k].offset;
                    if ((size_t)off + 16 <= sizeof(push_blob)) {
                        memcpy(push_blob + off, sz, 16);
                        if (off + 16 > push_size_used) push_size_used = off + 16;
                    }
                }
            }
            for (size_t k = 0; k < ps->mod->num_ubo_fields; ++k) {
                uint32_t fw, fh;
                if (lookup_size_field(p, ps->mod->ubo_fields[k].name, &fw, &fh)) {
                    float sz[4] = { (float)fw, (float)fh,
                                    1.0f / (float)fw, 1.0f / (float)fh };
                    uint32_t off = ps->mod->ubo_fields[k].offset;
                    if ((size_t)off + 16 <= ps->ubo_size)
                        memcpy(ubo + off, sz, 16);
                }
            }

            /* Apply each #pragma parameter default at its resolved offset.
             * Try push first; if not found there, try the UBO. */
            for (size_t f = 0; f < ps->mod->num_params; ++f) {
                const struct slang_param *pp = &ps->mod->params[f];
                if (!pp->name) continue;
                bool wrote_push = false;
                for (size_t k = 0; k < ps->mod->num_push_fields; ++k) {
                    if (ps->mod->push_fields[k].name &&
                        strcmp(ps->mod->push_fields[k].name, pp->name) == 0) {
                        uint32_t off = ps->mod->push_fields[k].offset;
                        if (off + 4 <= sizeof(push_blob)) {
                            memcpy(push_blob + off, &pp->default_value, 4);
                            if (off + 4 > push_size_used) push_size_used = off + 4;
                            wrote_push = true;
                        }
                        break;
                    }
                }
                if (!wrote_push) {
                    for (size_t k = 0; k < ps->mod->num_ubo_fields; ++k) {
                        if (ps->mod->ubo_fields[k].name &&
                            strcmp(ps->mod->ubo_fields[k].name, pp->name) == 0) {
                            uint32_t off = ps->mod->ubo_fields[k].offset;
                            if ((size_t)off + 4 <= ps->ubo_size)
                                memcpy(ubo + off, &pp->default_value, 4);
                            break;
                        }
                    }
                }
            }
        } else {
            /* No reflection (built-in passthrough fallback): legacy push. */
            memcpy(push_blob, &pc, sizeof(pc));
            push_size_used = sizeof(pc);
        }
        push_size_used = (push_size_used + 3) & ~3u;
        if (push_size_used == 0) push_size_used = 4;
        if (push_size_used > sizeof(push_blob)) push_size_used = sizeof(push_blob);
        #undef WRITE_FIELD

        VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 1 } } };
        VkRenderPassBeginInfo rpbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = ps->render_pass, .framebuffer = ps->framebuffer,
            .renderArea = { {0,0}, { ps->out_w, ps->out_h } },
            .clearValueCount = 1, .pClearValues = &clear,
        };
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ps->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ps->pipe_layout, 0, 1, &ps->dset, 0, NULL);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &p->vbuf, &off);
        vkCmdBindIndexBuffer(cmd, p->ibuf, 0, VK_INDEX_TYPE_UINT16);
        vkCmdPushConstants(cmd, ps->pipe_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, push_size_used, push_blob);
        vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
        vkCmdEndRenderPass(cmd);

        /* Render pass leaves out_img mip 0 in SHADER_READ_ONLY (next
         * pass's Source binding samples it directly). When this pass's
         * output feeds a downstream `mipmap_input` pass, generate the
         * full mip chain via vkCmdBlitImage cascade. */
        if (ps->mip_levels > 1)
            generate_mipmaps(cmd, ps->out_img, ps->out_w, ps->out_h, ps->mip_levels);

        prev_w = ps->out_w; prev_h = ps->out_h;
    }

    /* Phase 6: snapshot each feedback producer's output for next frame.
     * Run BEFORE the last-pass readback so any producer (including the
     * last pass itself) is captured cleanly. */
    for (size_t i = 0; i < p->num_passes; ++i) {
        struct pass_state *prod = &p->passes[i];
        if (!prod->is_feedback_producer) continue;

        barrier(cmd, prod->out_img,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier(cmd, prod->feedback_img,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

        VkImageCopy copy = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .extent = { prod->out_w, prod->out_h, 1 },
        };
        vkCmdCopyImage(cmd,
                       prod->out_img,      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       prod->feedback_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &copy);

        /* Restore feedback_img to SHADER_READ_ONLY for next frame's reads. */
        barrier(cmd, prod->feedback_img,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

        /* Restore out_img to SHADER_READ_ONLY (unless it's the last pass —
         * we'll transition that one to TRANSFER_SRC just below for readback). */
        if (i != p->num_passes - 1) {
            barrier(cmd, prod->out_img,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
    }

}

int slang_pipeline_run(struct slang_pipeline *p, const uint8_t *src, uint8_t *dst,
                       double time_sec)
{
    return slang_chain_run(&p, 1, src, dst, time_sec);
}

int slang_chain_run(struct slang_pipeline **pipes, size_t n,
                    const uint8_t *src, uint8_t *dst, double time_sec)
{
    if (!pipes || n == 0 || !src || !dst) return -1;
    struct slang_pipeline *first = pipes[0];
    struct slang_pipeline *final = pipes[n - 1];
    VkCommandBuffer cmd = first->cmd;

    /* 1. Upload input into the first pipeline's staging buffer. */
    size_t ubytes = (size_t)first->input_w * first->input_h * 4;
    memcpy(first->stg_upload_ptr, src, ubytes);

    /* 2. Record the whole chain into ONE command buffer: upload, every
     * pipeline's passes with a GPU-side blit between pipelines, and a single
     * readback at the end. One submit + one fence per frame regardless of
     * how many presets are stacked. */
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cmd, &cbbi) != VK_SUCCESS) return -2;

    barrier(cmd, first->in_img,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy bic = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { first->input_w, first->input_h, 1 },
    };
    vkCmdCopyBufferToImage(cmd, first->stg_upload, first->in_img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    barrier(cmd, first->in_img,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    for (size_t i = 0; i < n; ++i) {
        record_frame(pipes[i], cmd, time_sec);

        /* This pipeline's final image feeds either the next pipeline (blit)
         * or the readback; both need TRANSFER_SRC. A feedback-producing
         * final pass is already there (see record_frame postcondition). */
        struct pass_state *last = &pipes[i]->passes[pipes[i]->num_passes - 1];
        if (!last->is_feedback_producer) {
            barrier(cmd, last->out_img,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }

        if (i + 1 < n) {
            /* GPU-side handoff into the next pipeline's input image. Blit
             * (not copy) so a float-framebuffer final pass converts back to
             * the RGBA8 input format; extents match, so it is 1:1. */
            struct slang_pipeline *nx = pipes[i + 1];
            barrier(cmd, nx->in_img,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageBlit blit = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffsets = { {0, 0, 0},
                                { (int32_t)last->out_w, (int32_t)last->out_h, 1 } },
                .dstOffsets = { {0, 0, 0},
                                { (int32_t)nx->input_w, (int32_t)nx->input_h, 1 } },
            };
            vkCmdBlitImage(cmd,
                           last->out_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           nx->in_img,    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_NEAREST);
            barrier(cmd, nx->in_img,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            /* last->out_img stays TRANSFER_SRC; next frame's render pass
             * re-initializes it (attachment initialLayout is UNDEFINED). */
        }
    }

    /* 3. Read back only the final pipeline's output. */
    struct pass_state *tail = &final->passes[final->num_passes - 1];
    VkBufferImageCopy bic2 = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { tail->out_w, tail->out_h, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, tail->out_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           final->stg_readback, 1, &bic2);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return -3;

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };
    vkResetFences(first->dev, 1, &first->fence);
    if (vkQueueSubmit(first->queue, 1, &si, first->fence) != VK_SUCCESS) return -4;
    vkWaitForFences(first->dev, 1, &first->fence, VK_TRUE, UINT64_MAX);

    /* 4. Read back. Cached readback memory needs an invalidate so the CPU
     * sees the GPU's just-written bytes (no-op when the type is coherent). */
    size_t rbytes = (size_t)final->output_w * final->output_h * 4;
    if (!final->stg_readback_coherent) {
        VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = final->stg_readback_mem, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        vkInvalidateMappedMemoryRanges(final->dev, 1, &range);
    }
    memcpy(dst, final->stg_readback_ptr, rbytes);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Teardown                                                                   */
/* -------------------------------------------------------------------------- */

static void destroy_pass(VkDevice dev, struct pass_state *ps)
{
    if (ps->pipeline)      vkDestroyPipeline(dev, ps->pipeline, NULL);
    if (ps->pipe_layout)   vkDestroyPipelineLayout(dev, ps->pipe_layout, NULL);
    if (ps->dset_layout)   vkDestroyDescriptorSetLayout(dev, ps->dset_layout, NULL);
    if (ps->framebuffer)   vkDestroyFramebuffer(dev, ps->framebuffer, NULL);
    if (ps->render_pass)   vkDestroyRenderPass(dev, ps->render_pass, NULL);
    if (ps->ubo_mem)       { vkUnmapMemory(dev, ps->ubo_mem); vkFreeMemory(dev, ps->ubo_mem, NULL); }
    if (ps->ubo_buf)       vkDestroyBuffer(dev, ps->ubo_buf, NULL);
    if (ps->sampler)       vkDestroySampler(dev, ps->sampler, NULL);
    if (ps->feedback_view) vkDestroyImageView(dev, ps->feedback_view, NULL);
    if (ps->feedback_img)  vkDestroyImage(dev, ps->feedback_img, NULL);
    if (ps->feedback_mem)  vkFreeMemory(dev, ps->feedback_mem, NULL);
    if (ps->fbo_view && ps->fbo_view != ps->out_view)
        vkDestroyImageView(dev, ps->fbo_view, NULL);
    if (ps->out_view)      vkDestroyImageView(dev, ps->out_view, NULL);
    if (ps->out_img)       vkDestroyImage(dev, ps->out_img, NULL);
    if (ps->out_mem)       vkFreeMemory(dev, ps->out_mem, NULL);
    slang_module_free(ps->mod);
    /* dset is freed when descriptor pool is destroyed */
}

void slang_pipeline_destroy(struct slang_pipeline *p)
{
    if (!p) return;
    if (p->dev) vkDeviceWaitIdle(p->dev);

    if (p->fence)        vkDestroyFence(p->dev, p->fence, NULL);
    if (p->cmd_pool)     vkDestroyCommandPool(p->dev, p->cmd_pool, NULL);

    if (p->passes) {
        for (size_t i = 0; i < p->num_passes; ++i) destroy_pass(p->dev, &p->passes[i]);
        free(p->passes);
    }

    if (p->dpool)        vkDestroyDescriptorPool(p->dev, p->dpool, NULL);

    if (p->ubo_mem)          { vkUnmapMemory(p->dev, p->ubo_mem); vkFreeMemory(p->dev, p->ubo_mem, NULL); }
    if (p->stg_upload_mem)   { vkUnmapMemory(p->dev, p->stg_upload_mem);   vkFreeMemory(p->dev, p->stg_upload_mem, NULL); }
    if (p->stg_readback_mem) { vkUnmapMemory(p->dev, p->stg_readback_mem); vkFreeMemory(p->dev, p->stg_readback_mem, NULL); }
    if (p->vbuf_mem)         vkFreeMemory(p->dev, p->vbuf_mem, NULL);
    if (p->ibuf_mem)         vkFreeMemory(p->dev, p->ibuf_mem, NULL);

    if (p->ubo)          vkDestroyBuffer(p->dev, p->ubo, NULL);
    if (p->stg_upload)   vkDestroyBuffer(p->dev, p->stg_upload, NULL);
    if (p->stg_readback) vkDestroyBuffer(p->dev, p->stg_readback, NULL);
    if (p->vbuf)         vkDestroyBuffer(p->dev, p->vbuf, NULL);
    if (p->ibuf)         vkDestroyBuffer(p->dev, p->ibuf, NULL);

    if (p->in_sampler)   vkDestroySampler(p->dev, p->in_sampler, NULL);
    if (p->in_view)      vkDestroyImageView(p->dev, p->in_view, NULL);
    if (p->in_img)       vkDestroyImage(p->dev, p->in_img, NULL);
    if (p->in_mem)       vkFreeMemory(p->dev, p->in_mem, NULL);

    /* Aliases + external textures. */
    for (size_t i = 0; i < p->num_aliases; ++i) free(p->aliases[i].name);
    free(p->aliases);
    for (size_t i = 0; i < p->num_ext_textures; ++i) {
        free(p->ext_textures[i].name);
        if (p->ext_textures[i].sampler) vkDestroySampler(p->dev, p->ext_textures[i].sampler, NULL);
        if (p->ext_textures[i].view)    vkDestroyImageView(p->dev, p->ext_textures[i].view, NULL);
        if (p->ext_textures[i].img)     vkDestroyImage(p->dev, p->ext_textures[i].img, NULL);
        if (p->ext_textures[i].mem)     vkFreeMemory(p->dev, p->ext_textures[i].mem, NULL);
    }
    free(p->ext_textures);

    if (p->owns_device) {
        if (p->dev)      vkDestroyDevice(p->dev, NULL);
        if (p->instance) vkDestroyInstance(p->instance, NULL);
    }

    free(p);
}
