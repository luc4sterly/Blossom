// Blossom: glue between Corgi3DS' PICA200 emulation (gpu.cpp) and the Vulkan rasterizer
// (src/video_core/renderer_vulkan). The vertex shader, primitive assembly and clipping stay on the
// CPU; clipped polygons go to Vulkan instead of GPU::rasterize_tri.
//
// Memory stays the source of truth: the first draw of a command list uploads the color and depth
// buffers it targets, and the end of the list (or a change of target, or a texture read from the
// target) writes them back in the PICA's tiled formats.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/common.hpp"
#include "common/profiler.hpp"
#include "core/arm11/gpu.hpp"
#include "core/emulator.hpp"

#ifdef BLOSSOM_HAS_VULKAN
#include "video_core/renderer_vulkan/vk_rasterizer.hpp"
#endif

namespace {

constexpr uint32_t Convert1To8(uint32_t v) { return v * 255; }
constexpr uint32_t Convert4To8(uint32_t v) { return (v << 4) | v; }
constexpr uint32_t Convert5To8(uint32_t v) { return (v << 3) | (v >> 2); }
constexpr uint32_t Convert6To8(uint32_t v) { return (v << 2) | (v >> 4); }

int color_bpp(int fmt)
{
    switch (fmt)
    {
        case 0: return 4;
        case 1: return 3;
        default: return 2;
    }
}

int depth_bpp(int fmt)
{
    switch (fmt)
    {
        case 2: return 3;
        case 3: return 4;
        default: return 2;
    }
}

//Texture size in bytes for width x height texels of PICA format fmt
uint32_t texture_bytes(int fmt, uint32_t w, uint32_t h)
{
    static const int bits[16] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8, 0, 0};
    return (uint32_t)(((uint64_t)w * h * bits[fmt & 0xF]) / 8);
}

uint64_t hash_bytes(const uint8_t* p, size_t n, uint64_t seed)
{
    uint64_t h = seed ^ (n * 0x9E3779B97F4A7C15ULL);
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h ^= w * 0xC2B2AE3D27D4EB4FULL;
        h = ((h << 31) | (h >> 33)) * 0x9E3779B97F4A7C15ULL;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ULL;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 32;
    return h;
}

bool ranges_overlap(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
    return a < b + blen && b < a + alen;
}

} // namespace

struct GPU::HwRenderer
{
#ifdef BLOSSOM_HAS_VULKAN
    std::unique_ptr<VideoVulkan::VulkanRasterizer> vk;
    std::vector<VideoVulkan::HwVertex> verts;
#endif
    bool session = false;
    uint32_t color_addr = 0, depth_addr = 0;
    uint32_t frame_width = 0, frame_height = 0;
    uint32_t width = 0, height = 0; //Image size: frame size rounded up to whole 8x8 tiles
    uint8_t color_fmt = 0, depth_fmt = 0;
    uint32_t color_len = 0, depth_len = 0;
    bool color_written = false, depth_written = false;
    std::vector<uint32_t> color;
    std::vector<float> depth;
    std::vector<uint8_t> stencil;
    uint64_t sessions = 0, draws = 0;
};

const char* GPU::renderer_env()
{
    const char* env = getenv("BLOSSOM_GPU");
    return (env && *env) ? env : nullptr;
}

const char* GPU::renderer_name() const
{
    return hw ? "vulkan" : "software";
}

void GPU::hw_shutdown()
{
    if (!hw)
        return;
    hw_end_session();
    delete hw;
    hw = nullptr;
}

bool GPU::set_renderer(const char* name)
{
    std::string n = name ? name : "";
    if (n == "software" || n == "sw" || n == "soft")
    {
        if (hw)
            fprintf(stderr, "[GPU] Renderer: software\n");
        hw_shutdown();
        return true;
    }
    if (n != "vulkan" && n != "vk" && n != "hw")
    {
        fprintf(stderr, "[GPU] Unknown renderer '%s' (use software or vulkan)\n", n.c_str());
        return false;
    }
    if (hw)
        return true;
#ifdef BLOSSOM_HAS_VULKAN
    std::string err;
    auto vk = VideoVulkan::VulkanRasterizer::create(err);
    if (!vk)
    {
        fprintf(stderr, "[GPU] Vulkan failed to start (%s); using the software renderer\n", err.c_str());
        return false;
    }
    fprintf(stderr, "[GPU] Renderer: Vulkan on %s\n", vk->device_name().c_str());
    hw = new HwRenderer();
    hw->vk = std::move(vk);
    return true;
#else
    fprintf(stderr, "[GPU] This build has no Vulkan renderer (ENABLE_VULKAN=OFF); using software\n");
    return false;
#endif
}

//Reads size bytes (little endian) of a color/depth buffer word
uint32_t GPU::hw_read_fb(uint32_t addr, int size)
{
    uint8_t* p = e->phys_ptr(addr, size);
    uint32_t v = 0;
    if (p)
    {
        for (int i = 0; i < size; i++)
            v |= (uint32_t)p[i] << (8 * i);
        return v;
    }
    for (int i = 0; i < size; i++)
        v |= (uint32_t)e->arm11_read8(0, addr + i) << (8 * i);
    return v;
}

void GPU::hw_write_fb(uint32_t addr, uint32_t value, int size)
{
    uint8_t* p = e->phys_ptr(addr, size);
    if (p)
    {
        for (int i = 0; i < size; i++)
            p[i] = (value >> (8 * i)) & 0xFF;
        return;
    }
    for (int i = 0; i < size; i++)
        e->arm11_write8(0, addr + i, (value >> (8 * i)) & 0xFF);
}

#ifdef BLOSSOM_HAS_VULKAN

namespace {

int32_t clamp_u8(int32_t v)
{
    return std::clamp(v, 0, 255);
}

} // namespace

void GPU::hw_end_session()
{
    HwRenderer& h = *hw;
    if (!h.session)
        return;
    Profiler::Scope prof_scope(Profiler::GPU_RASTER);
    h.session = false;
    h.vk->end(h.color_written ? h.color.data() : nullptr, h.depth_written ? h.depth.data() : nullptr,
              h.depth_written ? h.stencil.data() : nullptr);

    if (h.color_written)
    {
        int bpp = color_bpp(h.color_fmt);
        for (uint32_t y = 0; y < h.height; y++)
        {
            for (uint32_t x = 0; x < h.width; x++)
            {
                uint32_t c = h.color[y * h.width + x];
                uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
                uint32_t addr = get_swizzled_tile_addr(h.color_addr, h.frame_width, x, y, bpp);
                switch (h.color_fmt)
                {
                    case 0:
                        hw_write_fb(addr, a | (b << 8) | (g << 16) | (r << 24), 4);
                        break;
                    case 1:
                        hw_write_fb(addr, b | (g << 8) | (r << 16), 3);
                        break;
                    case 2:
                        hw_write_fb(addr, ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7), 2);
                        break;
                    case 3:
                        hw_write_fb(addr, ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3), 2);
                        break;
                    default:
                        hw_write_fb(addr, ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4), 2);
                        break;
                }
            }
        }
    }

    if (h.depth_written)
    {
        int bpp = depth_bpp(h.depth_fmt);
        for (uint32_t y = 0; y < h.height; y++)
        {
            for (uint32_t x = 0; x < h.width; x++)
            {
                size_t i = y * h.width + x;
                uint32_t addr = get_swizzled_tile_addr(h.depth_addr, h.frame_width, x, y, bpp);
                float d = h.depth[i];
                switch (h.depth_fmt)
                {
                    case 2:
                        hw_write_fb(addr, (uint32_t)lroundf(d * 0xFFFFFF), 3);
                        break;
                    case 3:
                        hw_write_fb(addr, (uint32_t)lroundf(d * 0xFFFFFF) | ((uint32_t)h.stencil[i] << 24), 4);
                        break;
                    default:
                        hw_write_fb(addr, (uint32_t)lroundf(d * 0xFFFF), 2);
                        break;
                }
            }
        }
    }
}

void GPU::hw_polygon(std::vector<Vertex>& poly)
{
    Profiler::Scope prof_scope(Profiler::GPU_RASTER);
    HwRenderer& h = *hw;
    using namespace VideoVulkan;

    if (!ctx.frame_width || !ctx.frame_height)
        return;

    //Textures first: one that reads the current target needs the target written back
    uint32_t tex_ids[3] = {0, 0, 0};
    bool tex_on[3];
    for (int i = 0; i < 3; i++)
    {
        tex_on[i] = ctx.tex_enable[i] && ctx.tex_addr[i] && (i != 0 || ctx.tex0_type == 0) &&
                    ctx.tex_width[i] && ctx.tex_height[i] && ctx.tex_width[i] <= 1024 && ctx.tex_height[i] <= 1024;
        if (!tex_on[i])
            continue;
        uint32_t len = texture_bytes(ctx.tex_type[i], ctx.tex_width[i], ctx.tex_height[i]);
        if (h.session && ((h.color_written && ranges_overlap(ctx.tex_addr[i], len, h.color_addr, h.color_len)) ||
                          (h.depth_written && ranges_overlap(ctx.tex_addr[i], len, h.depth_addr, h.depth_len))))
            hw_end_session();
    }

    //Start a session, or a new one if the target changed
    uint32_t fw = ctx.frame_width, fh = ctx.frame_height;
    if (h.session && (h.color_addr != ctx.color_buffer_base || h.depth_addr != ctx.depth_buffer_base ||
                      h.frame_width != fw || h.frame_height != fh || h.color_fmt != ctx.color_format ||
                      h.depth_fmt != ctx.depth_format))
        hw_end_session();

    if (!h.session)
    {
        h.color_addr = ctx.color_buffer_base;
        h.depth_addr = ctx.depth_buffer_base;
        h.frame_width = fw;
        h.frame_height = fh;
        h.color_fmt = ctx.color_format;
        h.depth_fmt = ctx.depth_format;
        h.width = (fw + 7) & ~7u;
        h.height = (fh + 7) & ~7u;
        h.color_len = h.width * h.height * color_bpp(h.color_fmt);
        h.depth_len = h.width * h.height * depth_bpp(h.depth_fmt);
        h.color_written = h.depth_written = false;
        size_t n = (size_t)h.width * h.height;
        h.color.resize(n);
        h.depth.resize(n);
        h.stencil.resize(n);

        int cbpp = color_bpp(h.color_fmt), dbpp = depth_bpp(h.depth_fmt);
        for (uint32_t y = 0; y < h.height; y++)
        {
            for (uint32_t x = 0; x < h.width; x++)
            {
                size_t i = y * h.width + x;
                uint32_t v = hw_read_fb(get_swizzled_tile_addr(h.color_addr, fw, x, y, cbpp), cbpp);
                uint32_t r, g, b, a;
                switch (h.color_fmt)
                {
                    case 0:
                        r = v >> 24; g = (v >> 16) & 0xFF; b = (v >> 8) & 0xFF; a = v & 0xFF;
                        break;
                    case 1:
                        r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF; a = 0xFF;
                        break;
                    case 2:
                        r = Convert5To8(v >> 11); g = Convert5To8((v >> 6) & 0x1F);
                        b = Convert5To8((v >> 1) & 0x1F); a = Convert1To8(v & 1);
                        break;
                    case 3:
                        r = Convert5To8(v >> 11); g = Convert6To8((v >> 5) & 0x3F); b = Convert5To8(v & 0x1F);
                        a = 0xFF;
                        break;
                    default:
                        r = Convert4To8(v >> 12); g = Convert4To8((v >> 8) & 0xF);
                        b = Convert4To8((v >> 4) & 0xF); a = Convert4To8(v & 0xF);
                        break;
                }
                h.color[i] = r | (g << 8) | (b << 16) | (a << 24);

                uint32_t d = hw_read_fb(get_swizzled_tile_addr(h.depth_addr, fw, x, y, dbpp), dbpp);
                switch (h.depth_fmt)
                {
                    case 2:
                        h.depth[i] = d / 16777215.0f;
                        h.stencil[i] = 0;
                        break;
                    case 3:
                        h.depth[i] = (d & 0xFFFFFF) / 16777215.0f;
                        h.stencil[i] = d >> 24;
                        break;
                    default:
                        h.depth[i] = d / 65535.0f;
                        h.stencil[i] = 0;
                        break;
                }
            }
        }
        h.vk->begin(h.width, h.height, h.color.data(), h.depth.data(), h.stencil.data());
        h.session = true;
        h.sessions++;
    }

    for (int i = 0; i < 3; i++)
    {
        if (!tex_on[i])
            continue;
        uint32_t w = ctx.tex_width[i], th = ctx.tex_height[i];
        uint32_t len = texture_bytes(ctx.tex_type[i], w, th);
        uint8_t* p = e->phys_ptr(ctx.tex_addr[i], len);
        uint64_t hash;
        if (p)
            hash = hash_bytes(p, len, ((uint64_t)ctx.tex_type[i] << 56) ^ ctx.tex_addr[i]);
        else
        {
            std::vector<uint8_t> tmp(len);
            for (uint32_t j = 0; j < len; j++)
                tmp[j] = e->arm11_read8(0, ctx.tex_addr[i] + j);
            hash = hash_bytes(tmp.data(), len, ((uint64_t)ctx.tex_type[i] << 56) ^ ctx.tex_addr[i]);
        }
        tex_ids[i] = h.vk->texture(hash, w, th, [&](uint32_t* out) {
            for (uint32_t v = 0; v < th; v++)
            {
                for (uint32_t u = 0; u < w; u++)
                {
                    RGBA_Color c;
                    decode_texel(i, u, v, c);
                    out[v * w + u] = (uint32_t)clamp_u8(c.r) | ((uint32_t)clamp_u8(c.g) << 8) |
                                     ((uint32_t)clamp_u8(c.b) << 16) | ((uint32_t)clamp_u8(c.a) << 24);
                }
            }
        });
    }

    //Fixed-function state
    HwPipelineKey key{};
    if (ctx.blend_mode == 0)
    {
        key.logic_op_enable = 1;
        key.logic_op = ctx.logic_op & 0xF;
    }
    else
    {
        key.blend_enable = 1;
        key.rgb_eq = ctx.blend_rgb_equation;
        key.alpha_eq = ctx.blend_alpha_equation;
        key.rgb_src = ctx.blend_rgb_src_func;
        key.rgb_dst = ctx.blend_rgb_dst_func;
        key.alpha_src = ctx.blend_alpha_src_func;
        key.alpha_dst = ctx.blend_alpha_dst_func;
    }
    for (int i = 0; i < 4; i++)
        key.color_mask |= ctx.rgba_write_enabled[i] << i;
    key.depth_test = ctx.depth_test_enabled;
    key.depth_func = key.depth_test ? ctx.depth_test_func : 0;
    key.depth_write = ctx.depth_write_enabled && (ctx.allow_stencil_depth_write & 0x2);
    key.stencil_enable = ctx.stencil_test_enabled && ctx.depth_format == 3;
    if (key.stencil_enable)
    {
        key.stencil_func = ctx.stencil_test_func;
        key.stencil_fail = ctx.stencil_fail_func;
        key.stencil_zfail = ctx.stencil_depth_fail_func;
        key.stencil_pass = ctx.stencil_depth_pass_func;
    }

    HwDynamicState dyn{};
    dyn.blend_const[0] = ctx.blend_color.r / 255.0f;
    dyn.blend_const[1] = ctx.blend_color.g / 255.0f;
    dyn.blend_const[2] = ctx.blend_color.b / 255.0f;
    dyn.blend_const[3] = ctx.blend_color.a / 255.0f;
    if (key.stencil_enable)
    {
        dyn.stencil_ref = ctx.stencil_ref;
        dyn.stencil_compare_mask = ctx.stencil_input_mask;
        dyn.stencil_write_mask = (ctx.allow_stencil_depth_write & 0x1) ? ctx.stencil_write_mask : 0;
    }

    if (key.color_mask)
        h.color_written = true;
    if (key.depth_write || (key.stencil_enable && dyn.stencil_write_mask))
        h.depth_written = true;

    //Texture combiners and per-fragment tests (shaders/pica.frag)
    HwUniforms u{};
    compute_texcomb_range();
    for (int i = 0; i < 6; i++)
    {
        u.tev[i][0] = ctx.texcomb_rgb_source[i][0] | (ctx.texcomb_rgb_source[i][1] << 4) |
                      (ctx.texcomb_rgb_source[i][2] << 8) | (ctx.texcomb_alpha_source[i][0] << 16) |
                      (ctx.texcomb_alpha_source[i][1] << 20) | (ctx.texcomb_alpha_source[i][2] << 24);
        u.tev[i][1] = ctx.texcomb_rgb_operand[i][0] | (ctx.texcomb_rgb_operand[i][1] << 4) |
                      (ctx.texcomb_rgb_operand[i][2] << 8) | (ctx.texcomb_alpha_operand[i][0] << 16) |
                      (ctx.texcomb_alpha_operand[i][1] << 20) | (ctx.texcomb_alpha_operand[i][2] << 24);
        u.tev[i][2] = ctx.texcomb_rgb_op[i] | (ctx.texcomb_alpha_op[i] << 8) | (ctx.texcomb_rgb_scale[i] << 16) |
                      (ctx.texcomb_alpha_scale[i] << 24);
        u.tev[i][3] = (ctx.texcomb_rgb_buffer_update[i] ? 1 : 0) | (ctx.texcomb_alpha_buffer_update[i] ? 2 : 0);
        u.tev_const[i][0] = ctx.texcomb_const[i].r;
        u.tev_const[i][1] = ctx.texcomb_const[i].g;
        u.tev_const[i][2] = ctx.texcomb_const[i].b;
        u.tev_const[i][3] = ctx.texcomb_const[i].a;
    }
    u.buffer_init[0] = ctx.texcomb_buffer.r;
    u.buffer_init[1] = ctx.texcomb_buffer.g;
    u.buffer_init[2] = ctx.texcomb_buffer.b;
    u.buffer_init[3] = ctx.texcomb_buffer.a;
    u.misc[0] = ctx.texcomb_start;
    u.misc[1] = ctx.texcomb_end;
    u.misc[2] = ctx.alpha_test_enabled ? (0x10000 | (ctx.alpha_test_func & 0xF) | (ctx.alpha_test_ref << 4)) : 0;
    u.misc[3] = ctx.use_z_for_depth;
    u.depth[0] = ctx.depth_scale.ToFloat32();
    u.depth[1] = ctx.depth_offset.ToFloat32();
    for (int i = 0; i < 3; i++)
    {
        u.tex_info[i][0] = tex_on[i];
        u.tex_info[i][1] = ctx.tex_width[i];
        u.tex_info[i][2] = ctx.tex_height[i];
        u.tex_info[i][3] = (ctx.tex_swrap[i] & 0xF) | ((ctx.tex_twrap[i] & 0xF) << 4);
        u.tex_border[i][0] = ctx.tex_border[i].r;
        u.tex_border[i][1] = ctx.tex_border[i].g;
        u.tex_border[i][2] = ctx.tex_border[i].b;
        u.tex_border[i][3] = ctx.tex_border[i].a;
    }
    u.tex_misc[0] = ctx.tex2_uses_tex1_coords;

    //Vertices: the same viewport transform as GPU::viewport_transform, but the attributes are
    //left undivided because the GPU does the perspective-correct interpolation
    float vw = ctx.viewport_width.ToFloat32(), vh = ctx.viewport_height.ToFloat32();
    float vx = ctx.viewport_x, vy = ctx.viewport_y;
    std::vector<HwVertex> sv(poly.size());
    for (size_t i = 0; i < poly.size(); i++)
    {
        Vertex& v = poly[i];
        HwVertex& o = sv[i];
        float w = v.pos[3].ToFloat32();
        float inv_w = 1.0f / w;
        float x = v.pos[0].ToFloat32() * inv_w;
        float y = v.pos[1].ToFloat32() * inv_w;
        o.pos[0] = roundf(((x + 1.0f) * vw + vx) * 16.0f) / 16.0f;
        o.pos[1] = roundf(((1.0f - y) * vh + vy) * 16.0f) / 16.0f;
        o.pos[2] = v.pos[2].ToFloat32() * inv_w;
        o.pos[3] = w;
        for (int j = 0; j < 4; j++)
            o.color[j] = v.color[j].ToFloat32();
        for (int j = 0; j < 2; j++)
        {
            o.tc0[j] = v.texcoords[0][j].ToFloat32();
            o.tc1[j] = v.texcoords[1][j].ToFloat32();
            o.tc2[j] = v.texcoords[2][j].ToFloat32();
        }
    }

    //Fan triangulation and culling, as in GPU::process_tri / rasterize_tri
    h.verts.clear();
    for (size_t i = 0; i + 2 < sv.size(); i++)
    {
        const HwVertex& a = sv[0];
        const HwVertex& b = sv[i + 2]; //rasterize_tri swaps v1 and v2 first
        const HwVertex& c = sv[i + 1];
        float orient = (b.pos[0] - a.pos[0]) * (c.pos[1] - a.pos[1]) - (c.pos[0] - a.pos[0]) * (b.pos[1] - a.pos[1]);
        if (orient < 0.0f && ctx.cull_mode && ctx.cull_mode != 1)
            continue;
        h.verts.push_back(a);
        h.verts.push_back(b);
        h.verts.push_back(c);
    }
    if (h.verts.empty())
        return;

    h.draws++;
    h.vk->draw(key, dyn, u, tex_ids, h.verts.data(), (uint32_t)h.verts.size());
}

#else // !BLOSSOM_HAS_VULKAN

void GPU::hw_end_session() {}
void GPU::hw_polygon(std::vector<Vertex>&) {}

#endif
