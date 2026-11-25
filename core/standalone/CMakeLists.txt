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
# The Brawl Online release this core is (0.2.N; empty: an unstamped "0.1.0"): its `appVersion`,
# which mm holds to the update feeds as it holds that release's Dolphin (docs/design/
# brawl-online-core.md §10, "Which engine"). EmissaryRecomp's release build sets it to the release
# whose commit it pins.
set(BRAWL_ONLINE_APP_VERSION "" CACHE STRING "The Brawl Online release of this core (appVersion)")
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
  if(TARGET xxhash)  # a parent's (EmissaryRecomp's dependencies bring one)
    add_library(xxhash::xxhash ALIAS xxhash)
  else()
    add_subdirectory("${BO_EXT}/xxhash" "${CMAKE_CURRENT_BINARY_DIR}/ext/xxhash" EXCLUDE_FROM_ALL)
  endif()
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
  if(APPLE)
    # The macOS 27 SDK declares pipe2, but only from macOS 27: curl would call it on the older macOS
    # the build targets (CMAKE_OSX_DEPLOYMENT_TARGET), which fails the compile.
    set(HAVE_PIPE2 OFF CACHE INTERNAL "")
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
if(APPLE)
  # FileUtil.cpp finds the app bundle through DynamicLibrary on macOS.
  list(APPEND BO_COMMON_SOURCES DynamicLibrary.cpp)
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
  Core/Rollback/GameplayOnlineBackend.h
  Core/Rollback/InputTrace.h
  Core/Online/MatchQoS.h
  Core/Online/SpecialAssets.h
  Core/Rollback/CardFat.h
)
foreach(h IN LISTS BO_REUSED_HEADERS)
  configure_file("${BO_DOLPHIN}/Source/Core/${h}" "${BO_INC}/${h}" COPYONLY)
endforeach()
set(BO_REUSED_SOURCES
  "${BO_DOLPHIN}/Source/Core/Core/Rollback/FrameProfile.cpp"
  "${BO_DOLPHIN}/Source/Core/Core/Rollback/UdpSocket6.cpp"
  "${BO_DOLPHIN}/Source/Core/Core/Rollback/ReplayPlayback.cpp"
  "${BO_DOLPHIN}/Source/Core/Core/Rollback/GameplayOnlineBackend.cpp"
  "${BO_DOLPHIN}/Source/Core/Core/Rollback/InputTrace.cpp"
  "${BO_DOLPHIN}/Source/Core/Core/Online/MatchQoS.cpp"
  "${BO_DOLPHIN}/Source/Core/Core/Online/SpecialAssets.cpp"
  "${BO_DOLPHIN}/Source/Core/Core/Rollback/CardFat.cpp"
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
  # qwave: Online/MatchQoS.cpp marks a match's traffic as voice (qWAVE).
  target_link_libraries(brawl_online_common PUBLIC ws2_32 iphlpapi winmm shlwapi bcrypt qwave)
else()
  target_link_libraries(brawl_online_common PUBLIC pthread ${CMAKE_DL_LIBS})
endif()
if(APPLE)
  # StringUtil.cpp converts text with iconv: its own library on macOS (glibc has it built in).
  target_link_libraries(brawl_online_common PUBLIC iconv)
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
# The core's sources include GekkoNet's header from Core/Brawlback, built as a static library.
target_compile_definitions(brawl_online_core_standalone PUBLIC GEKKONET_STATIC GEKKONET_NO_ASIO)
if(BRAWL_ONLINE_APP_VERSION)
  if(NOT BRAWL_ONLINE_APP_VERSION MATCHES "^[0-9]+\.[0-9]+\.[0-9]+$")
    message(FATAL_ERROR "BRAWL_ONLINE_APP_VERSION must be major.minor.patch, not ${BRAWL_ONLINE_APP_VERSION}")
  endif()
  target_compile_definitions(brawl_online_core_standalone PUBLIC
    BRAWL_ONLINE_APP_VERSION="${BRAWL_ONLINE_APP_VERSION}")
endif()
target_link_libraries(brawl_online_core_standalone PUBLIC brawl_online_common ${BO_GEKKO_TARGET}
  PRIVATE monocypher xxhash::xxhash ZLIB::ZLIB mbedtls sfml-network sfml-system)
# The UI's headers (core/ui): the on-screen messages' (VideoCommon/OnScreenDisplay.h's stand-in).
target_include_directories(brawl_online_core_standalone PUBLIC "${BO_CORE}/ui/include")

# ---- The in-game UI (core/ui), for a parent that draws it -------------------------------------
# It needs the parent's `imgui` target (ImGui 1.92.2b, Dolphin's version). Without it, on-screen
# messages go to the host (BrawlOnline::Host::OnScreenMessage).
option(BRAWL_ONLINE_UI "Build the in-game UI (core/ui) with the parent's imgui target" OFF)
if(BRAWL_ONLINE_UI)
  add_subdirectory("${BO_CORE}/ui" "${CMAKE_CURRENT_BINARY_DIR}/ui")
else()
  target_compile_definitions(brawl_online_core_standalone PRIVATE BRAWL_ONLINE_NO_UI=1)
endif()

# ---- Dolphin's controller settings for the engine's pads (BrawlOnline/DolphinPads.h) ----------
# Dolphin's own input code, as it is: GCPadNew.ini through Core/HW/GCPadEmu.cpp, InputCommon's
# expressions and control groups, its Windows devices (DInput, XInput) and its macOS keyboard and
# mouse (Quartz; the parent passes the window's NSView). A parent with SDL 3 passes
# its target in BRAWL_ONLINE_SDL3_TARGET for Dolphin's SDL gamepads over the parent's SDL. Needs the
# sparse checkout's dolphin/Source/Core/InputCommon and Core/HW/GCPadEmu.*.
set(BO_DOLPHIN_INPUT_DEFAULT OFF)
if(WIN32 OR APPLE)
  set(BO_DOLPHIN_INPUT_DEFAULT ON)
endif()
option(BRAWL_ONLINE_DOLPHIN_INPUT "Build Dolphin's controller settings for the engine's pads" ${BO_DOLPHIN_INPUT_DEFAULT})
set(BRAWL_ONLINE_SDL3_TARGET "" CACHE STRING "The parent's SDL 3 target, for SDL gamepads")
if(BRAWL_ONLINE_DOLPHIN_INPUT)
  set(BO_IC "${BO_DOLPHIN}/Source/Core/InputCommon")
  file(GLOB_RECURSE BO_IC_HEADERS CONFIGURE_DEPENDS RELATIVE "${BO_IC}" "${BO_IC}/*.h")
  foreach(h IN LISTS BO_IC_HEADERS)
    configure_file("${BO_IC}/${h}" "${BO_INC}/InputCommon/${h}" COPYONLY)
  endforeach()
  configure_file("${BO_DOLPHIN}/Source/Core/Core/HW/GCPadEmu.h" "${BO_INC}/Core/HW/GCPadEmu.h" COPYONLY)
  set(BO_INPUT_SOURCES
    ControlReference/ControlReference.cpp
    ControlReference/ExpressionParser.cpp
    ControlReference/FunctionExpression.cpp
    ControllerEmu/ControllerEmu.cpp
    ControllerEmu/StickGate.cpp
    ControllerEmu/Control/Control.cpp
    ControllerEmu/Control/Input.cpp
    ControllerEmu/Control/Output.cpp
    ControllerEmu/ControlGroup/AnalogStick.cpp
    ControllerEmu/ControlGroup/Buttons.cpp
    ControllerEmu/ControlGroup/ControlGroup.cpp
    ControllerEmu/ControlGroup/MixedTriggers.cpp
    ControllerEmu/Setting/NumericSetting.cpp
    ControllerInterface/ControllerInterface.cpp
    ControllerInterface/CoreDevice.cpp
    ControllerInterface/InputBackend.cpp
  )
  if(WIN32)
    list(APPEND BO_INPUT_SOURCES
      ControllerInterface/Win32/Win32.cpp
      ControllerInterface/DInput/DInput.cpp
      ControllerInterface/DInput/DInputJoystick.cpp
      ControllerInterface/DInput/DInputKeyboardMouse.cpp
      ControllerInterface/DInput/XInputFilter.cpp
      ControllerInterface/ForceFeedback/ForceFeedbackDevice.cpp
      ControllerInterface/XInput/XInput.cpp
    )
  elseif(APPLE)
    # Dolphin's macOS keyboard and mouse ("Quartz/0/Keyboard & Mouse" in GCPadNew.ini), Objective-C++.
    list(APPEND BO_INPUT_SOURCES
      ControllerInterface/Quartz/Quartz.mm
      ControllerInterface/Quartz/QuartzKeyboardAndMouse.mm
    )
  endif()
  if(BRAWL_ONLINE_SDL3_TARGET)
    list(APPEND BO_INPUT_SOURCES ControllerInterface/SDL/SDLGamepad.cpp)
  endif()
  list(TRANSFORM BO_INPUT_SOURCES PREPEND "${BO_IC}/")
  list(APPEND BO_INPUT_SOURCES
    "${BO_DOLPHIN}/Source/Core/Core/HW/GCPadEmu.cpp"
    "${BO_CORE}/compat/input/DolphinPads.cpp"
    "${BO_CORE}/compat/input/Stubs.cpp"
  )
  if(BRAWL_ONLINE_SDL3_TARGET)
    list(APPEND BO_INPUT_SOURCES "${BO_CORE}/compat/input/SdlDevices.cpp")
  endif()
  add_library(brawl_online_dolphin_input STATIC ${BO_INPUT_SOURCES})
  target_compile_features(brawl_online_dolphin_input PUBLIC cxx_std_23)
  target_link_libraries(brawl_online_dolphin_input PUBLIC brawl_online_common)
  if(WIN32)
    target_link_libraries(brawl_online_dolphin_input PUBLIC dinput8 dxguid cfgmgr32 setupapi)
    # MSVC's xinput.h names the DLL (Windows 8 and later); MinGW's does not.
    set_source_files_properties("${BO_IC}/ControllerInterface/XInput/XInput.cpp" PROPERTIES
      COMPILE_DEFINITIONS "XINPUT_DLL=L\"xinput1_4.dll\"")
  elseif(APPLE)
    # Compiled as C++ told it is Objective-C++: enabling the OBJCXX language here, inside a parent's build, made
    # CMake compile the parent's SDL Objective-C files with C++ options.
    set_source_files_properties("${BO_IC}/ControllerInterface/Quartz/Quartz.mm"
      "${BO_IC}/ControllerInterface/Quartz/QuartzKeyboardAndMouse.mm" PROPERTIES
      LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
    target_link_libraries(brawl_online_dolphin_input PUBLIC
      "-framework Cocoa" "-framework Carbon" "-framework CoreFoundation" "-framework QuartzCore")
  endif()
  if(BRAWL_ONLINE_SDL3_TARGET)
    target_link_libraries(brawl_online_dolphin_input PUBLIC ${BRAWL_ONLINE_SDL3_TARGET})
    target_compile_definitions(brawl_online_dolphin_input PRIVATE BRAWL_ONLINE_DOLPHIN_INPUT_SDL=1)
  endif()
endif()

# ---- A smoke test: the core links and runs its start-up with a do-nothing engine --------------
if(PROJECT_IS_TOP_LEVEL)
  add_executable(brawl_online_core_smoke "${CMAKE_CURRENT_LIST_DIR}/smoke.cpp")
  # Every object of the core, so a symbol the core needs and nothing provides fails here.
  add_dependencies(brawl_online_core_smoke brawl_online_core_standalone)
  target_link_libraries(brawl_online_core_smoke PRIVATE
    -Wl,--whole-archive $<TARGET_FILE:brawl_online_core_standalone> -Wl,--no-whole-archive
    brawl_online_core_standalone)
  if(TARGET brawl_online_dolphin_input)
    # Loads a User folder's GCPadNew.ini and prints each port's pad (dolphin_pads_probe.cpp).
    add_executable(brawl_online_dolphin_pads_probe "${CMAKE_CURRENT_LIST_DIR}/dolphin_pads_probe.cpp")
    target_link_libraries(brawl_online_dolphin_pads_probe PRIVATE brawl_online_dolphin_input)
  endif()
endif()
