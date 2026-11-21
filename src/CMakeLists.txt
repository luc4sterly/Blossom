# Shared warning flags for every Blossom target.
add_library(blossom_warnings INTERFACE)
if (MSVC)
    target_compile_options(blossom_warnings INTERFACE /W4)
    if (ENABLE_WERROR)
        target_compile_options(blossom_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(blossom_warnings INTERFACE -Wall -Wextra)
    if (ENABLE_WERROR)
        target_compile_options(blossom_warnings INTERFACE -Werror)
    endif()
endif()

add_subdirectory(common)
add_subdirectory(core)

if (ENABLE_QT)
    add_subdirectory(blossom_qt)
endif()

if (ENABLE_TESTS)
    add_subdirectory(tests)
endif()
