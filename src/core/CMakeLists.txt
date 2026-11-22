add_library(blossom_core STATIC
    arm11/dsp.cpp
    arm11/dsp.hpp
    arm11/dsp_interpreter.cpp
    arm11/dsp_interpreter.hpp
    arm11/dsp_reg.hpp
    arm11/gpu.cpp
    arm11/gpu.hpp
    arm11/gpu_hw.cpp
    arm11/gpu_shader.cpp
    arm11/gpu_floats.hpp
    arm11/hash.cpp
    arm11/hash.hpp
    arm11/mpcore_pmr.cpp
    arm11/mpcore_pmr.hpp
    arm11/signextend.hpp
    arm11/vector_math.hpp
    arm11/wifi.cpp
    arm11/wifi.hpp
    arm11/wifi_timers.cpp
    arm11/wifi_timers.hpp
    arm11/xtensa.cpp
    arm11/xtensa.hpp
    arm11/xtensa_interpreter.cpp
    arm11/xtensa_interpreter.hpp
    arm9/aes.cpp
    arm9/aes.hpp
    arm9/aes_lib.c
    arm9/aes_lib.h
    arm9/aes_lib.hpp
    arm9/cartridge.cpp
    arm9/cartridge.hpp
    arm9/dma9.cpp
    arm9/dma9.hpp
    arm9/emmc.cpp
    arm9/emmc.hpp
    arm9/interrupt9.cpp
    arm9/interrupt9.hpp
    arm9/rsa.cpp
    arm9/rsa.hpp
    arm9/sha.cpp
    arm9/sha.hpp
    corelink_dma.cpp
    corelink_dma.hpp
    cpu/arm.cpp
    cpu/arm.hpp
    cpu/arm_disasm.cpp
    cpu/arm_disasm.hpp
    cpu/arm_interpret.cpp
    cpu/arm_interpret.hpp
    cpu/cp15.cpp
    cpu/cp15.hpp
    cpu/mmu.cpp
    cpu/mmu.hpp
    cpu/page_regions.hpp
    cpu/thumb_disasm.cpp
    cpu/thumb_interpret.cpp
    cpu/vfp.cpp
    cpu/vfp.hpp
    cpu/vfp_disasm.cpp
    cpu/vfp_interpreter.cpp
    emulator.cpp
    emulator.hpp
    i2c.cpp
    i2c.hpp
    p9_hle.cpp
    p9_hle.hpp
    pxi.cpp
    pxi.hpp
    scheduler.cpp
    scheduler.hpp
    sha_engine.cpp
    sha_engine.hpp
    spi.cpp
    spi.hpp
    timers.cpp
    timers.hpp
)
target_link_libraries(blossom_core
    PUBLIC blossom_common GMP::GMP
    PRIVATE blossom_warnings
)

if (ENABLE_JIT)
    target_sources(blossom_core PRIVATE cpu/arm_jit.cpp cpu/arm_jit.hpp)
    target_link_libraries(blossom_core PRIVATE dynarmic)
    target_compile_definitions(blossom_core PUBLIC BLOSSOM_HAS_JIT=1)
endif()

if (ENABLE_VULKAN)
    target_link_libraries(blossom_core PRIVATE blossom_video_vulkan)
    target_compile_definitions(blossom_core PUBLIC BLOSSOM_HAS_VULKAN=1)
endif()

find_package(Threads REQUIRED)
target_link_libraries(blossom_core PUBLIC Threads::Threads)
