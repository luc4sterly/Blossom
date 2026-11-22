// Blossom: Vulkan hardware rasterizer for the PICA200. See vk_rasterizer.hpp.
// Structure modeled on Azahar's renderer_vulkan (GPLv3, https://github.com/azahar-emu/azahar).
#include "video_core/renderer_vulkan/vk_rasterizer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "video_core/renderer_vulkan/pica_frag.spv.h"
#include "video_core/renderer_vulkan/pica_vert.spv.h"

namespace VideoVulkan {

bool HwPipelineKey::operator==(const HwPipelineKey& o) const
{
    return memcmp(this, &o, sizeof(*this)) == 0;
}

namespace {

constexpr VkDeviceSize VERTEX_RING_SIZE = 16 * 1024 * 1024;
constexpr VkDeviceSize UNIFORM_RING_SIZE = 4 * 1024 * 1024;
constexpr VkDeviceSize STAGING_CHUNK_SIZE = 8 * 1024 * 1024;
constexpr uint32_t DESCRIPTOR_SETS_PER_FLUSH = 1024;
constexpr size_t TEXTURE_CACHE_LIMIT = 768;
constexpr uint64_t TEXTURE_BYTES_LIMIT = 256ULL * 1024 * 1024;
constexpr VkDeviceSize STAGING_KEEP = 64 * 1024 * 1024;
constexpr VkFormat COLOR_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

void check(VkResult r, const char* what)
{
    if (r != VK_SUCCESS)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s failed (VkResult %d)", what, (int)r);
        throw std::runtime_error(buf);
    }
}

struct Buffer
{
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    VkDeviceSize size = 0;
    VkDeviceSize offset = 0; //Ring/linear allocator position
    bool coherent = true;
    bool cached = false;
};

struct Image
{
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

//A render target kept between sessions: the top and bottom screens (and render-to-texture
//targets) alternate, and recreating the images on every size change cost milliseconds each
struct Target
{
    uint32_t width = 0, height = 0;
    Image color, depth;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    Buffer readback;
    uint64_t tag = 0;     //Caller's id of the memory this target mirrors
    bool valid = false;   //The images hold what the last session with this tag left
    uint64_t last_used = 0;
};

struct Texture
{
    Image image;
    uint32_t width = 0, height = 0;
    uint64_t last_used = 0;
};

struct KeyHash
{
    size_t operator()(const HwPipelineKey& k) const
    {
        uint64_t h = 1469598103934665603ULL;
        const uint8_t* p = (const uint8_t*)&k;
        for (size_t i = 0; i < sizeof(k); i++)
            h = (h ^ p[i]) * 1099511628211ULL;
        return (size_t)h;
    }
};

struct TexTriple
{
    uint32_t t[3];
    bool operator==(const TexTriple& o) const { return t[0] == o.t[0] && t[1] == o.t[1] && t[2] == o.t[2]; }
};

struct TexTripleHash
{
    size_t operator()(const TexTriple& k) const
    {
        return ((size_t)k.t[0] * 73856093u) ^ ((size_t)k.t[1] * 19349663u) ^ ((size_t)k.t[2] * 83492791u);
    }
};

} // namespace

struct VulkanRasterizer::Impl
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkPhysicalDeviceMemoryProperties mem_props{};
    VkDeviceSize ubo_align = 256;
    bool has_logic_op = false;
    bool warned_logic_op = false;
    std::string name;

    VkFormat depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    bool depth_is_float = false;

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer upload_cmd = VK_NULL_HANDLE, draw_cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    std::unordered_map<HwPipelineKey, VkPipeline, KeyHash> pipelines;

    Image dummy_tex;

    //Render target
    uint32_t width = 0, height = 0;
    Image color, depth;
    VkImageView depth_only_view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    Buffer readback;
    uint64_t target_tag = 0;
    bool target_valid = false;
    std::vector<Target> parked;
    uint64_t target_creates = 0, target_reuses = 0;
    //Time in vkQueueSubmit and in vkWaitForFences since the last stats() call, in ms
    double t_submit = 0, t_fence = 0, t_copy_out = 0;

    Buffer vertex_ring, uniform_ring;
    std::vector<Buffer> staging;
    size_t staging_index = 0;

    std::unordered_map<uint64_t, Texture> textures;
    uint64_t texture_bytes = 0;
    std::vector<uint64_t> texture_ids; //id - 1 -> cache key, rebuilt every submit
    std::unordered_map<TexTriple, VkDescriptorSet, TexTripleHash> desc_sets;
    uint32_t desc_sets_used = 0;

    //Session state
    bool session = false;
    bool recording = false;
    bool render_pass_open = false;
    uint64_t serial = 1;

    //Pending (merged) draw
    bool pending = false;
    HwPipelineKey pending_key{};
    HwDynamicState pending_dyn{};
    HwUniforms pending_uniforms{};
    TexTriple pending_tex{};
    uint32_t pending_first = 0, pending_count = 0;

    //State bound in the current command buffer
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    bool have_dyn = false;
    HwDynamicState bound_dyn{};

    ~Impl();
    void init();
    uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags flags);
    //host_read: the CPU reads it back. Such buffers go in HOST_CACHED memory when the device has
    //it: plain host-visible memory is write-combined on discrete GPUs and reading it is very slow.
    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_read = false);
    void free_buffer(Buffer& b);
    Image make_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void free_image(Image& i);
    void make_target(uint32_t w, uint32_t h);
    void free_target();
    //Makes a w x h target current, preferring the parked one with this tag. Returns whether
    //it still holds that tag's content.
    bool select_target(uint32_t w, uint32_t h, uint64_t tag);
    void swap_target(Target& t);
    VkPipeline get_pipeline(const HwPipelineKey& key);
    uint8_t* stage(VkDeviceSize size, VkBuffer& buf, VkDeviceSize& offset);
    void begin_recording();
    void submit_and_wait();
    void flush_pending();
    void open_render_pass();
    void internal_flush();
    VkDescriptorSet get_desc_set(const TexTriple& t);
    void barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                 VkAccessFlags src_access, VkAccessFlags dst_access,
                 VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage);
};

VulkanRasterizer::Impl::~Impl()
{
    if (device)
    {
        vkDeviceWaitIdle(device);
        for (auto& t : textures)
            free_image(t.second.image);
        free_image(dummy_tex);
        free_target();
        for (Target& t : parked)
        {
            swap_target(t);
            free_target();
        }
        free_buffer(vertex_ring);
        free_buffer(uniform_ring);
        for (auto& b : staging)
            free_buffer(b);
        for (auto& p : pipelines)
            vkDestroyPipeline(device, p.second, nullptr);
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        vkDestroySampler(device, sampler, nullptr);
        vkDestroyDescriptorPool(device, desc_pool, nullptr);
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        vkDestroyRenderPass(device, render_pass, nullptr);
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, cmd_pool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (instance)
        vkDestroyInstance(instance, nullptr);
}

uint32_t VulkanRasterizer::Impl::find_memory(uint32_t bits, VkMemoryPropertyFlags flags)
{
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
    {
        if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    throw std::runtime_error("no suitable Vulkan memory type");
}

Buffer VulkanRasterizer::Impl::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_read)
{
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device, &bi, nullptr, &b.buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VkMemoryPropertyFlags wanted[3] = {visible | coherent, visible | coherent, visible | coherent};
    if (host_read)
    {
        wanted[0] = visible | cached | coherent;
        wanted[1] = visible | cached;
    }
    bool found = false;
    for (VkMemoryPropertyFlags f : wanted)
    {
        for (uint32_t i = 0; i < mem_props.memoryTypeCount && !found; i++)
        {
            if ((req.memoryTypeBits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & f) == f)
            {
                ai.memoryTypeIndex = i;
                b.coherent = (f & coherent) != 0;
                b.cached = (mem_props.memoryTypes[i].propertyFlags & cached) != 0;
                found = true;
            }
        }
        if (found)
            break;
    }
    if (!found)
        ai.memoryTypeIndex = find_memory(req.memoryTypeBits, visible | coherent);
    check(vkAllocateMemory(device, &ai, nullptr, &b.mem), "vkAllocateMemory");
    check(vkBindBufferMemory(device, b.buf, b.mem, 0), "vkBindBufferMemory");
    void* p;
    check(vkMapMemory(device, b.mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
    b.map = (uint8_t*)p;
    return b;
}

void VulkanRasterizer::Impl::free_buffer(Buffer& b)
{
    if (b.buf)
        vkDestroyBuffer(device, b.buf, nullptr);
    if (b.mem)
        vkFreeMemory(device, b.mem, nullptr);
    b = Buffer();
}

Image VulkanRasterizer::Impl::make_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                                         VkImageAspectFlags aspect)
{
    Image im;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device, &ii, nullptr, &im.img), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, im.img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &ai, nullptr, &im.mem), "vkAllocateMemory");
    check(vkBindImageMemory(device, im.img, im.mem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    check(vkCreateImageView(device, &vi, nullptr, &im.view), "vkCreateImageView");
    return im;
}

void VulkanRasterizer::Impl::free_image(Image& i)
{
    if (i.view)
        vkDestroyImageView(device, i.view, nullptr);
    if (i.img)
        vkDestroyImage(device, i.img, nullptr);
    if (i.mem)
        vkFreeMemory(device, i.mem, nullptr);
    i = Image();
}

void VulkanRasterizer::Impl::barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                                     VkImageLayout from, VkImageLayout to,
                                     VkAccessFlags src_access, VkAccessFlags dst_access,
                                     VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanRasterizer::Impl::init()
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Blossom";
    app.apiVersion = VK_API_VERSION_1_1;

    std::vector<const char*> layers;
    const char* validation = getenv("BLOSSOM_VK_VALIDATION");
    if (validation && validation[0] == '1')
    {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        for (auto& p : props)
        {
            if (!strcmp(p.layerName, "VK_LAYER_KHRONOS_validation"))
                layers.push_back("VK_LAYER_KHRONOS_validation");
        }
        if (layers.empty())
            fprintf(stderr, "[Vulkan] BLOSSOM_VK_VALIDATION=1 but the validation layer is not installed\n");
    }

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = (uint32_t)layers.size();
    ici.ppEnabledLayerNames = layers.data();
    check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (!count)
        throw std::runtime_error("no Vulkan devices");
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(instance, &count, devs.data());

    //Pick: BLOSSOM_VK_DEVICE index, else discrete > integrated > virtual > cpu
    int chosen = -1;
    const char* dev_env = getenv("BLOSSOM_VK_DEVICE");
    if (dev_env && *dev_env)
    {
        int idx = atoi(dev_env);
        if (idx >= 0 && idx < (int)count)
            chosen = idx;
    }
    if (chosen < 0)
    {
        auto rank = [](VkPhysicalDeviceType t) {
            switch (t)
            {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
                case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
                default: return 0;
            }
        };
        int best = -1;
        for (uint32_t i = 0; i < count; i++)
        {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devs[i], &p);
            fprintf(stderr, "[Vulkan] Device %u: %s\n", i, p.deviceName);
            if (rank(p.deviceType) > best)
            {
                best = rank(p.deviceType);
                chosen = (int)i;
            }
        }
    }
    phys = devs[chosen];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    name = props.deviceName;
    ubo_align = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);

    VkPhysicalDeviceFeatures feats;
    vkGetPhysicalDeviceFeatures(phys, &feats);
    has_logic_op = feats.logicOp;

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, qprops.data());
    bool found = false;
    for (uint32_t i = 0; i < qcount; i++)
    {
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            queue_family = i;
            found = true;
            break;
        }
    }
    if (!found)
        throw std::runtime_error("no graphics queue");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures enabled{};
    enabled.logicOp = has_logic_op;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &enabled;
    check(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    //Depth format: D24S8 matches the PICA, D32F_S8 is the fallback (AMD has no D24S8)
    auto depth_ok = [&](VkFormat f) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys, f, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        return (fp.optimalTilingFeatures & need) == need;
    };
    if (depth_ok(VK_FORMAT_D24_UNORM_S8_UINT))
        depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    else if (depth_ok(VK_FORMAT_D32_SFLOAT_S8_UINT))
    {
        depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        depth_is_float = true;
    }
    else
        throw std::runtime_error("no usable depth/stencil format");

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = queue_family;
    check(vkCreateCommandPool(device, &cpi, nullptr, &cmd_pool), "vkCreateCommandPool");
    VkCommandBuffer cmds[2];
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmd_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 2;
    check(vkAllocateCommandBuffers(device, &cai, cmds), "vkAllocateCommandBuffers");
    upload_cmd = cmds[0];
    draw_cmd = cmds[1];
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence");

    //Render pass: both attachments are loaded and stored, the session handles layouts around it
    VkAttachmentDescription att[2]{};
    att[0].format = COLOR_FORMAT;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1].format = depth_format;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    sub.pDepthStencilAttachment = &depth_ref;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 2;
    rpi.pAttachments = att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    check(vkCreateRenderPass(device, &rpi, nullptr, &render_pass), "vkCreateRenderPass");

    VkDescriptorSetLayoutBinding binds[4]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int i = 1; i < 4; i++)
    {
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 4;
    dli.pBindings = binds;
    check(vkCreateDescriptorSetLayout(device, &dli, nullptr, &set_layout), "vkCreateDescriptorSetLayout");

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    check(vkCreatePipelineLayout(device, &pli, nullptr, &pipeline_layout), "vkCreatePipelineLayout");

    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, DESCRIPTOR_SETS_PER_FLUSH},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DESCRIPTOR_SETS_PER_FLUSH * 3},
    };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = DESCRIPTOR_SETS_PER_FLUSH;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(device, &dpi, nullptr, &desc_pool), "vkCreateDescriptorPool");

    //Textures are read with texelFetch, so the sampler settings don't matter much
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    check(vkCreateSampler(device, &si, nullptr, &sampler), "vkCreateSampler");

    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(pica_vert_spv);
    smi.pCode = pica_vert_spv;
    check(vkCreateShaderModule(device, &smi, nullptr, &vert), "vkCreateShaderModule");
    smi.codeSize = sizeof(pica_frag_spv);
    smi.pCode = pica_frag_spv;
    check(vkCreateShaderModule(device, &smi, nullptr, &frag), "vkCreateShaderModule");

    vertex_ring = make_buffer(VERTEX_RING_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    uniform_ring = make_buffer(UNIFORM_RING_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    //1x1 texture bound to unused sampler slots
    dummy_tex = make_image(1, 1, COLOR_FORMAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    begin_recording();
    barrier(upload_cmd, dummy_tex.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    submit_and_wait();
}

void VulkanRasterizer::Impl::make_target(uint32_t w, uint32_t h)
{
    free_target();
    width = w;
    height = h;
    color = make_image(w, h, COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    depth = make_image(w, h, depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkImageView views[2] = {color.view, depth.view};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = render_pass;
    fi.attachmentCount = 2;
    fi.pAttachments = views;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    check(vkCreateFramebuffer(device, &fi, nullptr, &framebuffer), "vkCreateFramebuffer");
    //Color, depth (4 bytes per texel either way), stencil
    readback = make_buffer((VkDeviceSize)w * h * 9 + 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
}

void VulkanRasterizer::Impl::swap_target(Target& t)
{
    std::swap(width, t.width);
    std::swap(height, t.height);
    std::swap(color, t.color);
    std::swap(depth, t.depth);
    std::swap(framebuffer, t.framebuffer);
    std::swap(readback, t.readback);
    std::swap(target_tag, t.tag);
    std::swap(target_valid, t.valid);
}

bool VulkanRasterizer::Impl::select_target(uint32_t w, uint32_t h, uint64_t tag)
{
    if (width == w && height == h && (!tag || target_tag == tag))
        return tag && target_valid;
    //Park the current one and look for a better match
    if (width)
    {
        Target t;
        swap_target(t);
        t.last_used = serial;
        parked.push_back(t);
    }
    int best = -1;
    for (size_t i = 0; i < parked.size(); i++)
    {
        if (parked[i].width != w || parked[i].height != h)
            continue;
        if (tag && parked[i].tag == tag)
        {
            best = (int)i;
            break;
        }
        if (best < 0)
            best = (int)i;
    }
    if (best >= 0)
    {
        swap_target(parked[best]);
        parked.erase(parked.begin() + best);
        target_reuses++;
    }
    else
    {
        make_target(w, h);
        target_creates++;
    }
    if (target_tag != tag)
    {
        target_tag = tag;
        target_valid = false;
    }
    //Keep a few; free the least recently used
    while (parked.size() > 6)
    {
        size_t oldest = 0;
        for (size_t i = 1; i < parked.size(); i++)
            if (parked[i].last_used < parked[oldest].last_used)
                oldest = i;
        Target t = parked[oldest];
        parked.erase(parked.begin() + oldest);
        swap_target(t);
        free_target();
        swap_target(t);
    }
    return tag && target_valid;
}

void VulkanRasterizer::Impl::free_target()
{
    if (framebuffer)
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    framebuffer = VK_NULL_HANDLE;
    free_image(color);
    free_image(depth);
    free_buffer(readback);
    width = height = 0;
}

uint8_t* VulkanRasterizer::Impl::stage(VkDeviceSize size, VkBuffer& buf, VkDeviceSize& offset)
{
    size = (size + 15) & ~15ULL;
    while (staging_index < staging.size())
    {
        Buffer& b = staging[staging_index];
        if (b.offset + size <= b.size)
        {
            buf = b.buf;
            offset = b.offset;
            b.offset += size;
            return b.map + offset;
        }
        staging_index++;
    }
    staging.push_back(make_buffer(std::max(size, STAGING_CHUNK_SIZE), VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    staging_index = staging.size() - 1;
    Buffer& b = staging.back();
    buf = b.buf;
    offset = 0;
    b.offset = size;
    return b.map;
}

void VulkanRasterizer::Impl::begin_recording()
{
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(upload_cmd, &bi), "vkBeginCommandBuffer");
    check(vkBeginCommandBuffer(draw_cmd, &bi), "vkBeginCommandBuffer");
    recording = true;
    render_pass_open = false;
    bound_pipeline = VK_NULL_HANDLE;
    have_dyn = false;
}

void VulkanRasterizer::Impl::submit_and_wait()
{
    check(vkEndCommandBuffer(upload_cmd), "vkEndCommandBuffer");
    check(vkEndCommandBuffer(draw_cmd), "vkEndCommandBuffer");
    recording = false;
    VkCommandBuffer cmds[2] = {upload_cmd, draw_cmd};
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 2;
    si.pCommandBuffers = cmds;
    auto t0 = std::chrono::steady_clock::now();
    check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
    auto t1 = std::chrono::steady_clock::now();
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    auto t2 = std::chrono::steady_clock::now();
    t_submit += std::chrono::duration<double, std::milli>(t1 - t0).count();
    t_fence += std::chrono::duration<double, std::milli>(t2 - t1).count();
    vkResetFences(device, 1, &fence);

    //Everything recorded is done, so the per-submit allocators start over
    vertex_ring.offset = 0;
    uniform_ring.offset = 0;
    for (auto& b : staging)
        b.offset = 0;
    staging_index = 0;
    //A session with many texture misses can grow staging a lot; don't keep it all around
    VkDeviceSize staging_total = 0;
    for (auto& b : staging)
        staging_total += b.size;
    while (staging_total > STAGING_KEEP && staging.size() > 1)
    {
        staging_total -= staging.back().size;
        free_buffer(staging.back());
        staging.pop_back();
    }
    vkResetDescriptorPool(device, desc_pool, 0);
    desc_sets.clear();
    desc_sets_used = 0;
    serial++;

    //Evict old textures that the GPU is no longer using
    if (textures.size() > TEXTURE_CACHE_LIMIT || texture_bytes > TEXTURE_BYTES_LIMIT)
    {
        std::vector<std::pair<uint64_t, uint64_t>> age;
        for (auto& t : textures)
            age.push_back({t.second.last_used, t.first});
        std::sort(age.begin(), age.end());
        size_t drop = textures.size() > TEXTURE_CACHE_LIMIT / 2 ? textures.size() - TEXTURE_CACHE_LIMIT / 2 : 0;
        for (size_t i = 0; i < age.size() && (i < drop || texture_bytes > TEXTURE_BYTES_LIMIT / 2); i++)
        {
            Texture& t = textures[age[i].second];
            texture_bytes -= (uint64_t)t.width * t.height * 4;
            free_image(t.image);
            textures.erase(age[i].second);
        }
    }
    texture_ids.clear();
}

VkPipeline VulkanRasterizer::Impl::get_pipeline(const HwPipelineKey& key)
{
    auto it = pipelines.find(key);
    if (it != pipelines.end())
        return it->second;

    static const VkCompareOp compare_ops[8] = {
        VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
        VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL,
    };
    static const VkLogicOp logic_ops[16] = {
        VK_LOGIC_OP_CLEAR, VK_LOGIC_OP_AND, VK_LOGIC_OP_AND_REVERSE, VK_LOGIC_OP_COPY,
        VK_LOGIC_OP_SET, VK_LOGIC_OP_COPY_INVERTED, VK_LOGIC_OP_NO_OP, VK_LOGIC_OP_INVERT,
        VK_LOGIC_OP_NAND, VK_LOGIC_OP_OR, VK_LOGIC_OP_NOR, VK_LOGIC_OP_XOR,
        VK_LOGIC_OP_EQUIVALENT, VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_OR_REVERSE, VK_LOGIC_OP_OR_INVERTED,
    };
    //PICA blend factors, equations and stencil ops use the same numbering as Vulkan
    auto factor = [](uint8_t f) { return f <= 14 ? (VkBlendFactor)f : VK_BLEND_FACTOR_ONE; };
    auto equation = [](uint8_t e) { return e <= 4 ? (VkBlendOp)e : VK_BLEND_OP_ADD; };

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vb{0, sizeof(HwVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[5] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(HwVertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(HwVertex, color)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(HwVertex, tc0)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(HwVertex, tc1)},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(HwVertex, tc2)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = va;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; //Culling is decided on the CPU, like the software renderer
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkStencilOpState st{};
    st.failOp = (VkStencilOp)(key.stencil_fail & 7);
    st.depthFailOp = (VkStencilOp)(key.stencil_zfail & 7);
    st.passOp = (VkStencilOp)(key.stencil_pass & 7);
    st.compareOp = compare_ops[key.stencil_func & 7];
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    //Vulkan skips depth writes when the test is off, but the PICA still writes: test with ALWAYS
    ds.depthTestEnable = key.depth_test || key.depth_write;
    ds.depthWriteEnable = key.depth_write;
    ds.depthCompareOp = key.depth_test ? compare_ops[key.depth_func & 7] : VK_COMPARE_OP_ALWAYS;
    ds.stencilTestEnable = key.stencil_enable;
    ds.front = st;
    ds.back = st;
    ds.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = key.color_mask & 0xF;
    ba.blendEnable = key.blend_enable && !key.logic_op_enable;
    ba.srcColorBlendFactor = factor(key.rgb_src);
    ba.dstColorBlendFactor = factor(key.rgb_dst);
    ba.colorBlendOp = equation(key.rgb_eq);
    ba.srcAlphaBlendFactor = factor(key.alpha_src);
    ba.dstAlphaBlendFactor = factor(key.alpha_dst);
    ba.alphaBlendOp = equation(key.alpha_eq);
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.logicOpEnable = key.logic_op_enable && has_logic_op;
    cb.logicOp = logic_ops[key.logic_op & 0xF];
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = sizeof(dyn) / sizeof(dyn[0]);
    dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &dy;
    gpi.layout = pipeline_layout;
    gpi.renderPass = render_pass;
    gpi.subpass = 0;
    VkPipeline p;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &p), "vkCreateGraphicsPipelines");
    pipelines[key] = p;
    return p;
}

VkDescriptorSet VulkanRasterizer::Impl::get_desc_set(const TexTriple& t)
{
    auto it = desc_sets.find(t);
    if (it != desc_sets.end())
        return it->second;

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = desc_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout;
    VkDescriptorSet set;
    check(vkAllocateDescriptorSets(device, &ai, &set), "vkAllocateDescriptorSets");
    desc_sets_used++;

    VkDescriptorBufferInfo bi{uniform_ring.buf, 0, sizeof(HwUniforms)};
    VkDescriptorImageInfo ii[3];
    for (int i = 0; i < 3; i++)
    {
        VkImageView view = dummy_tex.view;
        if (t.t[i] && t.t[i] <= texture_ids.size())
            view = textures[texture_ids[t.t[i] - 1]].image.view;
        ii[i] = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkWriteDescriptorSet w[4]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = set;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w[0].pBufferInfo = &bi;
    for (int i = 0; i < 3; i++)
    {
        w[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i + 1].dstSet = set;
        w[i + 1].dstBinding = i + 1;
        w[i + 1].descriptorCount = 1;
        w[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i + 1].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(device, 4, w, 0, nullptr);
    desc_sets[t] = set;
    return set;
}

void VulkanRasterizer::Impl::open_render_pass()
{
    if (render_pass_open)
        return;
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = render_pass;
    rbi.framebuffer = framebuffer;
    rbi.renderArea = {{0, 0}, {width, height}};
    vkCmdBeginRenderPass(draw_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {width, height}};
    vkCmdSetViewport(draw_cmd, 0, 1, &vp);
    vkCmdSetScissor(draw_cmd, 0, 1, &sc);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(draw_cmd, 0, 1, &vertex_ring.buf, &zero);
    float scale[2] = {2.0f / width, 2.0f / height};
    vkCmdPushConstants(draw_cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(scale), scale);
    render_pass_open = true;
    bound_pipeline = VK_NULL_HANDLE;
    have_dyn = false;
}

void VulkanRasterizer::Impl::flush_pending()
{
    if (!pending)
        return;
    pending = false;
    if (!pending_count)
        return;

    //Room for the uniforms and a descriptor set, else submit what we have first
    VkDeviceSize ubo_size = (sizeof(HwUniforms) + ubo_align - 1) & ~(ubo_align - 1);
    if (uniform_ring.offset + ubo_size > uniform_ring.size ||
        (desc_sets_used >= DESCRIPTOR_SETS_PER_FLUSH && !desc_sets.count(pending_tex)))
        throw std::logic_error("flush_pending called without room"); //draw() guarantees room

    open_render_pass();
    VkPipeline p = get_pipeline(pending_key);
    if (p != bound_pipeline)
    {
        vkCmdBindPipeline(draw_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        bound_pipeline = p;
    }
    if (!have_dyn || memcmp(&bound_dyn, &pending_dyn, sizeof(bound_dyn)))
    {
        vkCmdSetBlendConstants(draw_cmd, pending_dyn.blend_const);
        vkCmdSetStencilReference(draw_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, pending_dyn.stencil_ref);
        vkCmdSetStencilCompareMask(draw_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, pending_dyn.stencil_compare_mask);
        vkCmdSetStencilWriteMask(draw_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, pending_dyn.stencil_write_mask);
        bound_dyn = pending_dyn;
        have_dyn = true;
    }

    uint32_t ubo_offset = (uint32_t)uniform_ring.offset;
    memcpy(uniform_ring.map + ubo_offset, &pending_uniforms, sizeof(HwUniforms));
    uniform_ring.offset += ubo_size;

    VkDescriptorSet set = get_desc_set(pending_tex);
    vkCmdBindDescriptorSets(draw_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &set, 1, &ubo_offset);
    vkCmdDraw(draw_cmd, pending_count, 1, pending_first, 0);
}

void VulkanRasterizer::Impl::internal_flush()
{
    //Out of ring space in the middle of a session: run what we have and keep going. The target
    //stays on the GPU, so nothing is read back.
    flush_pending();
    if (render_pass_open)
        vkCmdEndRenderPass(draw_cmd);
    submit_and_wait();
    begin_recording();
}

std::unique_ptr<VulkanRasterizer> VulkanRasterizer::create(std::string& err)
{
    auto impl = std::make_unique<Impl>();
    try
    {
        impl->init();
    }
    catch (const std::exception& ex)
    {
        err = ex.what();
        return nullptr;
    }
    return std::unique_ptr<VulkanRasterizer>(new VulkanRasterizer(std::move(impl)));
}

VulkanRasterizer::VulkanRasterizer(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
VulkanRasterizer::~VulkanRasterizer() = default;

const std::string& VulkanRasterizer::device_name() const
{
    return impl->name;
}

bool VulkanRasterizer::depth_is_float() const
{
    return impl->depth_is_float;
}

std::string VulkanRasterizer::stats()
{
    VkDeviceSize staging_total = 0;
    for (auto& b : impl->staging)
        staging_total += b.size;
    char buf[320];
    snprintf(buf, sizeof(buf), "textures %zu (%llu MB) staging %llu MB pipelines %zu submits %llu readback %s targets %zu new %llu"
             " | ms total since last: submit %.1f fence %.1f copy-out %.1f",
             impl->textures.size(), (unsigned long long)(impl->texture_bytes >> 20),
             (unsigned long long)(staging_total >> 20), impl->pipelines.size(),
             (unsigned long long)impl->serial, impl->readback.cached ? "cached" : "uncached",
             impl->parked.size() + 1, (unsigned long long)impl->target_creates, impl->t_submit, impl->t_fence,
             impl->t_copy_out);
    impl->t_submit = impl->t_fence = impl->t_copy_out = 0;
    return buf;
}

bool VulkanRasterizer::in_session() const
{
    return impl->session;
}

bool VulkanRasterizer::begin_cached(uint64_t tag, uint32_t width, uint32_t height)
{
    Impl& d = *impl;
    if (d.session)
        end(nullptr, nullptr, nullptr);
    if (!tag || !d.select_target(width, height, tag))
        return false;
    //The images are where the last end() left them (transfer source); keep their contents
    d.begin_recording();
    d.session = true;
    d.target_valid = false;
    VkCommandBuffer cmd = d.upload_cmd;
    d.barrier(cmd, d.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    d.barrier(cmd, d.depth.img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
              VK_ACCESS_TRANSFER_READ_BIT,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    return true;
}

void VulkanRasterizer::begin(uint32_t width, uint32_t height, const uint32_t* color, const float* depth,
                             const uint8_t* stencil, uint64_t tag)
{
    Impl& d = *impl;
    if (d.session)
        end(nullptr, nullptr, nullptr);
    d.select_target(width, height, tag);
    d.target_valid = false;

    d.begin_recording();
    d.session = true;

    size_t n = (size_t)width * height;
    VkBuffer buf;
    VkDeviceSize off;
    uint8_t* p = d.stage(n * 9, buf, off);
    memcpy(p, color, n * 4);
    uint32_t* dp = (uint32_t*)(p + n * 4);
    for (size_t i = 0; i < n; i++)
    {
        float f = std::clamp(depth[i], 0.0f, 1.0f);
        if (d.depth_is_float)
            memcpy(&dp[i], &f, 4);
        else
            dp[i] = (uint32_t)((double)f * 16777215.0 + 0.5); //f is clamped, so this is lroundf
    }
    memcpy(p + n * 8, stencil, n);

    VkCommandBuffer cmd = d.upload_cmd;
    d.barrier(cmd, d.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    d.barrier(cmd, d.depth.img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c{};
    c.bufferOffset = off;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, buf, d.color.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    VkBufferImageCopy dc[2]{};
    dc[0].bufferOffset = off + n * 4;
    dc[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    dc[0].imageExtent = {width, height, 1};
    dc[1].bufferOffset = off + n * 8;
    dc[1].imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
    dc[1].imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, buf, d.depth.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, dc);
    d.barrier(cmd, d.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    d.barrier(cmd, d.depth.img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
              VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
}

uint32_t VulkanRasterizer::texture(uint64_t hash, uint32_t width, uint32_t height,
                                   const std::function<void(uint32_t* rgba)>& fill)
{
    Impl& d = *impl;
    uint64_t key = hash ^ ((uint64_t)width << 48) ^ ((uint64_t)height << 32) ^ 0x9E3779B97F4A7C15ULL;
    auto it = d.textures.find(key);
    if (it == d.textures.end())
    {
        Texture t;
        t.width = width;
        t.height = height;
        t.image = d.make_image(width, height, COLOR_FORMAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
        VkBuffer buf;
        VkDeviceSize off;
        uint8_t* p = d.stage((VkDeviceSize)width * height * 4, buf, off);
        fill((uint32_t*)p);
        d.barrier(d.upload_cmd, t.image.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy c{};
        c.bufferOffset = off;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(d.upload_cmd, buf, t.image.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        d.barrier(d.upload_cmd, t.image.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        d.texture_bytes += (uint64_t)width * height * 4;
        it = d.textures.emplace(key, t).first;
    }
    it->second.last_used = d.serial;
    for (size_t i = 0; i < d.texture_ids.size(); i++)
    {
        if (d.texture_ids[i] == key)
            return (uint32_t)i + 1;
    }
    d.texture_ids.push_back(key);
    return (uint32_t)d.texture_ids.size();
}

void VulkanRasterizer::draw(const HwPipelineKey& key, const HwDynamicState& dyn, const HwUniforms& uniforms,
                            const uint32_t tex_ids[3], const HwVertex* vertices, uint32_t count)
{
    Impl& d = *impl;
    if (!d.session || !count)
        return;

    if (key.logic_op_enable && !d.has_logic_op && key.logic_op != 3 && !d.warned_logic_op)
    {
        fprintf(stderr, "[Vulkan] This device has no logicOp support; logic ops draw as COPY\n");
        d.warned_logic_op = true;
    }

    TexTriple tt{{tex_ids[0], tex_ids[1], tex_ids[2]}};
    VkDeviceSize bytes = (VkDeviceSize)count * sizeof(HwVertex);
    bool same = d.pending && d.pending_key == key && !memcmp(&d.pending_dyn, &dyn, sizeof(dyn)) &&
                d.pending_tex == tt && !memcmp(&d.pending_uniforms, &uniforms, sizeof(uniforms));

    //Room for this draw: vertices, plus uniforms and a descriptor set if it starts a new batch
    VkDeviceSize ubo_size = (sizeof(HwUniforms) + d.ubo_align - 1) & ~(d.ubo_align - 1);
    bool room = d.vertex_ring.offset + bytes <= d.vertex_ring.size &&
                d.uniform_ring.offset + 2 * ubo_size <= d.uniform_ring.size &&
                d.desc_sets_used + 2 <= DESCRIPTOR_SETS_PER_FLUSH;
    if (!room)
    {
        //Texture ids belong to the submit, so callers must look them up again after this.
        //GPU::hw_draw does that by always calling texture() right before draw(); keep the
        //textures alive across the flush by re-registering them.
        std::vector<uint64_t> keys;
        for (int i = 0; i < 3; i++)
            keys.push_back(tt.t[i] && tt.t[i] <= d.texture_ids.size() ? d.texture_ids[tt.t[i] - 1] : 0);
        d.internal_flush();
        for (int i = 0; i < 3; i++)
        {
            tt.t[i] = 0;
            if (!keys[i])
                continue;
            auto kt = d.textures.find(keys[i]);
            if (kt == d.textures.end())
                continue;
            kt->second.last_used = d.serial;
            d.texture_ids.push_back(keys[i]);
            tt.t[i] = (uint32_t)d.texture_ids.size();
        }
        same = false;
    }

    uint32_t first = (uint32_t)(d.vertex_ring.offset / sizeof(HwVertex));
    memcpy(d.vertex_ring.map + d.vertex_ring.offset, vertices, bytes);
    d.vertex_ring.offset += bytes;

    if (same && d.pending_first + d.pending_count == first)
    {
        d.pending_count += count;
        return;
    }

    d.flush_pending();
    d.pending = true;
    d.pending_key = key;
    d.pending_dyn = dyn;
    d.pending_uniforms = uniforms;
    d.pending_tex = tt;
    d.pending_first = first;
    d.pending_count = count;
}

void VulkanRasterizer::end(uint32_t* color, float* depth, uint8_t* stencil)
{
    Impl& d = *impl;
    if (!d.session)
        return;
    d.flush_pending();
    if (d.render_pass_open)
    {
        vkCmdEndRenderPass(d.draw_cmd);
        d.render_pass_open = false;
    }

    VkCommandBuffer cmd = d.draw_cmd;
    size_t n = (size_t)d.width * d.height;
    d.barrier(cmd, d.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    d.barrier(cmd, d.depth.img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {d.width, d.height, 1};
    vkCmdCopyImageToBuffer(cmd, d.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.readback.buf, 1, &c);
    VkBufferImageCopy dc[2]{};
    dc[0].bufferOffset = n * 4;
    dc[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    dc[0].imageExtent = {d.width, d.height, 1};
    dc[1].bufferOffset = n * 8;
    dc[1].imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
    dc[1].imageExtent = {d.width, d.height, 1};
    vkCmdCopyImageToBuffer(cmd, d.depth.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.readback.buf, 2, dc);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);

    d.submit_and_wait();
    d.session = false;
    //end() leaves the images in the transfer-source layout begin_cached expects
    d.target_valid = d.target_tag != 0;

    if (!d.readback.coherent)
    {
        VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        r.memory = d.readback.mem;
        r.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(d.device, 1, &r);
    }
    auto copy_t0 = std::chrono::steady_clock::now();
    struct CopyTimer
    {
        double& acc;
        std::chrono::steady_clock::time_point t0;
        ~CopyTimer() { acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
    } copy_timer{d.t_copy_out, copy_t0};
    const uint8_t* p = d.readback.map;
    if (color)
        memcpy(color, p, n * 4);
    if (depth)
    {
        const uint32_t* dp = (const uint32_t*)(p + n * 4);
        for (size_t i = 0; i < n; i++)
        {
            if (d.depth_is_float)
                memcpy(&depth[i], &dp[i], 4);
            else
                depth[i] = (dp[i] & 0xFFFFFF) / 16777215.0f;
        }
    }
    if (stencil)
        memcpy(stencil, p + n * 8, n);
}

} // namespace VideoVulkan
