cmake_minimum_required(VERSION 3.13)

# for revision info
if(GIT_FOUND)
  # defines DOLPHIN_WC_REVISION
  execute_process(WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
      OUTPUT_VARIABLE DOLPHIN_WC_REVISION
      OUTPUT_STRIP_TRAILING_WHITESPACE)
  # defines DOLPHIN_WC_DESCRIBE
  execute_process(WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} describe --always --long # --dirty # removed from here while we're using a patch for SDL; will likely be fixed in the future 
      OUTPUT_VARIABLE DOLPHIN_WC_DESCRIBE
      OUTPUT_STRIP_TRAILING_WHITESPACE)

  # remove hash (and trailing "-0" if needed) from description
  string(REGEX REPLACE "(-0)?-[^-]+((-dirty)?)$" "\\2" DOLPHIN_WC_DESCRIBE "${DOLPHIN_WC_DESCRIBE}")

  # defines DOLPHIN_WC_BRANCH
  execute_process(WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
      OUTPUT_VARIABLE DOLPHIN_WC_BRANCH
      OUTPUT_STRIP_TRAILING_WHITESPACE)
  # defines DOLPHIN_WC_COMMITS_AHEAD_MASTER
  execute_process(WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD ^master
      OUTPUT_VARIABLE DOLPHIN_WC_COMMITS_AHEAD_MASTER
      OUTPUT_STRIP_TRAILING_WHITESPACE)

  # defines DOLPHIN_WC_TAG
  execute_process(WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} describe --exact-match HEAD
    OUTPUT_VARIABLE DOLPHIN_WC_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()

string(TIMESTAMP DOLPHIN_WC_BUILD_DATE "%Y-%m-%d" UTC)

# version number
set(DOLPHIN_VERSION_MAJOR "2606")
set(DOLPHIN_VERSION_MINOR "0")
set(DOLPHIN_VERSION_PATCH ${DOLPHIN_WC_REVISION})

# If Dolphin is not built from a Git repository, default the version info to
# reasonable values.
if(NOT DOLPHIN_WC_REVISION)
  set(DOLPHIN_WC_DESCRIBE "${DOLPHIN_VERSION_MAJOR}.${DOLPHIN_VERSION_MINOR}")
  set(DOLPHIN_WC_REVISION "${DOLPHIN_WC_DESCRIBE} (no further info)")
  set(DOLPHIN_WC_BRANCH "master")
  set(DOLPHIN_WC_COMMITS_AHEAD_MASTER 0)
endif()

# If this is a tag (i.e. a release), then set the current patch version and
# the number of commits ahead to zero.
if(DOLPHIN_WC_TAG)
  set(DOLPHIN_VERSION_PATCH "0")
  set(DOLPHIN_WC_COMMITS_AHEAD_MASTER 0)
endif()

# DOLPHIN_SHADER_COMPILER_HASH: the code that turns a GX shader's generated source into what the
# shader cache keeps on disk (the backends with a disk cache, the GLSL -> SPIR-V -> HLSL/MSL tools
# and their versions). The cache's entries are fingerprinted with it and their source
# (VideoCommon/ShaderCache.cpp), so a new build keeps the shaders that compile as before: any
# change here compiles them again. Without git for the tools' versions it is this build's revision,
# which keeps nothing from other builds.
set(_shader_compiler_globs
  "Source/Core/VideoBackends/D3D/*"
  "Source/Core/VideoBackends/D3D12/*"
  "Source/Core/VideoBackends/D3DCommon/*"
  "Source/Core/VideoBackends/OGL/*"
  "Source/Core/VideoBackends/Vulkan/*"
  "Source/Core/Common/GL/*"
  "Source/Core/Common/LinearDiskCache.h"
  "Source/Core/VideoCommon/Spirv.*"
  "Source/Core/VideoCommon/ShaderCompileUtils.*"
  "Source/Core/VideoCommon/DriverDetails.*"
  "Externals/glslang/CMakeLists.txt"
  "Externals/spirv_cross/CMakeLists.txt"
)
set(_shader_compiler_files "")
foreach(_glob IN LISTS _shader_compiler_globs)
  file(GLOB_RECURSE _found LIST_DIRECTORIES false RELATIVE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/${_glob}")
  list(APPEND _shader_compiler_files ${_found})
endforeach()
list(REMOVE_DUPLICATES _shader_compiler_files)
list(SORT _shader_compiler_files)
set(_shader_compiler_input "")
foreach(_file IN LISTS _shader_compiler_files)
  file(SHA256 "${PROJECT_SOURCE_DIR}/${_file}" _file_hash)
  string(APPEND _shader_compiler_input "${_file} ${_file_hash}
")
endforeach()
set(_shader_compiler_tools_known TRUE)
foreach(_tool IN ITEMS "Externals/glslang/glslang" "Externals/spirv_cross/SPIRV-Cross")
  set(_tool_rev "")
  if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/${_tool}/.git")
    execute_process(WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/${_tool}" COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        OUTPUT_VARIABLE _tool_rev
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
  endif()
  if(NOT _tool_rev MATCHES "^[0-9a-f]+$")
    set(_shader_compiler_tools_known FALSE)
  endif()
  string(APPEND _shader_compiler_input "${_tool} ${_tool_rev}
")
endforeach()
if(_shader_compiler_tools_known AND _shader_compiler_files)
  string(SHA256 DOLPHIN_SHADER_COMPILER_HASH "${_shader_compiler_input}")
else()
  set(DOLPHIN_SHADER_COMPILER_HASH "${DOLPHIN_WC_REVISION}")
endif()

function(configure_source_file path)
  configure_file(
    "${PROJECT_SOURCE_DIR}/${path}.in"
    "${PROJECT_BINARY_DIR}/${path}.tmp"
  )

  execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_BINARY_DIR}/${path}.tmp" "${PROJECT_BINARY_DIR}/${path}")

  file(REMOVE "${PROJECT_BINARY_DIR}/${path}.tmp")
endfunction()

configure_source_file("Source/Core/Common/scmrev.h")

# Script mode: APPLE describes the host, DOLPHIN_TARGET_APPLE the target (cross builds).
if(APPLE OR DOLPHIN_TARGET_APPLE)
  configure_source_file("Source/Core/VersionInfo.plist")
endif()

if(LINUX)
  configure_source_file("Distribution/flatpak/org.projectplusgame.project-plus-dolphin.metainfo.xml")
endif()
