# Finds GMP and its C++ bindings (gmpxx) and exposes the GMP::GMP target.
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(PC_GMPXX QUIET gmpxx)
endif()

find_path(GMP_INCLUDE_DIR NAMES gmpxx.h HINTS ${PC_GMPXX_INCLUDE_DIRS})
find_library(GMP_LIBRARY NAMES gmp HINTS ${PC_GMPXX_LIBRARY_DIRS})
find_library(GMPXX_LIBRARY NAMES gmpxx HINTS ${PC_GMPXX_LIBRARY_DIRS})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GMP REQUIRED_VARS GMP_INCLUDE_DIR GMP_LIBRARY GMPXX_LIBRARY)

if (GMP_FOUND AND NOT TARGET GMP::GMP)
    add_library(GMP::GMP INTERFACE IMPORTED)
    target_include_directories(GMP::GMP INTERFACE "${GMP_INCLUDE_DIR}")
    target_link_libraries(GMP::GMP INTERFACE "${GMPXX_LIBRARY}" "${GMP_LIBRARY}")
endif()

mark_as_advanced(GMP_INCLUDE_DIR GMP_LIBRARY GMPXX_LIBRARY)
