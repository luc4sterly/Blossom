# Vulkan hardware rasterizer for the PICA200 (structure adapted from Azahar's renderer_vulkan).
# The SPIR-V headers are generated from renderer_vulkan/shaders by tools/gen_vk_shaders.sh and
# committed, so building Blossom only needs the Vulkan headers and loader, not a shader compiler.
add_library(blossom_video_vulkan STATIC
    renderer_vulkan/vk_rasterizer.cpp
    renderer_vulkan/vk_rasterizer.hpp
    renderer_vulkan/pica_vert.spv.h
    renderer_vulkan/pica_frag.spv.h
)
target_include_directories(blossom_video_vulkan PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(blossom_video_vulkan PUBLIC Vulkan::Vulkan PRIVATE blossom_warnings)
if (NOT MSVC)
    # Vulkan structs are zero-initialised with {sType}, which is the intended idiom
    target_compile_options(blossom_video_vulkan PRIVATE -Wno-missing-field-initializers)
endif()
