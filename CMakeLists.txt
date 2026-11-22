# Blossom: LLE Nintendo 3DS emulator (based on Corgi3DS, layout inspired by Azahar)
cmake_minimum_required(VERSION 3.25)

project(Blossom
    VERSION 0.1.0
    DESCRIPTION "Low-level Nintendo 3DS emulator that boots the full firmware"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/CMakeModules")

option(ENABLE_QT "Build the Qt 6 frontend" ON)
option(ENABLE_TESTS "Build the CPU test runner and register CTest tests" ON)
option(ENABLE_WERROR "Treat compiler warnings as errors" OFF)

# The dynarmic JIT only has an x86-64 backend wired up in Blossom for now.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(BLOSSOM_JIT_DEFAULT ON)
else()
    set(BLOSSOM_JIT_DEFAULT OFF)
endif()
option(ENABLE_JIT "Build the dynarmic JIT CPU backend (x86-64)" ${BLOSSOM_JIT_DEFAULT})
option(ENABLE_VULKAN "Build the Vulkan hardware renderer (needs the Vulkan headers and loader)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(GMP REQUIRED)

if (ENABLE_JIT)
    # dynarmic (vendored from azahar-emu/dynarmic, see externals/dynarmic/BLOSSOM.md)
    set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
    set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
    set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
    find_package(Boost 1.57 REQUIRED)
    add_subdirectory(externals/dynarmic EXCLUDE_FROM_ALL)
endif()

if (ENABLE_VULKAN)
    find_package(Vulkan)
    if (NOT Vulkan_FOUND)
        message(WARNING "Vulkan not found: building without the Vulkan renderer. "
                        "Install the Vulkan headers and loader (MSYS2: mingw-w64-ucrt-x86_64-vulkan-devel, "
                        "Ubuntu: libvulkan-dev) or pass -DENABLE_VULKAN=OFF.")
        set(ENABLE_VULKAN OFF)
    endif()
endif()

if (ENABLE_TESTS)
    enable_testing()
endif()

add_subdirectory(src)
