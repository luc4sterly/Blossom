// Blossom: fragment stage of the Vulkan PICA200 rasterizer.
// An uber-shader: the texture combiner (TEV) setup comes from a uniform buffer, so one pipeline
// covers every combiner configuration. The math is done in integers and mirrors
// GPU::combine_textures / GPU::tex_lookup in gpu.cpp, so it matches the software renderer.
// Blending, logic ops, depth and stencil are fixed-function pipeline state.
// Layout inspired by Azahar's PICA fragment shader generator (GPLv3).
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_tc0;
layout(location = 2) in vec2 v_tc1;
layout(location = 3) in vec2 v_tc2;
layout(location = 4) noperspective in float v_zw;

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0, std140) uniform DrawUniforms {
    uvec4 tev[6];       // x: sources (rgb 0-11, alpha 16-27), y: operands (same), z: ops | scales
    ivec4 tev_const[6];
    ivec4 buffer_init;
    ivec4 misc;         // x: first stage, y: end stage, z: alpha test, w: use z for depth
    vec4 depth;         // x: scale, y: offset
    ivec4 tex_info[3];  // x: enabled, y: width, z: height, w: swrap | twrap << 4
    ivec4 tex_border[3];
    ivec4 tex_misc;     // x: texture 2 uses texture 1 coordinates
} u;

layout(set = 0, binding = 1) uniform sampler2D tex0;
layout(set = 0, binding = 2) uniform sampler2D tex1;
layout(set = 0, binding = 3) uniform sampler2D tex2;

int wrap_coord(int c, int size, int mode, out bool border)
{
    border = false;
    if (mode == 0)
        return clamp(c, 0, size - 1);
    if (mode == 1)
    {
        border = c < 0 || c >= size;
        return c;
    }
    if (mode == 2)
        return int(uint(c) % uint(size));
    uint t = uint(c) % uint(size * 2);
    if (t >= uint(size))
        t = uint(size * 2) - 1u - t;
    return int(t);
}

ivec4 fetch_tex(int i, vec2 tc)
{
    ivec4 info = u.tex_info[i];
    if (info.x == 0)
        return ivec4(0);
    int w = info.y;
    int h = info.z;
    bool border_s, border_t;
    int s = wrap_coord(int(tc.x * float(w)), w, info.w & 0xF, border_s);
    if (border_s)
        return u.tex_border[i];
    int t = wrap_coord(int(tc.y * float(h)), h, (info.w >> 4) & 0xF, border_t);
    if (border_t)
        return u.tex_border[i];
    //Texcoords are vertically flipped
    t = h - 1 - t;
    vec4 c;
    if (i == 0)
        c = texelFetch(tex0, ivec2(s, t), 0);
    else if (i == 1)
        c = texelFetch(tex1, ivec2(s, t), 0);
    else
        c = texelFetch(tex2, ivec2(s, t), 0);
    return ivec4(round(c * 255.0));
}

ivec4 primary, prev, comb;
ivec4 tex[3];

ivec4 get_source(uint src, int stage)
{
    switch (src)
    {
        case 0u: case 1u: case 2u: return primary;
        case 3u: return tex[0];
        case 4u: return tex[1];
        case 5u: return tex[2];
        case 13u: return comb;
        case 14u: return u.tev_const[stage];
        case 15u: return prev;
    }
    return ivec4(0);
}

ivec3 rgb_operand(ivec4 s, uint op)
{
    switch (op)
    {
        case 0u: return s.rgb;
        case 1u: return 255 - s.rgb;
        case 2u: return ivec3(s.a);
        case 3u: return ivec3(255 - s.a);
        case 4u: return ivec3(s.r);
        case 5u: return ivec3(255 - s.r);
        case 8u: return ivec3(s.g);
        case 9u: return ivec3(255 - s.g);
        case 12u: return ivec3(s.b);
        case 13u: return ivec3(255 - s.b);
    }
    return ivec3(0);
}

int alpha_operand(ivec4 s, uint op)
{
    switch (op)
    {
        case 0u: return s.a;
        case 1u: return 255 - s.a;
        case 2u: return s.r;
        case 3u: return 255 - s.r;
        case 4u: return s.g;
        case 5u: return 255 - s.g;
        case 6u: return s.b;
        case 7u: return 255 - s.b;
    }
    return 0;
}

ivec3 combine_rgb(uint op, ivec3 a, ivec3 b, ivec3 c)
{
    switch (op)
    {
        case 0u: return a;
        case 1u: return (a * b) / 255;
        case 2u: return min(ivec3(255), a + b);
        case 3u: return clamp(a + b - 128, 0, 255);
        case 4u: return (a * c + b * (255 - c)) / 255;
        case 5u: return max(ivec3(0), a - b);
        case 8u: return min(ivec3(255), (a * b + 255 * c) / 255);
        case 9u: return (min(ivec3(255), a + b) * c) / 255;
    }
    return ivec3(0);
}

int combine_alpha(uint op, int a, int b, int c)
{
    switch (op)
    {
        case 0u: return a;
        case 1u: return (a * b) / 255;
        case 2u: return min(255, a + b);
        case 3u: return clamp(a + b - 128, 0, 255);
        case 4u: return (a * c + b * (255 - c)) / 255;
        case 5u: return max(0, a - b);
        case 8u: return min(255, (a * b + 255 * c) / 255);
        case 9u: return (min(255, a + b) * c) / 255;
    }
    return 0;
}

bool compare(uint func, int a, int b)
{
    switch (func)
    {
        case 0u: return false;
        case 1u: return true;
        case 2u: return a == b;
        case 3u: return a != b;
        case 4u: return a < b;
        case 5u: return a <= b;
        case 6u: return a > b;
        case 7u: return a >= b;
    }
    return true;
}

void main()
{
    primary = ivec4(round(clamp(v_color, 0.0, 1.0) * 255.0));
    prev = primary;
    comb = ivec4(0);
    ivec4 next_comb = u.buffer_init;

    tex[0] = fetch_tex(0, v_tc0);
    tex[1] = fetch_tex(1, v_tc1);
    tex[2] = fetch_tex(2, u.tex_misc.x != 0 ? v_tc1 : v_tc2);

    for (int i = u.misc.x; i < u.misc.y; i++)
    {
        uvec4 cfg = u.tev[i];
        ivec3 rgb_ops[3];
        int alpha_ops[3];
        for (int j = 0; j < 3; j++)
        {
            ivec4 rgb_src = get_source((cfg.x >> uint(4 * j)) & 0xFu, i);
            ivec4 alpha_src = get_source((cfg.x >> uint(16 + 4 * j)) & 0xFu, i);
            rgb_ops[j] = rgb_operand(rgb_src, (cfg.y >> uint(4 * j)) & 0xFu);
            alpha_ops[j] = alpha_operand(alpha_src, (cfg.y >> uint(16 + 4 * j)) & 0xFu);
        }

        ivec3 rgb = combine_rgb(cfg.z & 0xFu, rgb_ops[0], rgb_ops[1], rgb_ops[2]);
        int alpha = combine_alpha((cfg.z >> 8u) & 0xFu, alpha_ops[0], alpha_ops[1], alpha_ops[2]);

        uint rgb_scale = (cfg.z >> 16u) & 0x3u;
        uint alpha_scale = (cfg.z >> 24u) & 0x3u;
        if (rgb_scale == 1u || rgb_scale == 2u)
            rgb = min(ivec3(255), rgb << int(rgb_scale));
        if (alpha_scale == 1u || alpha_scale == 2u)
            alpha = min(255, alpha << int(alpha_scale));

        prev = ivec4(rgb, alpha);

        comb = next_comb;
        if ((cfg.w & 1u) != 0u)
            next_comb.rgb = prev.rgb;
        if ((cfg.w & 2u) != 0u)
            next_comb.a = prev.a;
    }

    int alpha_test = u.misc.z;
    if ((alpha_test & 0x10000) != 0 && !compare(uint(alpha_test & 0xF), prev.a, (alpha_test >> 4) & 0xFF))
        discard;

    float depth = v_zw * u.depth.x + u.depth.y;
    if (u.misc.w == 0)
        depth /= gl_FragCoord.w;
    gl_FragDepth = clamp(depth, 0.0, 1.0);

    o_color = vec4(prev) / 255.0;
}
