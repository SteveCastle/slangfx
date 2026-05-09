/*
 * vf-slang — Vulkan offscreen render pipeline.
 *
 * Phase 1: bring up VkInstance/VkDevice/queue + offscreen render-to-texture.
 * Phase 4: drive the pipeline with a slang shader.
 *
 * The pipeline is set up to the libretro slang binding contract:
 *   set=0, binding=0   UBO (mat4 MVP and any UBO scalars from the shader)
 *   set=0, binding=2   sampler2D Source (current pass input)
 *   push_constant      Push { vec4 SourceSize, OriginalSize, OutputSize;
 *                              uint FrameCount; ...params... }
 *   vertex inputs      location 0 = vec4 Position, location 1 = vec2 TexCoord
 *
 * Per-frame:
 *   1. memcpy host RGBA into upload staging buffer
 *   2. record commands: layout barriers + image upload + render pass +
 *      image -> readback staging copy
 *   3. submit, wait fence
 *   4. memcpy readback staging -> host RGBA
 *
 * Phase 5/6 (multi-pass + PassFeedback) extend the per-frame block to
 * iterate over passes, manage a chain of intermediate framebuffers, and
 * flip ring-buffer slots for the feedback samplers.
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

/* Built-in slang shader used when no preset / empty preset is given.
 * Identical math to passthrough, but written in slang convention so the
 * pipeline's bindings/layout don't need a separate code path. */
static const char *BUILTIN_SLANG =
    "#version 450\n"
    "layout(std140, set = 0, binding = 0) uniform UBO {\n"
    "    mat4 MVP;\n"
    "} global;\n"
    "layout(push_constant) uniform Push {\n"
    "    vec4 SourceSize;\n"
    "    vec4 OriginalSize;\n"
    "    vec4 OutputSize;\n"
    "    uint FrameCount;\n"
    "} params;\n"
    "\n"
    "#pragma stage vertex\n"
    "layout(location = 0) in vec4 Position;\n"
    "layout(location = 1) in vec2 TexCoord;\n"
    "layout(location = 0) out vec2 vUV;\n"
    "void main() {\n"
    "    gl_Position = global.MVP * Position;\n"
    "    vUV = TexCoord;\n"
    "}\n"
    "\n"
    "#pragma stage fragment\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 0) out vec4 FragColor;\n"
    "layout(set = 0, binding = 2) uniform sampler2D Source;\n"
    "void main() {\n"
    "    FragColor = texture(Source, vUV);\n"
    "}\n";

/* Standard push-constant header used to drive shaders in Phase 4. The full
 * 128-byte layout matches the slang convention's first 52 bytes; everything
 * past offset 52 is reserved for shader-declared parameters and will be
 * populated once Phase 7 reflection lands. */
struct slang_push {
    float    source_size  [4];   /* offset 0:  (w, h, 1/w, 1/h) of Source */
    float    original_size[4];   /* offset 16 */
    float    output_size  [4];   /* offset 32 */
    uint32_t frame_count;        /* offset 48 */
    uint32_t _pad0;              /* offset 52 — keep struct 16-byte aligned */
    float    reserved     [16];  /* parameter slots, all zero in Phase 4 */
};

struct slang_ubo {
    float MVP[16];               /* identity in Phase 4 */
};

/* Vertex layout matches slang's expected attributes:
 *   location 0 = vec4 Position (NDC-space xyzw)
 *   location 1 = vec2 TexCoord */
struct vertex {
    float pos[4];
    float uv[2];
};

/* Fullscreen quad. NDC origin is center; Vulkan NDC has +y down so vertex
 * order picks UV (0,0) at NDC (-1,-1) which in Vulkan is the top-left of
 * the framebuffer. */
static const struct vertex QUAD_VERTS[4] = {
    { { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
    { {  1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
    { {  1.0f,  1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
    { { -1.0f,  1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
};
static const uint16_t QUAD_INDICES[6] = { 0, 1, 2,  0, 2, 3 };

/* -------------------------------------------------------------------------- */
/* Pipeline state                                                             */
/* -------------------------------------------------------------------------- */

struct slang_pipeline {
    /* Dimensions. */
    unsigned input_w, input_h;
    unsigned output_w, output_h;

    /* Vulkan core. */
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         dev;
    uint32_t         queue_family;
    VkQueue          queue;
    VkPhysicalDeviceMemoryProperties mem_props;

    /* Command pool / buffer / fence. */
    VkCommandPool   cmd_pool;
    VkCommandBuffer cmd;
    VkFence         fence;

    /* Input image (sampled) and output image (color attachment). */
    VkImage        in_img;
    VkDeviceMemory in_mem;
    VkImageView    in_view;
    VkSampler      in_sampler;
    VkImage        out_img;
    VkDeviceMemory out_mem;
    VkImageView    out_view;

    /* Staging buffers. */
    VkBuffer       stg_upload;
    VkDeviceMemory stg_upload_mem;
    void          *stg_upload_ptr;
    VkBuffer       stg_readback;
    VkDeviceMemory stg_readback_mem;
    void          *stg_readback_ptr;

    /* Vertex / index buffers (fullscreen quad). */
    VkBuffer       vbuf;
    VkDeviceMemory vbuf_mem;
    VkBuffer       ibuf;
    VkDeviceMemory ibuf_mem;

    /* UBO buffer (mat4 MVP — host-visible so we can write the identity at
     * setup; never changes). */
    VkBuffer       ubo;
    VkDeviceMemory ubo_mem;
    void          *ubo_ptr;

    /* Render pass + framebuffer. */
    VkRenderPass  render_pass;
    VkFramebuffer framebuffer;

    /* Pipeline state. */
    VkDescriptorSetLayout dset_layout;
    VkPipelineLayout      pipe_layout;
    VkPipeline            pipeline;
    VkDescriptorPool      dpool;
    VkDescriptorSet       dset;

    /* Frame counter for the FrameCount push constant. */
    uint32_t frame_count;

    /* Owning ref to the compiled slang module so its SPIR-V outlives the
     * VkShaderModules it spawned. (The VkShaderModules are destroyed after
     * pipeline creation — only the SPIR-V bytecode in `mod` persists.) */
    struct slang_module *mod;
};

/* -------------------------------------------------------------------------- */
/* Vulkan helpers                                                             */
/* -------------------------------------------------------------------------- */

static uint32_t find_memtype(const VkPhysicalDeviceMemoryProperties *mp,
                             uint32_t req, VkMemoryPropertyFlags props)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; ++i)
        if ((req & (1u << i)) &&
            (mp->memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

static int init_instance_and_device(struct slang_pipeline *p, char **err_out)
{
    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vfslang", .applicationVersion = 1,
        .pEngineName      = "vfslang", .engineVersion      = 1,
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai,
    };
    VK_CHECK(vkCreateInstance(&ici, NULL, &p->instance), "vkCreateInstance");

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(p->instance, &n, NULL);
    if (n == 0) { if (err_out) *err_out = xstrdup("no Vulkan physical devices"); goto fail; }
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    vkEnumeratePhysicalDevices(p->instance, &n, devs);
    p->phys = devs[0];
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(devs[i], &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { p->phys = devs[i]; break; }
    }

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p->phys, &qn, NULL);
    VkQueueFamilyProperties qp[16]; if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(p->phys, &qn, qp);
    p->queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qn; ++i)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { p->queue_family = i; break; }
    if (p->queue_family == UINT32_MAX) {
        if (err_out) *err_out = xstrdup("no graphics queue family"); goto fail;
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
fail:
    return -1;
}

static int create_image(struct slang_pipeline *p, uint32_t w, uint32_t h,
                        VkFormat fmt, VkImageUsageFlags usage,
                        VkImage *img_out, VkDeviceMemory *mem_out, char **err_out)
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
    if (mt == UINT32_MAX) { if (err_out) *err_out = xstrdup("no device-local memory for image"); goto fail; }

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
                         VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags props,
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
    if (mt == UINT32_MAX) { if (err_out) *err_out = xstrdup("no compatible memory for buffer"); goto fail; }
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
        .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(d, &ivci, NULL, &v);
    return v;
}

static int create_render_pass(struct slang_pipeline *p, VkFormat fmt, char **err_out)
{
    VkAttachmentDescription att = {
        .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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
    VK_CHECK(vkCreateRenderPass(p->dev, &rpci, NULL, &p->render_pass), "vkCreateRenderPass");
    return 0;
fail: return -1;
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

/* -------------------------------------------------------------------------- */
/* Pipeline construction (slang convention)                                   */
/* -------------------------------------------------------------------------- */

static int create_pipeline(struct slang_pipeline *p, char **err_out)
{
    /* Descriptor set layout: UBO @ binding 0 + sampler @ binding 2.
     * (Binding 1 is unused — slang reserves it; we leave a gap rather than
     * collapse, so shaders can be built unmodified.) */
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
    VK_CHECK(vkCreateDescriptorSetLayout(p->dev, &dlci, NULL, &p->dset_layout),
             "vkCreateDescriptorSetLayout");

    /* Pipeline layout: descriptor set + push constant range covering the
     * entire slang_push struct (visible to both stages). */
    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0, .size = sizeof(struct slang_push),
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &p->dset_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
    };
    VK_CHECK(vkCreatePipelineLayout(p->dev, &plci, NULL, &p->pipe_layout),
             "vkCreatePipelineLayout");

    /* Shader modules from the slang module's SPIR-V. */
    VkShaderModule vmod = create_shader(p->dev, p->mod->vert_spv, p->mod->vert_spv_words);
    VkShaderModule fmod = create_shader(p->dev, p->mod->frag_spv, p->mod->frag_spv_words);
    if (!vmod || !fmod) {
        if (err_out) *err_out = xstrdup("vkCreateShaderModule failed");
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

    /* Vertex input state. */
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
    VkViewport vp = { 0, 0, (float)p->output_w, (float)p->output_h, 0, 1 };
    VkRect2D sc = { {0,0}, { p->output_w, p->output_h } };
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
        .pVertexInputState   = &vis,
        .pInputAssemblyState = &ias,
        .pViewportState      = &vps,
        .pRasterizationState = &rs,
        .pMultisampleState   = &ms,
        .pColorBlendState    = &cbs,
        .layout = p->pipe_layout,
        .renderPass = p->render_pass, .subpass = 0,
    };
    VkResult vr = vkCreateGraphicsPipelines(p->dev, VK_NULL_HANDLE, 1, &gpci, NULL, &p->pipeline);
    vkDestroyShaderModule(p->dev, vmod, NULL);
    vkDestroyShaderModule(p->dev, fmod, NULL);
    if (vr != VK_SUCCESS) {
        if (err_out) *err_out = err_fmt("vkCreateGraphicsPipelines (VkResult=%d)", vr);
        goto fail;
    }
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

    /* Compile the shader: prefer the preset's first pass; fall back to the
     * built-in passthrough so the binary is always invocable end-to-end. */
    if (preset && preset->num_passes > 0 && preset->passes[0].path) {
        char *cerr = NULL;
        p->mod = slang_compile_file(preset->passes[0].path, &cerr);
        if (!p->mod) {
            if (err_out) *err_out = err_fmt("compiling '%s' failed: %s",
                                            preset->passes[0].path,
                                            cerr ? cerr : "(unknown)");
            free(cerr);
            goto fail;
        }
    } else {
        p->mod = slang_compile_string(BUILTIN_SLANG, NULL, err_out);
        if (!p->mod) goto fail;
    }

    if (init_instance_and_device(p, err_out) != 0) goto fail;

    /* Pass 1 of multi-pass support: 5 phases away. For now images at I/O
     * size; intermediate framebuffers added in Phase 5. */
    const VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
    if (create_image(p, input_w, input_h, FMT,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     &p->in_img, &p->in_mem, err_out) != 0) goto fail;
    if (create_image(p, output_w, output_h, FMT,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     &p->out_img, &p->out_mem, err_out) != 0) goto fail;
    p->in_view  = create_view(p->dev, p->in_img,  FMT);
    p->out_view = create_view(p->dev, p->out_img, FMT);

    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .unnormalizedCoordinates = VK_FALSE,
    };
    VK_CHECK(vkCreateSampler(p->dev, &sci, NULL, &p->in_sampler), "vkCreateSampler");

    /* Staging buffers. */
    VkDeviceSize ubytes = (VkDeviceSize)input_w  * input_h  * 4;
    VkDeviceSize rbytes = (VkDeviceSize)output_w * output_h * 4;
    if (create_buffer(p, ubytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &p->stg_upload, &p->stg_upload_mem, &p->stg_upload_ptr, err_out) != 0) goto fail;
    if (create_buffer(p, rbytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &p->stg_readback, &p->stg_readback_mem, &p->stg_readback_ptr, err_out) != 0) goto fail;

    /* Vertex + index buffers. Both DEVICE_LOCAL would be ideal; for
     * Phase 4 we use HOST_VISIBLE so we don't need to introduce another
     * staging round-trip just to upload a constant 96-byte quad. */
    if (create_buffer(p, sizeof(QUAD_VERTS),
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &p->vbuf, &p->vbuf_mem, NULL, err_out) != 0) goto fail;
    {
        void *m;
        VK_CHECK(vkMapMemory(p->dev, p->vbuf_mem, 0, sizeof(QUAD_VERTS), 0, &m), "map vbuf");
        memcpy(m, QUAD_VERTS, sizeof(QUAD_VERTS));
        vkUnmapMemory(p->dev, p->vbuf_mem);
    }
    if (create_buffer(p, sizeof(QUAD_INDICES),
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &p->ibuf, &p->ibuf_mem, NULL, err_out) != 0) goto fail;
    {
        void *m;
        VK_CHECK(vkMapMemory(p->dev, p->ibuf_mem, 0, sizeof(QUAD_INDICES), 0, &m), "map ibuf");
        memcpy(m, QUAD_INDICES, sizeof(QUAD_INDICES));
        vkUnmapMemory(p->dev, p->ibuf_mem);
    }

    /* UBO with identity MVP. */
    if (create_buffer(p, sizeof(struct slang_ubo),
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &p->ubo, &p->ubo_mem, &p->ubo_ptr, err_out) != 0) goto fail;
    {
        struct slang_ubo u = {0};
        u.MVP[0] = 1.0f; u.MVP[5]  = 1.0f;
        u.MVP[10] = 1.0f; u.MVP[15] = 1.0f;
        memcpy(p->ubo_ptr, &u, sizeof(u));
    }

    /* Render pass + framebuffer. */
    if (create_render_pass(p, FMT, err_out) != 0) goto fail;
    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = p->render_pass,
        .attachmentCount = 1, .pAttachments = &p->out_view,
        .width = output_w, .height = output_h, .layers = 1,
    };
    VK_CHECK(vkCreateFramebuffer(p->dev, &fbci, NULL, &p->framebuffer), "vkCreateFramebuffer");

    if (create_pipeline(p, err_out) != 0) goto fail;

    /* Descriptor pool/set + writes. */
    VkDescriptorPoolSize ps[2] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps,
    };
    VK_CHECK(vkCreateDescriptorPool(p->dev, &dpci, NULL, &p->dpool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p->dpool, .descriptorSetCount = 1,
        .pSetLayouts = &p->dset_layout,
    };
    VK_CHECK(vkAllocateDescriptorSets(p->dev, &dsai, &p->dset), "vkAllocateDescriptorSets");

    VkDescriptorBufferInfo dbi = { .buffer = p->ubo, .offset = 0, .range = sizeof(struct slang_ubo) };
    VkDescriptorImageInfo  dii = {
        .sampler = p->in_sampler, .imageView = p->in_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->dset,
          .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo = &dbi },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->dset,
          .dstBinding = 2, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &dii },
    };
    vkUpdateDescriptorSets(p->dev, 2, writes, 0, NULL);

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
        .oldLayout = from, .newLayout = to,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    };
    vkCmdPipelineBarrier(cmd, src_s, dst_s, 0, 0, NULL, 0, NULL, 1, &b);
}

int slang_pipeline_run(struct slang_pipeline *p, const uint8_t *src, uint8_t *dst)
{
    if (!p || !src || !dst) return -1;

    /* 1. Upload. */
    size_t ubytes = (size_t)p->input_w * p->input_h * 4;
    memcpy(p->stg_upload_ptr, src, ubytes);

    /* 2. Build the push constant block. */
    struct slang_push pc = {0};
    pc.source_size  [0] = (float)p->input_w;
    pc.source_size  [1] = (float)p->input_h;
    pc.source_size  [2] = 1.0f / (float)p->input_w;
    pc.source_size  [3] = 1.0f / (float)p->input_h;
    pc.original_size[0] = pc.source_size[0];
    pc.original_size[1] = pc.source_size[1];
    pc.original_size[2] = pc.source_size[2];
    pc.original_size[3] = pc.source_size[3];
    pc.output_size  [0] = (float)p->output_w;
    pc.output_size  [1] = (float)p->output_h;
    pc.output_size  [2] = 1.0f / (float)p->output_w;
    pc.output_size  [3] = 1.0f / (float)p->output_h;
    pc.frame_count      = p->frame_count++;

    /* 3. Record + submit. */
    vkResetCommandBuffer(p->cmd, 0);
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(p->cmd, &cbbi) != VK_SUCCESS) return -2;

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

    VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 1 } } };
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = p->render_pass, .framebuffer = p->framebuffer,
        .renderArea = { {0,0}, { p->output_w, p->output_h } },
        .clearValueCount = 1, .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(p->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(p->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    vkCmdBindDescriptorSets(p->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p->pipe_layout, 0, 1, &p->dset, 0, NULL);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(p->cmd, 0, 1, &p->vbuf, &off);
    vkCmdBindIndexBuffer(p->cmd, p->ibuf, 0, VK_INDEX_TYPE_UINT16);
    vkCmdPushConstants(p->cmd, p->pipe_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDrawIndexed(p->cmd, 6, 1, 0, 0, 0);
    vkCmdEndRenderPass(p->cmd);

    VkBufferImageCopy bic2 = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { p->output_w, p->output_h, 1 },
    };
    vkCmdCopyImageToBuffer(p->cmd, p->out_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           p->stg_readback, 1, &bic2);
    barrier(p->cmd, p->out_img,
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

void slang_pipeline_destroy(struct slang_pipeline *p)
{
    if (!p) return;
    if (p->dev) vkDeviceWaitIdle(p->dev);

    if (p->fence)        vkDestroyFence(p->dev, p->fence, NULL);
    if (p->cmd_pool)     vkDestroyCommandPool(p->dev, p->cmd_pool, NULL);
    if (p->dpool)        vkDestroyDescriptorPool(p->dev, p->dpool, NULL);
    if (p->pipeline)     vkDestroyPipeline(p->dev, p->pipeline, NULL);
    if (p->pipe_layout)  vkDestroyPipelineLayout(p->dev, p->pipe_layout, NULL);
    if (p->dset_layout)  vkDestroyDescriptorSetLayout(p->dev, p->dset_layout, NULL);
    if (p->framebuffer)  vkDestroyFramebuffer(p->dev, p->framebuffer, NULL);
    if (p->render_pass)  vkDestroyRenderPass(p->dev, p->render_pass, NULL);

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
    if (p->out_view)     vkDestroyImageView(p->dev, p->out_view, NULL);
    if (p->in_img)       vkDestroyImage(p->dev, p->in_img, NULL);
    if (p->out_img)      vkDestroyImage(p->dev, p->out_img, NULL);
    if (p->in_mem)       vkFreeMemory(p->dev, p->in_mem, NULL);
    if (p->out_mem)      vkFreeMemory(p->dev, p->out_mem, NULL);

    if (p->dev)          vkDestroyDevice(p->dev, NULL);
    if (p->instance)     vkDestroyInstance(p->instance, NULL);

    slang_module_free(p->mod);
    free(p);
}
