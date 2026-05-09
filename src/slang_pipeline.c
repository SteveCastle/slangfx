/*
 * vf-slang — Vulkan offscreen render pipeline.
 *
 * Phase 1: brings up an instance + device + queue, creates a single-pass
 * graphics pipeline that samples an input image and writes to an output
 * framebuffer. Per-frame, the host hands in CPU RGBA bytes; we upload via
 * staging buffer, dispatch the pipeline, copy the output to a readback
 * staging buffer, and the host gets RGBA bytes back. The pipeline is
 * fixed at one-shader-pass for now.
 *
 * Phase 4 will swap the hardcoded passthrough fragment shader for the
 * SPIR-V emitted by slang_compile, and Phase 5 expands to multiple passes.
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
    char buf[1024];
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

/* Hardcoded SPIR-V is unwieldy; we let shaderc compile our built-in
 * pipeline shaders at startup. This keeps the source readable and matches
 * how Phase 4 will compile slang shaders. */
#include <shaderc/shaderc.h>

static int compile_glsl(shaderc_shader_kind kind, const char *src,
                        uint32_t **spv_out, size_t *spv_words_out,
                        char **err_out)
{
    shaderc_compiler_t  c = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan,
                                           shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_target_spirv(o, shaderc_spirv_version_1_5);

    shaderc_compilation_result_t r = shaderc_compile_into_spv(
        c, src, strlen(src), kind, "<built-in>", "main", o);

    int rc = -1;
    if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success) {
        size_t bytes = shaderc_result_get_length(r);
        uint32_t *out = (uint32_t *)malloc(bytes);
        if (out) {
            memcpy(out, shaderc_result_get_bytes(r), bytes);
            *spv_out = out;
            *spv_words_out = bytes / 4;
            rc = 0;
        } else if (err_out) {
            *err_out = xstrdup("oom (built-in shader spirv)");
        }
    } else if (err_out) {
        const char *m = shaderc_result_get_error_message(r);
        *err_out = err_fmt("built-in shader compile failed: %s", m ? m : "(no message)");
    }

    shaderc_result_release(r);
    shaderc_compile_options_release(o);
    shaderc_compiler_release(c);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* Pipeline state                                                             */
/* -------------------------------------------------------------------------- */

struct slang_pipeline {
    /* Dimensions */
    unsigned input_w, input_h;
    unsigned output_w, output_h;

    /* Vulkan instance / device */
    VkInstance        instance;
    VkPhysicalDevice  phys;
    VkDevice          dev;
    uint32_t          queue_family;
    VkQueue           queue;

    /* Memory properties cache */
    VkPhysicalDeviceMemoryProperties mem_props;

    /* Command pool / buffer */
    VkCommandPool   cmd_pool;
    VkCommandBuffer cmd;

    /* Input image (sampled) and output image (color attachment) */
    VkImage        in_img;
    VkDeviceMemory in_mem;
    VkImageView    in_view;
    VkSampler      in_sampler;

    VkImage        out_img;
    VkDeviceMemory out_mem;
    VkImageView    out_view;

    /* Staging buffers for CPU<->GPU transfer */
    VkBuffer       stg_upload;
    VkDeviceMemory stg_upload_mem;
    void          *stg_upload_ptr;

    VkBuffer       stg_readback;
    VkDeviceMemory stg_readback_mem;
    void          *stg_readback_ptr;

    /* Render pass + framebuffer */
    VkRenderPass  render_pass;
    VkFramebuffer framebuffer;

    /* Pipeline */
    VkDescriptorSetLayout dset_layout;
    VkPipelineLayout      pipe_layout;
    VkPipeline            pipeline;

    VkDescriptorPool dpool;
    VkDescriptorSet  dset;

    /* Synchronization */
    VkFence  fence;

    /* SPIR-V */
    uint32_t *vert_spv;     size_t vert_spv_words;
    uint32_t *frag_spv;     size_t frag_spv_words;
};

/* Default built-in shaders (Phase 1). Vertex emits a fullscreen triangle
 * with NDC positions and a UV that covers [0,1]. Fragment samples the
 * input texture and writes through. Phase 4 swaps the fragment shader
 * (and possibly the vertex shader) with slang-compiled SPIR-V. */
static const char *BUILTIN_VERT_GLSL =
    "#version 450\n"
    "layout(location = 0) out vec2 vUV;\n"
    "void main() {\n"
    /*  large triangle that covers the screen; UVs span [0,1] over the
     *  visible region. */
    "    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "    vUV  = pos;\n"
    "    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *BUILTIN_FRAG_GLSL =
    "#version 450\n"
    "layout(location = 0) in  vec2 vUV;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "layout(set = 0, binding = 0) uniform sampler2D Source;\n"
    "void main() {\n"
    "    oColor = texture(Source, vUV);\n"
    "}\n";

/* Find a memory type compatible with `req` bits and `props` flags.
 * Returns UINT32_MAX on failure. */
static uint32_t find_memtype(const VkPhysicalDeviceMemoryProperties *mp,
                             uint32_t req, VkMemoryPropertyFlags props)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; ++i) {
        if ((req & (1u << i)) &&
            (mp->memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

/* -------------------------------------------------------------------------- */
/* Init: instance, device, queue                                              */
/* -------------------------------------------------------------------------- */

static int init_instance_and_device(struct slang_pipeline *p, char **err_out)
{
    /* Instance — no extensions needed for offscreen; validation layers can
     * be enabled via VK_INSTANCE_LAYERS env in development. */
    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "vfslang",
        .applicationVersion = 1,
        .pEngineName        = "vfslang",
        .engineVersion      = 1,
        .apiVersion         = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai,
    };
    VK_CHECK(vkCreateInstance(&ici, NULL, &p->instance), "vkCreateInstance");

    /* Pick first physical device (prefer discrete). */
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

    /* Find a queue family that supports graphics. */
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p->phys, &qn, NULL);
    VkQueueFamilyProperties qprops[16];
    if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(p->phys, &qn, qprops);
    p->queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qn; ++i) {
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { p->queue_family = i; break; }
    }
    if (p->queue_family == UINT32_MAX) {
        if (err_out) *err_out = xstrdup("no graphics queue family");
        goto fail;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = p->queue_family,
        .queueCount       = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos    = &qci,
    };
    VK_CHECK(vkCreateDevice(p->phys, &dci, NULL, &p->dev), "vkCreateDevice");
    vkGetDeviceQueue(p->dev, p->queue_family, 0, &p->queue);
    vkGetPhysicalDeviceMemoryProperties(p->phys, &p->mem_props);
    return 0;
fail:
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Image + buffer creation helpers                                            */
/* -------------------------------------------------------------------------- */

static int create_image(struct slang_pipeline *p, uint32_t w, uint32_t h,
                        VkFormat fmt, VkImageUsageFlags usage,
                        VkImage *img_out, VkDeviceMemory *mem_out,
                        char **err_out)
{
    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = fmt,
        .extent        = { w, h, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(p->dev, &ici, NULL, img_out), "vkCreateImage");

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(p->dev, *img_out, &mr);
    uint32_t mt = find_memtype(&p->mem_props, mr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) {
        if (err_out) *err_out = xstrdup("no DEVICE_LOCAL memory type for image");
        goto fail;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mr.size,
        .memoryTypeIndex = mt,
    };
    VK_CHECK(vkAllocateMemory(p->dev, &mai, NULL, mem_out), "vkAllocateMemory(image)");
    VK_CHECK(vkBindImageMemory(p->dev, *img_out, *mem_out, 0), "vkBindImageMemory");
    return 0;
fail:
    return -1;
}

static int create_buffer(struct slang_pipeline *p, VkDeviceSize size,
                         VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags props,
                         VkBuffer *buf_out, VkDeviceMemory *mem_out,
                         void **mapped_out,
                         char **err_out)
{
    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(p->dev, &bci, NULL, buf_out), "vkCreateBuffer");

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(p->dev, *buf_out, &mr);
    uint32_t mt = find_memtype(&p->mem_props, mr.memoryTypeBits, props);
    if (mt == UINT32_MAX) {
        if (err_out) *err_out = xstrdup("no compatible memory type for staging buffer");
        goto fail;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mr.size,
        .memoryTypeIndex = mt,
    };
    VK_CHECK(vkAllocateMemory(p->dev, &mai, NULL, mem_out), "vkAllocateMemory(buf)");
    VK_CHECK(vkBindBufferMemory(p->dev, *buf_out, *mem_out, 0), "vkBindBufferMemory");

    if (mapped_out) {
        VK_CHECK(vkMapMemory(p->dev, *mem_out, 0, size, 0, mapped_out), "vkMapMemory");
    }
    return 0;
fail:
    return -1;
}

static VkImageView create_view(VkDevice d, VkImage img, VkFormat fmt)
{
    VkImageViewCreateInfo ivci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = fmt,
        .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(d, &ivci, NULL, &v);
    return v;
}

/* -------------------------------------------------------------------------- */
/* Render pass + pipeline                                                     */
/* -------------------------------------------------------------------------- */

static int create_render_pass(struct slang_pipeline *p, VkFormat fmt, char **err_out)
{
    VkAttachmentDescription att = {
        .format         = fmt,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    VkAttachmentReference cref = {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription sub = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &cref,
    };
    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rpci = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &att,
        .subpassCount    = 1,
        .pSubpasses      = &sub,
        .dependencyCount = 1,
        .pDependencies   = &dep,
    };
    VK_CHECK(vkCreateRenderPass(p->dev, &rpci, NULL, &p->render_pass),
             "vkCreateRenderPass");
    return 0;
fail:
    return -1;
}

static VkShaderModule create_shader(VkDevice d, const uint32_t *spv, size_t words)
{
    VkShaderModuleCreateInfo smci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = words * 4,
        .pCode    = spv,
    };
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(d, &smci, NULL, &m);
    return m;
}

static int create_pipeline(struct slang_pipeline *p, char **err_out)
{
    /* Descriptor set layout: one combined image sampler at binding 0. */
    VkDescriptorSetLayoutBinding b0 = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dlci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &b0,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(p->dev, &dlci, NULL, &p->dset_layout),
             "vkCreateDescriptorSetLayout");

    VkPipelineLayoutCreateInfo plci = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &p->dset_layout,
    };
    VK_CHECK(vkCreatePipelineLayout(p->dev, &plci, NULL, &p->pipe_layout),
             "vkCreatePipelineLayout");

    VkShaderModule vmod = create_shader(p->dev, p->vert_spv, p->vert_spv_words);
    VkShaderModule fmod = create_shader(p->dev, p->frag_spv, p->frag_spv_words);
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

    VkPipelineVertexInputStateCreateInfo vis = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo ias = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport vp = {
        .x = 0, .y = 0,
        .width = (float)p->output_w, .height = (float)p->output_h,
        .minDepth = 0, .maxDepth = 1,
    };
    VkRect2D sc = { .offset = {0,0}, .extent = { p->output_w, p->output_h } };
    VkPipelineViewportStateCreateInfo vps = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp,
        .scissorCount  = 1, .pScissors  = &sc,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cbs = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &blend,
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vis,
        .pInputAssemblyState = &ias,
        .pViewportState      = &vps,
        .pRasterizationState = &rs,
        .pMultisampleState   = &ms,
        .pColorBlendState    = &cbs,
        .layout              = p->pipe_layout,
        .renderPass          = p->render_pass,
        .subpass             = 0,
    };
    VkResult vr = vkCreateGraphicsPipelines(p->dev, VK_NULL_HANDLE, 1, &gpci, NULL, &p->pipeline);
    vkDestroyShaderModule(p->dev, vmod, NULL);
    vkDestroyShaderModule(p->dev, fmod, NULL);
    if (vr != VK_SUCCESS) {
        if (err_out) *err_out = err_fmt("vkCreateGraphicsPipelines (VkResult=%d)", vr);
        goto fail;
    }
    return 0;
fail:
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

struct slang_pipeline *slang_pipeline_create(const struct slangp_preset *preset,
                                             unsigned input_w, unsigned input_h,
                                             unsigned output_w, unsigned output_h,
                                             char **err_out)
{
    /* Phase 1: preset is used only to confirm we have something; the
     * shader itself is the built-in passthrough. Phase 4 will replace
     * the built-in fragment shader with the slang-compiled one. */
    (void)preset;

    struct slang_pipeline *p = (struct slang_pipeline *)calloc(1, sizeof(*p));
    if (!p) { if (err_out) *err_out = xstrdup("oom"); return NULL; }
    p->input_w = input_w;   p->input_h  = input_h;
    p->output_w = output_w; p->output_h = output_h;

    if (init_instance_and_device(p, err_out) != 0) goto fail;

    /* Compile built-in shaders. */
    if (compile_glsl(shaderc_glsl_vertex_shader,   BUILTIN_VERT_GLSL,
                     &p->vert_spv, &p->vert_spv_words, err_out) != 0) goto fail;
    if (compile_glsl(shaderc_glsl_fragment_shader, BUILTIN_FRAG_GLSL,
                     &p->frag_spv, &p->frag_spv_words, err_out) != 0) goto fail;

    /* Images: input (sampled) + output (color attachment + transfer src). */
    const VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
    if (create_image(p, input_w, input_h, FMT,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     &p->in_img, &p->in_mem, err_out) != 0) goto fail;
    if (create_image(p, output_w, output_h, FMT,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     &p->out_img, &p->out_mem, err_out) != 0) goto fail;

    p->in_view  = create_view(p->dev, p->in_img,  FMT);
    p->out_view = create_view(p->dev, p->out_img, FMT);

    /* Sampler. */
    VkSamplerCreateInfo sci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .unnormalizedCoordinates = VK_FALSE,
    };
    VK_CHECK(vkCreateSampler(p->dev, &sci, NULL, &p->in_sampler), "vkCreateSampler");

    /* Staging buffers (host-visible). */
    VkDeviceSize ubytes = (VkDeviceSize)input_w  * input_h  * 4;
    VkDeviceSize rbytes = (VkDeviceSize)output_w * output_h * 4;
    if (create_buffer(p, ubytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &p->stg_upload, &p->stg_upload_mem, &p->stg_upload_ptr, err_out) != 0) goto fail;
    if (create_buffer(p, rbytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &p->stg_readback, &p->stg_readback_mem, &p->stg_readback_ptr, err_out) != 0) goto fail;

    /* Render pass + framebuffer. */
    if (create_render_pass(p, FMT, err_out) != 0) goto fail;
    VkFramebufferCreateInfo fbci = {
        .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass      = p->render_pass,
        .attachmentCount = 1,
        .pAttachments    = &p->out_view,
        .width           = output_w,
        .height          = output_h,
        .layers          = 1,
    };
    VK_CHECK(vkCreateFramebuffer(p->dev, &fbci, NULL, &p->framebuffer), "vkCreateFramebuffer");

    /* Pipeline (descriptor set layout + pipeline layout + graphics pipeline). */
    if (create_pipeline(p, err_out) != 0) goto fail;

    /* Descriptor pool + set + write. */
    VkDescriptorPoolSize ps = {
        .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = 1,
        .poolSizeCount = 1,
        .pPoolSizes    = &ps,
    };
    VK_CHECK(vkCreateDescriptorPool(p->dev, &dpci, NULL, &p->dpool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = p->dpool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &p->dset_layout,
    };
    VK_CHECK(vkAllocateDescriptorSets(p->dev, &dsai, &p->dset), "vkAllocateDescriptorSets");

    VkDescriptorImageInfo dii = {
        .sampler     = p->in_sampler,
        .imageView   = p->in_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet w = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = p->dset,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &dii,
    };
    vkUpdateDescriptorSets(p->dev, 1, &w, 0, NULL);

    /* Command pool + buffer + fence. */
    VkCommandPoolCreateInfo cpci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = p->queue_family,
    };
    VK_CHECK(vkCreateCommandPool(p->dev, &cpci, NULL, &p->cmd_pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = p->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
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
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                    VkAccessFlags src_access, VkAccessFlags dst_access)
{
    VkImageMemoryBarrier b = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask    = src_access,
        .dstAccessMask    = dst_access,
        .oldLayout        = from,
        .newLayout        = to,
        .image            = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

int slang_pipeline_run(struct slang_pipeline *p, const uint8_t *src, uint8_t *dst)
{
    if (!p || !src || !dst) return -1;

    /* 1. Copy into upload staging buffer. */
    size_t ubytes = (size_t)p->input_w * p->input_h * 4;
    memcpy(p->stg_upload_ptr, src, ubytes);

    /* 2. Record + submit command buffer. */
    vkResetCommandBuffer(p->cmd, 0);
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(p->cmd, &cbbi) != VK_SUCCESS) return -2;

    /* Input image: UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY. */
    barrier(p->cmd, p->in_img,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy bic = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent      = { p->input_w, p->input_h, 1 },
    };
    vkCmdCopyBufferToImage(p->cmd, p->stg_upload, p->in_img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

    barrier(p->cmd, p->in_img,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    /* Render pass on output image (UNDEFINED -> ... -> TRANSFER_SRC via finalLayout). */
    VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rpbi = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass      = p->render_pass,
        .framebuffer     = p->framebuffer,
        .renderArea      = { {0,0}, { p->output_w, p->output_h } },
        .clearValueCount = 1,
        .pClearValues    = &clear,
    };
    vkCmdBeginRenderPass(p->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(p->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    vkCmdBindDescriptorSets(p->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p->pipe_layout, 0, 1, &p->dset, 0, NULL);
    vkCmdDraw(p->cmd, 3, 1, 0, 0);    /* fullscreen triangle */
    vkCmdEndRenderPass(p->cmd);

    /* Render pass leaves out_img in TRANSFER_SRC_OPTIMAL via finalLayout. */
    VkBufferImageCopy bic2 = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent      = { p->output_w, p->output_h, 1 },
    };
    vkCmdCopyImageToBuffer(p->cmd, p->out_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           p->stg_readback, 1, &bic2);

    /* Re-transition out_img back to UNDEFINED so the next frame's render
     * pass loadOp=CLEAR doesn't violate layout assumptions. */
    barrier(p->cmd, p->out_img,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, 0);

    if (vkEndCommandBuffer(p->cmd) != VK_SUCCESS) return -3;

    VkSubmitInfo si = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &p->cmd,
    };
    vkResetFences(p->dev, 1, &p->fence);
    if (vkQueueSubmit(p->queue, 1, &si, p->fence) != VK_SUCCESS) return -4;
    vkWaitForFences(p->dev, 1, &p->fence, VK_TRUE, UINT64_MAX);

    /* 3. Read back. */
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

    if (p->stg_upload_mem)   { vkUnmapMemory(p->dev, p->stg_upload_mem);   vkFreeMemory(p->dev, p->stg_upload_mem, NULL); }
    if (p->stg_readback_mem) { vkUnmapMemory(p->dev, p->stg_readback_mem); vkFreeMemory(p->dev, p->stg_readback_mem, NULL); }
    if (p->stg_upload)   vkDestroyBuffer(p->dev, p->stg_upload,   NULL);
    if (p->stg_readback) vkDestroyBuffer(p->dev, p->stg_readback, NULL);

    if (p->in_sampler)   vkDestroySampler(p->dev, p->in_sampler, NULL);
    if (p->in_view)      vkDestroyImageView(p->dev, p->in_view, NULL);
    if (p->out_view)     vkDestroyImageView(p->dev, p->out_view, NULL);
    if (p->in_img)       vkDestroyImage(p->dev, p->in_img, NULL);
    if (p->out_img)      vkDestroyImage(p->dev, p->out_img, NULL);
    if (p->in_mem)       vkFreeMemory(p->dev, p->in_mem, NULL);
    if (p->out_mem)      vkFreeMemory(p->dev, p->out_mem, NULL);

    if (p->dev)          vkDestroyDevice(p->dev, NULL);
    if (p->instance)     vkDestroyInstance(p->instance, NULL);

    free(p->vert_spv);
    free(p->frag_spv);
    free(p);
}
