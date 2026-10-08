if (NOT MSVC)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

set(SRCS
  TracyClient.cpp
  common/TracySystem.cpp
  common/tracy_lz4.cpp
  common/TracySocket.cpp
  common/TracyStackFrames.cpp
  client/TracyProfiler.cpp
  client/TracyCallstack.cpp
  client/TracySysPower.cpp
  client/TracySysTime.cpp
  client/TracySysTrace.cpp
  client/tracy_rpmalloc.cpp
  client/TracyDxt1.cpp
  client/TracyAlloc.cpp
  client/TracyOverride.cpp
)

# Only include platform-specific backtrace files on non-Windows platforms
# Windows uses Windows-native stack walking
if(NOT WIN32)
  list(APPEND SRCS
    libbacktrace/alloc.cpp
    libbacktrace/dwarf.cpp
    libbacktrace/fileline.cpp
    libbacktrace/mmapio.cpp
    libbacktrace/posix.cpp
    libbacktrace/sort.cpp
    libbacktrace/state.cpp
  )

  # On macOS, include macho.cpp for backtraces
  if(APPLE)
    list(APPEND SRCS libbacktrace/macho.cpp)
  else()
    # On Linux and other POSIX systems, include elf.cpp
    list(APPEND SRCS libbacktrace/elf.cpp)
  endif()
endif()

add_library(tracy STATIC ${SRCS})
dolphin_disable_warnings(tracy)

# PUBLIC include directories are exposed to libraries that link to tracy
# The .. makes Externals the base, so #include <tracy/Tracy.hpp> works
target_include_directories(tracy
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(tracy
  PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
# Without TRACY_ENABLE the client compiles to nothing.
if(ENABLE_TRACY)
  target_compile_definitions(tracy PUBLIC TRACY_ENABLE=1)
endif()

# Link against dbghelp.lib on Windows for callstack support
if(WIN32)
  target_link_libraries(tracy PUBLIC dbghelp)
endif()

# Suppress warnings for Tracy's code
target_compile_options(tracy PRIVATE -w)
