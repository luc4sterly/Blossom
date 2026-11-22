add_library(blossom_common STATIC
    bswp.cpp
    bswp.hpp
    common.hpp
    exceptions.cpp
    exceptions.hpp
    log.cpp
    log.hpp
    native_stack.cpp
    native_stack.hpp
    profiler.cpp
    profiler.hpp
    rotr.cpp
    rotr.hpp
)
target_include_directories(blossom_common PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(blossom_common PRIVATE blossom_warnings)
if(WIN32)
    target_link_libraries(blossom_common PRIVATE psapi)
endif()
