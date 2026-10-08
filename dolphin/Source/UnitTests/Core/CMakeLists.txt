add_dolphin_test(MMIOTest MMIOTest.cpp)
add_dolphin_test(PageFaultTest PageFaultTest.cpp)
add_dolphin_test(CoreTimingTest CoreTimingTest.cpp)
add_dolphin_test(PatchAllowlistTest PatchAllowlistTest.cpp)

add_dolphin_test(DSPAcceleratorTest DSP/DSPAcceleratorTest.cpp)
add_dolphin_test(DSPAssemblyTest
  DSP/DSPAssemblyTest.cpp
  DSP/DSPTestBinary.cpp
  DSP/DSPTestText.cpp
  DSP/HermesBinary.cpp
  DSP/HermesText.cpp
)

add_dolphin_test(ESFormatsTest IOS/ES/FormatsTest.cpp)

add_dolphin_test(FileSystemTest IOS/FS/FileSystemTest.cpp)

add_dolphin_test(SkylandersTest IOS/USB/SkylandersTest.cpp)

if(_M_X86_64)
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
    PowerPC/Jit64Common/ConvertDoubleToSingle.cpp
    PowerPC/Jit64Common/Fres.cpp
    PowerPC/Jit64Common/Frsqrte.cpp
  )
elseif(_M_ARM_64)
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
    PowerPC/JitArm64/ConvertSingleDouble.cpp
    PowerPC/JitArm64/FPRF.cpp
    PowerPC/JitArm64/Fres.cpp
    PowerPC/JitArm64/Frsqrte.cpp
    PowerPC/JitArm64/MovI2R.cpp
  )
else()
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
  )
endif()

target_sources(PowerPCTest PRIVATE
  PowerPC/TestValues.h
  StubJit.h
)

# What the other player's machine sends to the gameplay session (Core/Rollback/PeerData) and
# GekkoNet's packet handling against malformed and hostile packets.
add_dolphin_test(OnlineSecurityTest
  Rollback/PeerDataTest.cpp
  Rollback/GekkoNetFuzzTest.cpp
)
target_include_directories(OnlineSecurityTest PRIVATE
  ${CMAKE_SOURCE_DIR}/Source/Core/Core/Brawlback/include/gekkonet/GekkoLib/include
  ${CMAKE_SOURCE_DIR}/Source/Core/Core/Brawlback/include/gekkonet/GekkoLib/include/private
  ${CMAKE_SOURCE_DIR}/Source/Core/Core/Brawlback/include/gekkonet/GekkoLib/thirdparty
)
target_compile_definitions(OnlineSecurityTest PRIVATE GEKKONET_STATIC GEKKONET_NO_ASIO)
if(MSVC)
  # zpp_bits (GekkoNet's serializer) trips /W4 warnings; GekkoNet's own sources do the same.
  set_source_files_properties(Rollback/GekkoNetFuzzTest.cpp
    PROPERTIES COMPILE_FLAGS "/WX- /wd4018 /wd4189 /wd4244 /wd4267 /wd4389")
endif()
