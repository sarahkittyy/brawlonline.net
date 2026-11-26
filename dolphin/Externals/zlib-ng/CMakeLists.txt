set(ZLIB_ENABLE_TESTS OFF)
set(ZLIB_COMPAT ON)
set(SKIP_INSTALL_ALL ON)

option(BUILD_SHARED_LIBS "Build shared library" OFF)

add_subdirectory(zlib-ng)

# zlib-ng's own x86_features.c turns AVX2 and AVX-512 on from CPUID alone, without asking
# whether the OS saves those registers (XCR0); a PC whose Windows leaves AVX-512 state off
# then crashed with an illegal instruction at boot (bug report #362). Build our copy, which
# checks XCR0, in its place.
get_target_property(_zlib_sources zlib SOURCES)
if(_zlib_sources MATCHES "x86_features\\.c")
  list(TRANSFORM _zlib_sources REPLACE ".*x86_features\\.c$" "${CMAKE_CURRENT_SOURCE_DIR}/x86_features.c")
  set_target_properties(zlib PROPERTIES SOURCES "${_zlib_sources}")
endif()

# Set ZLIB variables for find_package used by other projects
set(ZLIB_INCLUDE_DIR ${CMAKE_BINARY_DIR}/zlib-ng CACHE STRING "Path to zlib include directory")
set(ZLIB_LIBRARY ZLIB::ZLIB CACHE STRING "Path to zlib library")

# Setup zlib alias project so FindZLIB doesn't recreate it
add_library(ZLIB::ZLIB ALIAS zlib)
dolphin_disable_warnings(zlib)
