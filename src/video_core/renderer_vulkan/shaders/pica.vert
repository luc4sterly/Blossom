// Blossom: vertex stage of the Vulkan PICA200 rasterizer.
// The PICA vertex shader, clipping and viewport transform still run on the CPU (gpu.cpp); this
// stage only turns screen-space vertices back into clip space so the GPU interpolates the
// attributes with perspective correction, the same way GPU::rasterize_tri does.
#version 450

layout(location = 0) in vec4 a_pos;   // x, y in pixels; z/w; w
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_tc0;
layout(location = 3) in vec2 a_tc1;
layout(location = 4) in vec2 a_tc2;

layout(push_constant) uniform PushConstants {
    vec2 scale; // 2 / framebuffer size
} pc;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_tc0;
layout(location = 2) out vec2 v_tc1;
layout(location = 3) out vec2 v_tc2;
layout(location = 4) noperspective out float v_zw;

void main()
{
    float w = a_pos.w;
    vec2 ndc = a_pos.xy * pc.scale - 1.0;
    gl_Position = vec4(ndc * w, 0.5 * w, w);
    v_color = a_color;
    v_tc0 = a_tc0;
    v_tc1 = a_tc1;
    v_tc2 = a_tc2;
    v_zw = a_pos.z;
}
