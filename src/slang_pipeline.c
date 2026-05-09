/*
 * vf-slang — Vulkan offscreen render pipeline.
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

    /* The pass's framebuffer image (= this pass's output, = next pass's
     * Source). Always created with COLOR_ATTACHMENT + SAMPLED + TRANSFER_SRC
     * so it can serve all three roles (write target, sampled by next pass,
     * copied to readback for the final pass). */
    VkImage        out_img;
    VkDeviceMemory out_mem;
    VkImageView    out_view;
    VkSampler      sampler;

    VkRenderPass   render_pass;
    VkFramebuffer  framebuffer;

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

    uint32_t frame_count;
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
        .pApplicationName = "vfslang", .applicationVersion = 1,
        .pEngineName = "vfslang", .engineVersion = 1,
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
                        VkImage *img_out, VkDeviceMemory *mem_out,
                        char **err_out)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
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

static VkImageView create_view(VkDevice d, VkImage img, VkFormat fmt)
{
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(d, &ivci, NULL, &v);
    return v;
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

/* -------------------------------------------------------------------------- */
/* Per-pass setup                                                             */
/* -------------------------------------------------------------------------- */

/* Resolve a pass's output dimensions from its slangp scale rules.
 * `prev_w/h` is the previous pass's output (or the input frame for pass 0).
 * `final_w/h` is the final viewport (downstream output of vfslang).
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

static int build_pass(struct slang_pipeline *p, struct pass_state *ps,
                      const struct slangp_pass *ppass,
                      uint32_t out_w, uint32_t out_h,
                      VkImageView source_view, VkSampler source_sampler,
                      char **err_out)
{
    ps->out_w = out_w;
    ps->out_h = out_h;

    /* Pass framebuffer image. */
    const VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
    if (create_image(p, out_w, out_h, FMT,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     &ps->out_img, &ps->out_mem, err_out) != 0) goto fail;
    ps->out_view = create_view(p->dev, ps->out_img, FMT);
    if (!ps->out_view) { if (err_out) *err_out = xstrdup("vkCreateImageView (pass out)"); goto fail; }

    /* Sampler with the per-pass filter/wrap config. */
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (ppass && ppass->filter_linear) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (ppass && ppass->filter_linear) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = wrap_to_vk(ppass ? ppass->wrap_mode : SLANGP_WRAP_CLAMP_TO_EDGE),
        .addressModeV = wrap_to_vk(ppass ? ppass->wrap_mode : SLANGP_WRAP_CLAMP_TO_EDGE),
        .addressModeW = wrap_to_vk(ppass ? ppass->wrap_mode : SLANGP_WRAP_CLAMP_TO_EDGE),
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    VK_CHECK(vkCreateSampler(p->dev, &sci, NULL, &ps->sampler), "vkCreateSampler (pass)");
    (void)source_sampler;  /* current pass uses its OWN sampler for upstream
                              reads; the previous pass's sampler is used by
                              its OWN pass when it was the producer. */

    /* Render pass. */
    VkAttachmentDescription att = {
        .format = FMT, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        /* Final layout is SHADER_READ_ONLY so the next pass can sample.
         * For the very last pass we still keep this; the readback path
         * issues a layout transition to TRANSFER_SRC explicitly. */
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

    /* Framebuffer. */
    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = ps->render_pass,
        .attachmentCount = 1, .pAttachments = &ps->out_view,
        .width = out_w, .height = out_h, .layers = 1,
    };
    VK_CHECK(vkCreateFramebuffer(p->dev, &fbci, NULL, &ps->framebuffer),
             "vkCreateFramebuffer (pass)");

    /* Descriptor set layout: UBO@0 + Source@2 (slang convention). */
    VkDescriptorSetLayoutBinding bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dlci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(p->dev, &dlci, NULL, &ps->dset_layout),
             "vkCreateDescriptorSetLayout (pass)");

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0, .size = sizeof(struct slang_push),
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &ps->dset_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
    };
    VK_CHECK(vkCreatePipelineLayout(p->dev, &plci, NULL, &ps->pipe_layout),
             "vkCreatePipelineLayout (pass)");

    /* Pipeline. */
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
    VkViewport vp = { 0, 0, (float)out_w, (float)out_h, 0, 1 };
    VkRect2D sc = { {0,0}, { out_w, out_h } };
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
        if (err_out) *err_out = err_fmt("vkCreateGraphicsPipelines (pass) VkResult=%d", vr);
        goto fail;
    }

    /* Descriptor set + writes. */
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p->dpool, .descriptorSetCount = 1,
        .pSetLayouts = &ps->dset_layout,
    };
    VK_CHECK(vkAllocateDescriptorSets(p->dev, &dsai, &ps->dset),
             "vkAllocateDescriptorSets (pass)");

    VkDescriptorBufferInfo dbi = { .buffer = p->ubo, .offset = 0, .range = sizeof(struct slang_ubo) };
    VkDescriptorImageInfo  dii = {
        .sampler = ps->sampler, .imageView = source_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ps->dset,
          .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ps->dset,
          .dstBinding = 2, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii },
    };
    vkUpdateDescriptorSets(p->dev, 2, writes, 0, NULL);
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
    struct slang_pipeline *p = (struct slang_pipeline *)calloc(1, sizeof(*p));
    if (!p) { if (err_out) *err_out = xstrdup("oom"); return NULL; }
    p->input_w = input_w;   p->input_h  = input_h;
    p->output_w = output_w; p->output_h = output_h;

    if (init_instance_and_device(p, err_out) != 0) goto fail;

    /* Shared resources: input image + sampler, staging buffers, vertex/index
     * buffers, UBO, descriptor pool. */
    const VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
    if (create_image(p, input_w, input_h, FMT,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
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
    if (create_buffer(p, rbytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &p->stg_readback, &p->stg_readback_mem, &p->stg_readback_ptr, err_out) != 0) goto fail;

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

    /* Compile each pass's shader and build its pipeline. */
    uint32_t prev_w = input_w, prev_h = input_h;
    VkImageView prev_view    = p->in_view;
    VkSampler   prev_sampler = p->in_sampler;
    for (size_t i = 0; i < n_passes; ++i) {
        struct pass_state *ps = &p->passes[i];
        const struct slangp_pass *ppass = (preset && preset->num_passes > 0)
                                          ? &preset->passes[i] : NULL;

        /* Compile this pass's shader (or built-in for the empty preset). */
        char *cerr = NULL;
        if (ppass && ppass->path) {
            ps->mod = slang_compile_file(ppass->path, &cerr);
        } else {
            ps->mod = slang_compile_string(BUILTIN_SLANG, NULL, &cerr);
        }
        if (!ps->mod) {
            if (err_out) *err_out = err_fmt("compiling pass %zu (%s) failed: %s",
                                            i, (ppass && ppass->path) ? ppass->path : "<built-in>",
                                            cerr ? cerr : "(unknown)");
            free(cerr);
            goto fail;
        }

        /* Resolve dims. The final pass is forced to output_w x output_h so
         * the readback buffer matches. (The slangp `viewport` scale type
         * usually achieves this naturally; we coerce as a safety net.) */
        uint32_t pw, ph;
        if (ppass) resolve_pass_dims(ppass, prev_w, prev_h, output_w, output_h, &pw, &ph);
        else       { pw = output_w; ph = output_h; }
        if (i == n_passes - 1) { pw = output_w; ph = output_h; }

        if (build_pass(p, ps, ppass, pw, ph, prev_view, prev_sampler, err_out) != 0) goto fail;

        prev_w = pw; prev_h = ph;
        prev_view    = ps->out_view;
        prev_sampler = ps->sampler;
    }

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
fail:
    slang_pipeline_destroy(p);
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Per-frame dispatch                                                         */
/* -------------------------------------------------------------------------- */

static void barrier(VkCommandBuffer cmd, VkImage img,
                    VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags src_s, VkPipelineStageFlags dst_s,
                    VkAccessFlags src_a, VkAccessFlags dst_a)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_a, .dstAccessMask = dst_a,
        .oldLayout = from, .newLayout = to, .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    };
    vkCmdPipelineBarrier(cmd, src_s, dst_s, 0, 0, NULL, 0, NULL, 1, &b);
}

int slang_pipeline_run(struct slang_pipeline *p, const uint8_t *src, uint8_t *dst)
{
    if (!p || !src || !dst) return -1;

    /* 1. Upload input. */
    size_t ubytes = (size_t)p->input_w * p->input_h * 4;
    memcpy(p->stg_upload_ptr, src, ubytes);

    /* 2. Push constants block (same SourceSize/OutputSize/FrameCount applied
     *    to all passes for now; per-pass dim-specific values are populated
     *    lazily inside the loop). */
    struct slang_push pc = {0};
    pc.original_size[0] = (float)p->input_w;
    pc.original_size[1] = (float)p->input_h;
    pc.original_size[2] = 1.0f / (float)p->input_w;
    pc.original_size[3] = 1.0f / (float)p->input_h;
    pc.frame_count      = p->frame_count++;

    /* 3. Record + submit. */
    vkResetCommandBuffer(p->cmd, 0);
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(p->cmd, &cbbi) != VK_SUCCESS) return -2;

    /* Upload to in_img. */
    barrier(p->cmd, p->in_img,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy bic = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { p->input_w, p->input_h, 1 },
    };
    vkCmdCopyBufferToImage(p->cmd, p->stg_upload, p->in_img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    barrier(p->cmd, p->in_img,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

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

        /* Build a push-constant blob honoring this shader's reflected layout.
         * For each declared push field we look up its name and write the
         * matching standard-field bytes at the right offset. Fields the
         * shader didn't declare are simply not written. After the standard
         * fields, every #pragma parameter default is written at its
         * resolved offset. */
        memset(push_blob, 0, sizeof(push_blob));
        push_size_used = 0;
        if (ps->mod && ps->mod->num_push_fields > 0) {
            for (size_t f = 0; f < ps->mod->num_push_fields; ++f) {
                const struct slang_push_field *pf = &ps->mod->push_fields[f];
                if (!pf->name || pf->offset >= sizeof(push_blob)) continue;
                if (pf->offset > push_size_used) push_size_used = pf->offset;
                if (!strcmp(pf->name, "SourceSize")) {
                    if (pf->offset + 16 <= sizeof(push_blob))
                        memcpy(push_blob + pf->offset, pc.source_size, 16);
                    push_size_used = pf->offset + 16;
                } else if (!strcmp(pf->name, "OriginalSize")) {
                    if (pf->offset + 16 <= sizeof(push_blob))
                        memcpy(push_blob + pf->offset, pc.original_size, 16);
                    push_size_used = pf->offset + 16;
                } else if (!strcmp(pf->name, "OutputSize")) {
                    if (pf->offset + 16 <= sizeof(push_blob))
                        memcpy(push_blob + pf->offset, pc.output_size, 16);
                    push_size_used = pf->offset + 16;
                } else if (!strcmp(pf->name, "FinalViewportSize")) {
                    if (pf->offset + 16 <= sizeof(push_blob))
                        memcpy(push_blob + pf->offset, pc.output_size, 16);
                    push_size_used = pf->offset + 16;
                } else if (!strcmp(pf->name, "FrameCount")) {
                    if (pf->offset + 4 <= sizeof(push_blob))
                        memcpy(push_blob + pf->offset, &pc.frame_count, 4);
                    push_size_used = pf->offset + 4;
                } else if (!strcmp(pf->name, "FrameDirection")) {
                    int32_t dir = 1;
                    if (pf->offset + 4 <= sizeof(push_blob))
                        memcpy(push_blob + pf->offset, &dir, 4);
                    push_size_used = pf->offset + 4;
                } else if (!strcmp(pf->name, "Rotation")) {
                    uint32_t rot = 0;
                    if (pf->offset + 4 <= sizeof(push_blob))
                        memcpy(push_blob + pf->offset, &rot, 4);
                    push_size_used = pf->offset + 4;
                }
            }
            /* Apply each #pragma parameter default at its resolved offset. */
            for (size_t f = 0; f < ps->mod->num_params; ++f) {
                const struct slang_param *pp = &ps->mod->params[f];
                if (pp->push_offset == 0 && pp->name) {
                    /* unresolved (no matching push field name) — skip */
                    continue;
                }
                if (pp->push_offset + 4 > sizeof(push_blob)) continue;
                memcpy(push_blob + pp->push_offset, &pp->default_value, 4);
                if (pp->push_offset + 4 > push_size_used)
                    push_size_used = pp->push_offset + 4;
            }
        } else {
            /* No reflection (e.g. built-in passthrough): use the legacy
             * fixed slang_push struct exactly. */
            memcpy(push_blob, &pc, sizeof(pc));
            push_size_used = sizeof(pc);
        }
        /* Round up to 4 (Vulkan requires 4-byte aligned push range). */
        push_size_used = (push_size_used + 3) & ~3u;
        if (push_size_used == 0) push_size_used = 4;
        if (push_size_used > sizeof(push_blob)) push_size_used = sizeof(push_blob);

        VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 1 } } };
        VkRenderPassBeginInfo rpbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = ps->render_pass, .framebuffer = ps->framebuffer,
            .renderArea = { {0,0}, { ps->out_w, ps->out_h } },
            .clearValueCount = 1, .pClearValues = &clear,
        };
        vkCmdBeginRenderPass(p->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(p->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ps->pipeline);
        vkCmdBindDescriptorSets(p->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ps->pipe_layout, 0, 1, &ps->dset, 0, NULL);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(p->cmd, 0, 1, &p->vbuf, &off);
        vkCmdBindIndexBuffer(p->cmd, p->ibuf, 0, VK_INDEX_TYPE_UINT16);
        vkCmdPushConstants(p->cmd, ps->pipe_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, push_size_used, push_blob);
        vkCmdDrawIndexed(p->cmd, 6, 1, 0, 0, 0);
        vkCmdEndRenderPass(p->cmd);

        /* Render pass leaves out_img in SHADER_READ_ONLY (next pass's
         * Source binding can sample from it directly). */
        prev_w = ps->out_w; prev_h = ps->out_h;
    }

    /* Last pass output: SHADER_READ_ONLY -> TRANSFER_SRC, copy, restore. */
    struct pass_state *last = &p->passes[p->num_passes - 1];
    barrier(p->cmd, last->out_img,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy bic2 = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { last->out_w, last->out_h, 1 },
    };
    vkCmdCopyImageToBuffer(p->cmd, last->out_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           p->stg_readback, 1, &bic2);
    barrier(p->cmd, last->out_img,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, 0);

    if (vkEndCommandBuffer(p->cmd) != VK_SUCCESS) return -3;

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &p->cmd,
    };
    vkResetFences(p->dev, 1, &p->fence);
    if (vkQueueSubmit(p->queue, 1, &si, p->fence) != VK_SUCCESS) return -4;
    vkWaitForFences(p->dev, 1, &p->fence, VK_TRUE, UINT64_MAX);

    /* 4. Read back. */
    size_t rbytes = (size_t)p->output_w * p->output_h * 4;
    memcpy(dst, p->stg_readback_ptr, rbytes);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Teardown                                                                   */
/* -------------------------------------------------------------------------- */

static void destroy_pass(VkDevice dev, struct pass_state *ps)
{
    if (ps->pipeline)     vkDestroyPipeline(dev, ps->pipeline, NULL);
    if (ps->pipe_layout)  vkDestroyPipelineLayout(dev, ps->pipe_layout, NULL);
    if (ps->dset_layout)  vkDestroyDescriptorSetLayout(dev, ps->dset_layout, NULL);
    if (ps->framebuffer)  vkDestroyFramebuffer(dev, ps->framebuffer, NULL);
    if (ps->render_pass)  vkDestroyRenderPass(dev, ps->render_pass, NULL);
    if (ps->sampler)      vkDestroySampler(dev, ps->sampler, NULL);
    if (ps->out_view)     vkDestroyImageView(dev, ps->out_view, NULL);
    if (ps->out_img)      vkDestroyImage(dev, ps->out_img, NULL);
    if (ps->out_mem)      vkFreeMemory(dev, ps->out_mem, NULL);
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

    if (p->dev)          vkDestroyDevice(p->dev, NULL);
    if (p->instance)     vkDestroyInstance(p->instance, NULL);

    free(p);
}
