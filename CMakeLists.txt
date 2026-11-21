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

if (ENABLE_TESTS)
    enable_testing()
endif()

add_subdirectory(src)
