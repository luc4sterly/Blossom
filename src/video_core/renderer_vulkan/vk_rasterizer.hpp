// Blossom: Vulkan hardware rasterizer for the PICA200.
//
// The structure (instance/device setup, a pipeline cache keyed by fixed-function state, a texture
// cache and one uber fragment shader) follows Azahar's renderer_vulkan (GPLv3,
// https://github.com/azahar-emu/azahar). Blossom keeps Corgi3DS' CPU vertex pipeline, so this
// class only receives clipped, screen-space triangles and fixed-function state from GPU (gpu_hw.cpp).
//
// Every PICA command list is one "session": the color/depth buffers are uploaded from emulated
// memory when the session starts, the draws are recorded, and at the end the result is read back
// and written to emulated memory, so the CPU, the display transfer engine and the LCD keep seeing
// the same memory they see with the software renderer.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace VideoVulkan {

struct HwVertex
{
    float pos[4];   // x, y in pixels; z / w; w
    float color[4];
    float tc0[2];
    float tc1[2];
    float tc2[2];
};
static_assert(sizeof(HwVertex) == 56);

// std140 mirror of DrawUniforms in shaders/pica.frag
struct HwUniforms
{
    uint32_t tev[6][4];
    int32_t tev_const[6][4];
    int32_t buffer_init[4];
    int32_t misc[4];
    float depth[4];
    int32_t tex_info[3][4];
    int32_t tex_border[3][4];
    int32_t tex_misc[4];
};

// Fixed-function state that needs its own VkPipeline. Values use the PICA register encodings.
struct HwPipelineKey
{
    uint8_t blend_enable;
    uint8_t logic_op_enable;
    uint8_t logic_op;
    uint8_t rgb_eq, alpha_eq;
    uint8_t rgb_src, rgb_dst, alpha_src, alpha_dst;
    uint8_t color_mask;     // bit 0 = R ... bit 3 = A
    uint8_t depth_test;     // test against the stored depth
    uint8_t depth_func;
    uint8_t depth_write;
    uint8_t stencil_enable;
    uint8_t stencil_func;
    uint8_t stencil_fail, stencil_zfail, stencil_pass;
    uint8_t pad[2];

    bool operator==(const HwPipelineKey& o) const;
};

struct HwDynamicState
{
    float blend_const[4];
    uint8_t stencil_ref;
    uint8_t stencil_compare_mask;
    uint8_t stencil_write_mask;
};

class VulkanRasterizer
{
    public:
        // Returns nullptr and fills err when no usable Vulkan device exists.
        static std::unique_ptr<VulkanRasterizer> create(std::string& err);
        ~VulkanRasterizer();

        const std::string& device_name() const;
        bool in_session() const;

        // Starts a session on a width x height target. color: RGBA8 (R in the low byte),
        // depth: 0..1, stencil: 8 bits; all row-major, top row first.
        void begin(uint32_t width, uint32_t height, const uint32_t* color, const float* depth,
                   const uint8_t* stencil);

        // Returns a texture id for this session. fill is only called on a cache miss and must
        // write width * height RGBA8 texels, row-major.
        uint32_t texture(uint64_t hash, uint32_t width, uint32_t height,
                         const std::function<void(uint32_t* rgba)>& fill);

        void draw(const HwPipelineKey& key, const HwDynamicState& dyn, const HwUniforms& uniforms,
                  const uint32_t textures[3], const HwVertex* vertices, uint32_t count);

        // Ends the session, waits for the GPU and copies the target back (any pointer can be null).
        void end(uint32_t* color, float* depth, uint8_t* stencil);

        struct Impl;
    private:
        explicit VulkanRasterizer(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl;
};

} // namespace VideoVulkan
