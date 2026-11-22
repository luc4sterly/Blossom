// Blossom: compares the Vulkan rasterizer against Corgi's software rasterizer.
// Each scenario sets PICA registers, draws random clipped triangles with both renderers on the
// same starting memory, and compares the color / depth / stencil buffers they leave in VRAM.
// Needs a Vulkan device (lavapipe works). Exit code 77 = skipped (no Vulkan).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "core/arm11/gpu.hpp"
#include "core/emulator.hpp"

namespace {

constexpr uint32_t COLOR = 0x18000000, DEPTH = 0x18100000, TEX0 = 0x18200000, TEX1 = 0x18240000;
constexpr int W = 64, H = 64;

uint32_t rng_state = 1;
uint32_t rnd()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
float frand(float a, float b)
{
    return a + (b - a) * (rnd() & 0xFFFFFF) / 16777215.0f;
}

uint32_t f24(float f)
{
    if (f == 0.0f)
        return 0;
    uint32_t b;
    memcpy(&b, &f, 4);
    uint32_t sign = b >> 31;
    int exp = (int)((b >> 23) & 0xFF) - 64;
    uint32_t mant = (b >> 7) & 0xFFFF;
    return (sign << 23) | ((uint32_t)exp << 16) | mant;
}

struct Scenario
{
    const char* name;
    std::function<void(GPU&)> setup;
    int triangles;
    bool perspective;
    float tc_min, tc_max;
    int color_bpp;
    std::function<void(GPU&, int)> per_triangle; //optional state change between triangles
    int lists = 1; //the triangles are split over this many command lists
    std::function<void(GPU&, Emulator&, int)> between_lists; //optional: runs after each list but the last
};

void tev_stage(GPU& g, int i, uint32_t src, uint32_t operand, uint32_t op, uint32_t cnst, uint32_t scale)
{
    int base = i < 4 ? 0xC0 + i * 8 : 0xF0 + (i - 4) * 8;
    g.test_write_reg(base + 0, src);
    g.test_write_reg(base + 1, operand);
    g.test_write_reg(base + 2, op);
    g.test_write_reg(base + 3, cnst);
    g.test_write_reg(base + 4, scale);
}

void common_setup(GPU& g, int color_fmt)
{
    g.test_write_reg(0x040, 0);                 //no culling
    g.test_write_reg(0x041, f24(W / 2.0f));
    g.test_write_reg(0x043, f24(H / 2.0f));
    g.test_write_reg(0x068, 0);
    g.test_write_reg(0x04D, f24(-1.0f));         //depth = -z
    g.test_write_reg(0x04E, 0);
    g.test_write_reg(0x06D, 1);                 //use z for depth
    g.test_write_reg(0x080, 0);                 //textures off
    for (int i = 0; i < 6; i++)
        tev_stage(g, i, 0x00F000F, 0, 0, 0, 0); //pass-through (prev)
    tev_stage(g, 0, 0, 0, 0, 0, 0);             //stage 0: primary color
    g.test_write_reg(0x0E0, 0);
    g.test_write_reg(0x0FD, 0);
    g.test_write_reg(0x100, 0);                 //logic op mode
    g.test_write_reg(0x102, 3);                 //COPY
    g.test_write_reg(0x103, 0);
    g.test_write_reg(0x104, 0);
    g.test_write_reg(0x105, 0);
    g.test_write_reg(0x106, 0);
    g.test_write_reg(0x107, 0xF00);             //color writes on, no depth test/write
    g.test_write_reg(0x115, 3);
    g.test_write_reg(0x116, 3);                 //D24S8
    g.test_write_reg(0x117, color_fmt << 16);
    g.test_write_reg(0x11C, DEPTH >> 3);
    g.test_write_reg(0x11D, COLOR >> 3);
    g.test_write_reg(0x11E, W | ((H - 1) << 12));
}

void texture_regs(GPU& g, int unit, uint32_t addr, int w, int h, int fmt, int swrap, int twrap)
{
    if (unit == 0)
    {
        g.test_write_reg(0x082, h | (w << 16));
        g.test_write_reg(0x083, (twrap << 8) | (swrap << 12));
        g.test_write_reg(0x085, addr >> 3);
        g.test_write_reg(0x08E, fmt);
        g.test_write_reg(0x081, 0xFF00FF00);
    }
    else
    {
        int b = unit == 1 ? 0x091 : 0x099;
        g.test_write_reg(b + 0, 0xFF0000FF);
        g.test_write_reg(b + 1, h | (w << 16));
        g.test_write_reg(b + 2, (twrap << 8) | (swrap << 12));
        g.test_write_reg(b + 4, addr >> 3);
        g.test_write_reg(b + 5, fmt);
    }
}

void depth_less_setup(GPU& g)
{
    common_setup(g, 0);
    g.test_write_reg(0x107, 0xF00 | 1 | (4 << 4) | (1 << 12)); //LESS, depth write
}

Vertex random_vertex(bool perspective, float tc_min, float tc_max)
{
    Vertex v;
    memset(&v, 0, sizeof(v));
    float w = perspective ? frand(0.5f, 2.0f) : 1.0f;
    v.pos[0] = float24::FromFloat32(frand(-1.3f, 1.3f) * w);
    v.pos[1] = float24::FromFloat32(frand(-1.3f, 1.3f) * w);
    v.pos[2] = float24::FromFloat32(-frand(0.05f, 0.95f) * w);
    v.pos[3] = float24::FromFloat32(w);
    for (int i = 0; i < 4; i++)
        v.color[i] = float24::FromFloat32(frand(0.0f, 1.0f));
    for (int t = 0; t < 3; t++)
        for (int i = 0; i < 2; i++)
            v.texcoords[t][i] = float24::FromFloat32(frand(tc_min, tc_max));
    return v;
}

struct Snapshot
{
    std::vector<uint8_t> color, depth;
};

Snapshot run(Emulator& e, const Scenario& s, const char* renderer, const std::vector<uint8_t>& init_vram,
             uint32_t seed)
{
    GPU& g = e.gpu_for_tests();
    if (!g.set_renderer(renderer))
    {
        fprintf(stderr, "renderer %s unavailable\n", renderer);
        exit(77);
    }
    memcpy(e.phys_ptr(0x18000000, (uint32_t)init_vram.size()), init_vram.data(), init_vram.size());
    rng_state = seed;
    s.setup(g);
    int per_list = s.triangles / std::max(s.lists, 1);
    for (int i = 0; i < s.triangles; i++)
    {
        if (s.lists > 1 && i && i % per_list == 0 && i / per_list < s.lists)
        {
            g.test_end_list();
            if (s.between_lists)
                s.between_lists(g, e, i / per_list - 1);
        }
        if (s.per_triangle)
            s.per_triangle(g, i);
        Vertex a = random_vertex(s.perspective, s.tc_min, s.tc_max);
        Vertex b = random_vertex(s.perspective, s.tc_min, s.tc_max);
        Vertex c = random_vertex(s.perspective, s.tc_min, s.tc_max);
        g.test_triangle(a, b, c);
    }
    g.test_end_list();
    Snapshot snap;
    uint8_t* cp = e.phys_ptr(COLOR, W * H * 4);
    uint8_t* dp = e.phys_ptr(DEPTH, W * H * 4);
    snap.color.assign(cp, cp + W * H * s.color_bpp);
    snap.depth.assign(dp, dp + W * H * 4);
    return snap;
}

bool compare(const Scenario& s, const Snapshot& sw, const Snapshot& hw)
{
    //Color: a pixel is "off" if any byte differs by more than 8 (rounding between float24 and
    //float32 math, and edge pixels where the fill rules differ, are expected)
    int bpp = s.color_bpp, off = 0, exact = 0;
    int max_diff = 0;
    for (int p = 0; p < W * H; p++)
    {
        int d = 0;
        for (int b = 0; b < bpp; b++)
            d = std::max(d, std::abs((int)sw.color[p * bpp + b] - (int)hw.color[p * bpp + b]));
        if (d > 8)
            off++;
        if (!d)
            exact++;
        max_diff = std::max(max_diff, d);
    }
    int depth_off = 0, stencil_off = 0;
    for (int p = 0; p < W * H; p++)
    {
        uint32_t a, b;
        memcpy(&a, &sw.depth[p * 4], 4);
        memcpy(&b, &hw.depth[p * 4], 4);
        if (std::abs((int)(a & 0xFFFFFF) - (int)(b & 0xFFFFFF)) > 0x800)
            depth_off++;
        if ((a >> 24) != (b >> 24))
            stencil_off++;
    }
    double pct = 100.0 * off / (W * H);
    double dpct = 100.0 * depth_off / (W * H);
    double spct = 100.0 * stencil_off / (W * H);
    bool ok = pct <= 2.0 && dpct <= 2.0 && spct <= 2.0;
    printf("%-18s %s  color: %5.2f%% off, %5.1f%% exact, max diff %3d | depth %5.2f%% off | stencil %5.2f%% off\n",
           s.name, ok ? "OK  " : "FAIL", pct, 100.0 * exact / (W * H), max_diff, dpct, spct);
    return ok;
}

void write_ppm(const char* path, const std::vector<uint8_t>& color)
{
    //RGBA8 framebuffer, tiled: decode for viewing
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    static const uint32_t xlut[] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const uint32_t ylut[] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            uint32_t offs = ((x & ~7) * 8) + ((y & ~7) * W) + xlut[x % 8] + ylut[y % 8];
            const uint8_t* p = &color[offs * 4];
            uint8_t rgb[3] = {p[3], p[2], p[1]};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}


//--bench: a menu-like load (400x240 target, textured small quads, 4 command lists a frame) to
//time the CPU side of the Vulkan path. Not part of the comparison.
int bench(Emulator& e, const char* renderer, int frames)
{
    GPU& g = e.gpu_for_tests();
    if (!g.set_renderer(renderer))
        return 77;
    const int BW = 400, BH = 240;
    uint8_t* vram = e.phys_ptr(0x18000000, 0x300000);
    for (uint32_t i = 0; i < 0x300000; i++)
        vram[i] = rnd() & 0xFF;
    common_setup(g, 0);
    g.test_write_reg(0x041, f24(BW / 2.0f));
    g.test_write_reg(0x043, f24(BH / 2.0f));
    g.test_write_reg(0x11E, BW | ((BH - 1) << 12));
    g.test_write_reg(0x080, 1);
    texture_regs(g, 0, TEX0, 256, 256, 0, 2, 2);
    tev_stage(g, 0, 0x30 | (0x30 << 16), 0, 1 | (1 << 16), 0, 0);
    g.test_write_reg(0x100, 0x100);
    g.test_write_reg(0x101, 0 | (0 << 8) | (6 << 16) | (7 << 20));
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; f++)
    {
        for (int list = 0; list < 4; list++)
        {
            for (int q = 0; q < 1000; q++)
            {
                float x = frand(-1.0f, 0.9f), y = frand(-1.0f, 0.9f), s = 0.08f;
                Vertex v[3];
                for (int k = 0; k < 3; k++)
                {
                    v[k] = random_vertex(false, 0, 1);
                    v[k].pos[2] = float24::FromFloat32(-0.5f);
                }
                v[0].pos[0] = float24::FromFloat32(x); v[0].pos[1] = float24::FromFloat32(y);
                v[1].pos[0] = float24::FromFloat32(x + s); v[1].pos[1] = float24::FromFloat32(y);
                v[2].pos[0] = float24::FromFloat32(x); v[2].pos[1] = float24::FromFloat32(y + s);
                g.test_triangle(v[0], v[1], v[2]);
            }
            g.test_end_list();
        }
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (frames == 60)
        g.print_debug_stats(60); //with BLOSSOM_TRACE_GPU=1: the per-phase times
    printf("[bench] %s: %.2f ms per frame (4 lists x 1000 textured triangles, 400x240)\n", renderer, ms / frames);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "--bench"))
    {
        Emulator e;
        e.reset();
        int frames = argc > 2 ? atoi(argv[2]) : 20;
        int r = bench(e, "vulkan", frames);
        if (argc > 3)
            bench(e, "software", frames);
        e.gpu_for_tests().set_renderer("software");
        return r;
    }

    const char* dump_dir = argc > 1 ? argv[1] : nullptr;
    Emulator e;
    e.reset();

    //Starting VRAM: random color/depth/stencil and textures
    std::vector<uint8_t> vram(0x300000);
    rng_state = 12345;
    for (auto& b : vram)
        b = rnd() & 0xFF;

    std::vector<Scenario> scenarios = {
        {"gouraud", [](GPU& g) { common_setup(g, 0); }, 24, false, 0, 1, 4, nullptr},
        {"perspective", [](GPU& g) { common_setup(g, 0); }, 24, true, 0, 1, 4, nullptr},
        {"tex-modulate-blend", [](GPU& g) {
            common_setup(g, 0);
            g.test_write_reg(0x080, 1);
            texture_regs(g, 0, TEX0, 64, 32, 0, 2, 3); //RGBA8, repeat / mirror
            tev_stage(g, 0, 0x30 | (0x30 << 16), 0, 1 | (1 << 16), 0, 0); //primary * tex0
            g.test_write_reg(0x100, 0x100);
            g.test_write_reg(0x101, 0 | (0 << 8) | (6 << 16) | (7 << 20) | (1 << 24) | (0u << 28));
        }, 20, true, -1.0f, 2.0f, 4, nullptr},
        {"depth-565", [](GPU& g) {
            common_setup(g, 3);
            g.test_write_reg(0x107, 0xF00 | 1 | (4 << 4) | (1 << 12)); //LESS, depth write
        }, 30, true, 0, 1, 2, nullptr},
        {"stencil-alpha", [](GPU& g) {
            common_setup(g, 0);
            g.test_write_reg(0x104, 1 | (6 << 4) | (0x40 << 8)); //alpha > 0x40
        }, 20, false, 0, 1, 4, [](GPU& g, int i) {
            if (i < 10)
            {
                g.test_write_reg(0x105, 1 | (1 << 4) | (0xFF << 8) | (0x5A << 16) | (0xFFu << 24)); //ALWAYS
                g.test_write_reg(0x106, 0 | (0 << 4) | (2 << 8)); //pass: REPLACE
            }
            else
            {
                g.test_write_reg(0x105, 1 | (2 << 4) | (0x0F << 8) | (0x5A << 16) | (0xFFu << 24)); //EQUAL
                g.test_write_reg(0x106, 3 | (0 << 4) | (6 << 8)); //fail: INCR, pass: INCR_WRAP
            }
        }},
        {"xor-4444-tev", [](GPU& g) {
            common_setup(g, 0);
            g.test_write_reg(0x102, 11);          //XOR
            g.test_write_reg(0x080, 1 | 2);
            texture_regs(g, 0, TEX0, 32, 32, 4, 0, 1); //RGBA4444, clamp / border
            texture_regs(g, 1, TEX1, 16, 64, 7, 2, 2); //I8
            g.test_write_reg(0x0FD, 0x80402010);
            g.test_write_reg(0x0E0, (1 << 8) | (1 << 12));
            //stage 0: tex0 * const, x2 ; stage 1: prev + buffer ; stage 2: lerp(prev, tex1, primary.a)
            tev_stage(g, 0, 0x3E | (0x3E << 16), 0, 1 | (1 << 16), 0x80C0E0F0, 1 | (1 << 16));
            tev_stage(g, 1, 0xDF | (0xDF << 16), 0, 2 | (2 << 16), 0, 0);
            tev_stage(g, 2, 0x04F | (0x04F << 16) | (0 << 8), 0x200, 4 | (4 << 16), 0, 0);
        }, 20, true, -0.5f, 1.5f, 4, nullptr},
        {"logic-xor", [](GPU& g) {
            common_setup(g, 0);
            g.test_write_reg(0x102, 11);
        }, 20, false, 0, 1, 4, nullptr},
        {"tex-4444-border", [](GPU& g) {
            common_setup(g, 0);
            g.test_write_reg(0x080, 1);
            texture_regs(g, 0, TEX0, 32, 32, 4, 0, 1);
            tev_stage(g, 0, 0x3 | (0x3 << 16), 0, 0, 0, 0);
        }, 20, true, -0.5f, 1.5f, 4, nullptr},
        {"tex1-i8", [](GPU& g) {
            common_setup(g, 0);
            g.test_write_reg(0x080, 2);
            texture_regs(g, 1, TEX1, 16, 64, 7, 2, 2);
            tev_stage(g, 0, 0x4 | (0x4 << 16), 0, 0, 0, 0);
        }, 20, true, -0.5f, 1.5f, 4, nullptr},
        {"tev-chain", [](GPU& g) {
            common_setup(g, 0);
            g.test_write_reg(0x0FD, 0x80402010);
            g.test_write_reg(0x0E0, (1 << 8) | (1 << 12));
            tev_stage(g, 0, 0xE0 | (0xE0 << 16), 0, 1 | (1 << 16), 0x80C0E0F0, 1 | (1 << 16));
            tev_stage(g, 1, 0xDF | (0xDF << 16), 0, 2 | (2 << 16), 0, 0);
        }, 20, false, 0, 1, 4, nullptr},
        {"cull-ccw", [](GPU& g) {
            common_setup(g, 2);
            g.test_write_reg(0x040, 2);
        }, 24, false, 0, 1, 2, nullptr},
        //Several command lists on the same buffers: the Vulkan target is kept between them while
        //memory is unchanged, and must be uploaded again when the CPU changed it or filled it
        {"lists-kept", depth_less_setup, 36, true, 0, 1, 4, nullptr, 4, nullptr},
        {"lists-cpu-write", depth_less_setup, 36, true, 0, 1, 4, nullptr, 4, [](GPU&, Emulator& e, int list) {
            uint8_t* c = e.phys_ptr(COLOR, W * H * 4);
            uint8_t* d = e.phys_ptr(DEPTH, W * H * 4);
            for (int i = 0; i < W * H * 4; i += 7 + list)
            {
                c[i] ^= 0x5A;
                d[i] ^= 0xA5;
            }
        }},
        {"lists-fill", depth_less_setup, 36, true, 0, 1, 4, nullptr, 4, [](GPU&, Emulator& e, int list) {
            uint8_t* c = e.phys_ptr(COLOR, W * H * 4);
            uint8_t* d = e.phys_ptr(DEPTH, W * H * 4);
            for (int i = 0; i < W * H; i++)
            {
                uint32_t cv = 0x20406080u + list, dv = 0x00C00000u | ((uint32_t)list << 24);
                memcpy(c + i * 4, &cv, 4);
                memcpy(d + i * 4, &dv, 4);
            }
        }},
        {"lists-two-targets", depth_less_setup, 36, true, 0, 1, 4, nullptr, 4, [](GPU& g, Emulator&, int list) {
            //Odd lists draw to another color buffer, so COLOR alternates between being drawn and not
            g.test_write_reg(0x11D, (list % 2 == 0 ? COLOR + 0x40000 : COLOR) >> 3);
        }},
    };

    int failures = 0;
    for (size_t i = 0; i < scenarios.size(); i++)
    {
        Snapshot sw = run(e, scenarios[i], "software", vram, 1000 + (uint32_t)i);
        Snapshot hw = run(e, scenarios[i], "vulkan", vram, 1000 + (uint32_t)i);
        if (!compare(scenarios[i], sw, hw))
            failures++;
        if (dump_dir && scenarios[i].color_bpp == 4)
        {
            write_ppm((std::string(dump_dir) + "/" + scenarios[i].name + "_sw.ppm").c_str(), sw.color);
            write_ppm((std::string(dump_dir) + "/" + scenarios[i].name + "_vk.ppm").c_str(), hw.color);
        }
    }
    e.gpu_for_tests().set_renderer("software");
    printf("%d of %zu scenarios failed\n", failures, scenarios.size());
    return failures ? 1 : 0;
}
