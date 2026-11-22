#!/bin/sh
# Regenerates the SPIR-V headers of the Vulkan renderer. Needs glslangValidator
# (Ubuntu: glslang-tools, MSYS2: mingw-w64-ucrt-x86_64-glslang).
set -e
dir="$(cd "$(dirname "$0")/../src/video_core/renderer_vulkan" && pwd)"
glslangValidator -V --vn pica_vert_spv -o "$dir/pica_vert.spv.h" "$dir/shaders/pica.vert"
glslangValidator -V --vn pica_frag_spv -o "$dir/pica_frag.spv.h" "$dir/shaders/pica.frag"
