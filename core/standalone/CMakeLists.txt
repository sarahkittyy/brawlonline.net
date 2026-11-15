# Brawl Online core outside Dolphin's tree (docs/design/brawl-online-core.md §10): the build
# EmissaryRecomp uses. It compiles the core with Dolphin's own Common (a subset, as is) and the
# Externals they use, from a sparse checkout of this repository:
#
#   core/                                        the core and its compat headers
#   dolphin/Source/Core/Common/                  Dolphin's Common
#   dolphin/Source/Core/Core/Brawlback/include/  GekkoNet and Brawlback's constants
#   dolphin/Externals/{fmt,enet,SFML,miniupnpc,curl,mbedtls,zlib-ng,monocypher,xxhash,picojson}
#
# Standalone:  cmake -S core/standalone -B build -G Ninja
# From a parent project:  add_subdirectory(<checkout>/core/standalone brawl-online-core), then link
# brawl_online_core_standalone and give it an engine (BrawlOnline::SetEngine) and a host
# (core/compat/include/BrawlOnline/Host.h).

cmake_minimum_required(VERSION 3.20)
project(brawl_online_core_standalone C CXX)

set(BRAWL_ONLINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "The repository checkout (core/ and dolphin/ under it)")
set(BRAWL_ONLINE_VERSION "emissary" CACHE STRING "What the core reports as its build (Common::GetScmDescStr)")
get_filename_component(BO_ROOT "${BRAWL_ONLINE_ROOT}" ABSOLUTE)
set(BO_CORE "${BO_ROOT}/core")
set(BO_DOLPHIN "${BO_ROOT}/dolphin")
set(BO_EXT "${BO_DOLPHIN}/Externals")
set(BO_COMMON "${BO_DOLPHIN}/Source/Core/Common")

# The Externals' CMake files call Dolphin's helpers.
if(NOT COMMAND dolphin_disable_warnings)
  function(dolphin_disable_warnings target)
    get_target_property(type ${target} TYPE)
    if(NOT type STREQUAL "INTERFACE_LIBRARY")
      target_compile_options(${target} PRIVATE -w)
    endif()
  endfunction()
endif()

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Externals -------------------------------------------------------------------------------
if(NOT TARGET fmt::fmt)
  add_subdirectory("${BO_EXT}/fmt" "${CMAKE_CURRENT_BINARY_DIR}/ext/fmt" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET enet::enet)
  add_subdirectory("${BO_EXT}/enet" "${CMAKE_CURRENT_BINARY_DIR}/ext/enet" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET xxhash::xxhash)
  add_subdirectory("${BO_EXT}/xxhash" "${CMAKE_CURRENT_BINARY_DIR}/ext/xxhash" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET monocypher)
  add_subdirectory("${BO_EXT}/monocypher" "${CMAKE_CURRENT_BINARY_DIR}/ext/monocypher" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET ZLIB::ZLIB)
  add_subdirectory("${BO_EXT}/zlib-ng" "${CMAKE_CURRENT_BINARY_DIR}/ext/zlib-ng" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET Miniupnpc::miniupnpc)
  add_subdirectory("${BO_EXT}/miniupnpc" "${CMAKE_CURRENT_BINARY_DIR}/ext/miniupnpc" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET MbedTLS::mbedtls AND NOT TARGET mbedtls)
  add_subdirectory("${BO_EXT}/mbedtls" "${CMAKE_CURRENT_BINARY_DIR}/ext/mbedtls" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET sfml-network AND NOT TARGET sfml)
  add_subdirectory("${BO_EXT}/SFML" "${CMAKE_CURRENT_BINARY_DIR}/ext/SFML" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET CURL::libcurl)
  if(NOT COMMAND dolphin_find_optional_system_library)
    macro(dolphin_find_optional_system_library)
    endmacro()
  endif()
  add_subdirectory("${BO_EXT}/curl" "${CMAKE_CURRENT_BINARY_DIR}/ext/curl" EXCLUDE_FROM_ALL)
endif()

# ---- Dolphin's Common (the part the core uses) ------------------------------------------------
set(BO_GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(DOLPHIN_WC_REVISION "${BRAWL_ONLINE_VERSION}")
set(DOLPHIN_WC_DESCRIBE "${BRAWL_ONLINE_VERSION}")
set(DOLPHIN_WC_BRANCH "emissary")
set(DOLPHIN_WC_COMMITS_AHEAD_MASTER 0)
set(DOLPHIN_SHADER_COMPILER_HASH "")
set(DISTRIBUTOR "Brawl Online")
set(DOLPHIN_DEFAULT_UPDATE_TRACK "")
configure_file("${BO_COMMON}/scmrev.h.in" "${BO_GEN}/Common/scmrev.h")

set(BO_COMMON_SOURCES
  CommonFuncs.cpp
  Config/Config.cpp
  Config/ConfigInfo.cpp
  Config/Layer.cpp
  ENet.cpp
  FileUtil.cpp
  Hash.cpp
  HttpRequest.cpp
  IOFile.cpp
  IniFile.cpp
  Logging/LogManager.cpp
  MsgHandler.cpp
  NatPmp.cpp
  Random.cpp
  SocketContext.cpp
  Network.cpp
  StringUtil.cpp
  Thread.cpp
  TimeUtil.cpp
  Timer.cpp
  UPnP.cpp
  Version.cpp
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  list(APPEND BO_COMMON_SOURCES x64CPUDetect.cpp)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  list(APPEND BO_COMMON_SOURCES ArmCPUDetect.cpp)
else()
  list(APPEND BO_COMMON_SOURCES GenericCPUDetect.cpp)
endif()
if(WIN32)
  list(APPEND BO_COMMON_SOURCES Logging/ConsoleListenerWin.cpp)
else()
  list(APPEND BO_COMMON_SOURCES Logging/ConsoleListenerNix.cpp)
endif()
list(TRANSFORM BO_COMMON_SOURCES PREPEND "${BO_COMMON}/")

# The include root holds only what the recomp fetches: Common's headers and Brawlback's (GekkoNet,
# constants). Dolphin's other Core/ headers must not be found here: the compat stand-ins answer for
# them (a header missing from compat is then a compile error, not Dolphin's real one).
set(BO_INC "${BO_GEN}/include")
file(GLOB_RECURSE BO_COMMON_HEADERS CONFIGURE_DEPENDS RELATIVE "${BO_COMMON}" "${BO_COMMON}/*.h" "${BO_COMMON}/*.inl")
foreach(h IN LISTS BO_COMMON_HEADERS)
  configure_file("${BO_COMMON}/${h}" "${BO_INC}/Common/${h}" COPYONLY)
endforeach()
set(BO_BRAWLBACK "${BO_DOLPHIN}/Source/Core/Core/Brawlback/include")
file(GLOB_RECURSE BO_BRAWLBACK_HEADERS CONFIGURE_DEPENDS RELATIVE "${BO_BRAWLBACK}" "${BO_BRAWLBACK}/*.h" "${BO_BRAWLBACK}/*.hpp")
foreach(h IN LISTS BO_BRAWLBACK_HEADERS)
  configure_file("${BO_BRAWLBACK}/${h}" "${BO_INC}/Core/Brawlback/include/${h}" COPYONLY)
endforeach()

# Dolphin files the core uses as they are (they need nothing else of Dolphin's).
set(BO_REUSED_HEADERS
  InputCommon/GCPadStatus.h
  Core/Rollback/FrameProfile.h
  Core/Rollback/UdpSocket6.h
  Core/Rollback/ReplayPlayback.h
)
foreach(h IN LISTS BO_REUSED_HEADERS)
  configure_file("${BO_DOLPHIN}/Source/Core/${h}" "${BO_INC}/${h}" COPYONLY)
endforeach()
set(BO_REUSED_SOURCES
  "${BO_DOLPHIN}/Source/Core/Core/Rollback/FrameProfile.cpp"
  "${BO_DOLPHIN}/Source/Core/Core/Rollback/UdpSocket6.cpp"
  "${BO_DOLPHIN}/Source/Core/Core/Rollback/ReplayPlayback.cpp"
)

add_library(brawl_online_common STATIC ${BO_COMMON_SOURCES})
target_compile_features(brawl_online_common PUBLIC cxx_std_23)
target_include_directories(brawl_online_common PUBLIC
  "${BO_CORE}/compat/include"      # first: the compat stand-ins for Dolphin's Core/ headers
  "${BO_INC}"                      # Common/ and Core/Brawlback/include only
  "${BO_GEN}"
  "${BO_EXT}/picojson"
)
target_compile_definitions(brawl_online_common PUBLIC
  _ARCH_64=1 BRAWL_ONLINE_STANDALONE=1 USE_UPNP=1
  DATA_DIR="./"
)
if(WIN32)
  target_compile_definitions(brawl_online_common PUBLIC
    NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00
    _CRT_SECURE_NO_WARNINGS _POSIX_THREAD_SAFE_FUNCTIONS)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  target_compile_definitions(brawl_online_common PUBLIC _M_X86_64=1)
endif()
target_link_libraries(brawl_online_common PUBLIC fmt::fmt enet::enet
  PRIVATE CURL::libcurl Miniupnpc::miniupnpc ZLIB::ZLIB mbedtls sfml-network sfml-system)
if(WIN32)
  target_link_libraries(brawl_online_common PUBLIC ws2_32 iphlpapi winmm shlwapi bcrypt)
else()
  target_link_libraries(brawl_online_common PUBLIC pthread ${CMAKE_DL_LIBS})
endif()

# ---- GekkoNet (Brawl Online's, as Dolphin builds it) ------------------------------------------
# A parent that already builds the same GekkoNet (EmissaryRecomp vendors it) passes its target.
set(BRAWL_ONLINE_GEKKONET_TARGET "" CACHE STRING "An existing GekkoNet target to use")
if(BRAWL_ONLINE_GEKKONET_TARGET)
  set(BO_GEKKO_TARGET "${BRAWL_ONLINE_GEKKONET_TARGET}")
else()
  set(BO_GEKKO "${BO_BRAWLBACK}/gekkonet/GekkoLib")
  file(GLOB BO_GEKKO_SOURCES CONFIGURE_DEPENDS "${BO_GEKKO}/src/*.cpp")
  add_library(brawl_online_gekkonet STATIC ${BO_GEKKO_SOURCES})
  target_include_directories(brawl_online_gekkonet
    PUBLIC "${BO_GEKKO}/include"
    PRIVATE "${BO_GEKKO}/include/private" "${BO_GEKKO}/thirdparty")
  target_compile_definitions(brawl_online_gekkonet PUBLIC GEKKONET_STATIC GEKKONET_NO_ASIO)
  target_link_libraries(brawl_online_gekkonet PRIVATE brawl_online_common)
  target_compile_options(brawl_online_gekkonet PRIVATE -w)
  set(BO_GEKKO_TARGET brawl_online_gekkonet)
endif()

# ---- The core ----------------------------------------------------------------------------------
file(GLOB_RECURSE BO_CORE_SOURCES CONFIGURE_DEPENDS "${BO_CORE}/src/*.cpp" "${BO_CORE}/compat/src/*.cpp")
add_library(brawl_online_core_standalone STATIC ${BO_CORE_SOURCES} ${BO_REUSED_SOURCES})
target_compile_features(brawl_online_core_standalone PUBLIC cxx_std_23)
target_include_directories(brawl_online_core_standalone PUBLIC "${BO_CORE}/include")
target_link_libraries(brawl_online_core_standalone PUBLIC brawl_online_common ${BO_GEKKO_TARGET}
  PRIVATE monocypher xxhash::xxhash ZLIB::ZLIB mbedtls sfml-network sfml-system)

# ---- A smoke test: the core links and runs its start-up with a do-nothing engine --------------
if(PROJECT_IS_TOP_LEVEL)
  add_executable(brawl_online_core_smoke "${CMAKE_CURRENT_LIST_DIR}/smoke.cpp")
  # Every object of the core, so a symbol the core needs and nothing provides fails here.
  add_dependencies(brawl_online_core_smoke brawl_online_core_standalone)
  target_link_libraries(brawl_online_core_smoke PRIVATE
    -Wl,--whole-archive $<TARGET_FILE:brawl_online_core_standalone> -Wl,--no-whole-archive
    brawl_online_core_standalone)
endif()
