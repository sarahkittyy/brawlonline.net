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
    PowerPC/Jit64Common/PsMaddEft.cpp
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

# The SD card of a whole-machine session (Core/Rollback/MachineSDCard): the throwaway layer and its
# rollback, a joiner on the session's card, and hostile manifests, requests and chunks.
add_dolphin_test(MachineSDCardTest MachineSDCardTest.cpp)
# The GameCube memory card of a whole-machine session: the same state on every machine (bug report
# #128), written back by the host only, to its own files.
add_dolphin_test(GCMemcardDirectoryTest GCMemcardDirectoryTest.cpp)
# A network session's reads of the local controllers (Gprb::ReadHostPad, bug report #70).
add_dolphin_test(HostPadReadTest Rollback/HostPadReadTest.cpp)
# A GameCube's ARAM in whole-machine rollback snapshots, outside the savestate (bug report #135).
add_dolphin_test(ARAMSnapshotTest Rollback/ARAMSnapshotTest.cpp)
# The rollback job pool (Core/Rollback/job.h) stopped and started again (GitHub issue #3).
add_dolphin_test(JobPoolTest Rollback/JobPoolTest.cpp)
# Replay files (Core/Rollback/ReplayFile): round trip, cut and damaged files, hostile sizes.
add_dolphin_test(ReplayFileTest Rollback/ReplayFileTest.cpp)
# Spectating: the uploader's pacing, backoff and roles (Core/Online/Spectate).
add_dolphin_test(SpectateTest Online/SpectateTest.cpp)
# 3-4 player matches: teams and who picks the next stage (Core/Online/GameSetup).
add_dolphin_test(NPlayerSetupTest Online/NPlayerSetupTest.cpp)
# The online connection's game-alive while the game runs (Core/Online/GameAlive).
add_dolphin_test(GameAliveTest Online/GameAliveTest.cpp)
# Rooms: the mm server's room messages, validated before they reach the game (Core/Online/RoomMessages).
add_dolphin_test(RoomMessagesTest Online/RoomMessagesTest.cpp)
# Watching the room's game: the watch channel's packets into a live replay (Core/Online/RoomWatch).
add_dolphin_test(RoomWatchTest Online/RoomWatchTest.cpp)
# Match rules: get-ticket-resp's `rules`, checked before they reach the game (Core/Online/MatchRules).
add_dolphin_test(MatchRulesTest Online/MatchRulesTest.cpp)
add_dolphin_test(SpecialAssetsTest Online/SpecialAssetsTest.cpp)
# Sets: the stage steps of a set's format (Core/Online/SetFormat).
add_dolphin_test(SetFormatTest Online/SetFormatTest.cpp)
# Chat: the text cleaner, the crypto, and the chat state against hostile messages (Core/Online/Chat*).
add_dolphin_test(ChatTest
  Online/ChatTextTest.cpp
  Online/ChatCryptoTest.cpp
  Online/ChatTest.cpp
)
if(MSVC)
  # zpp_bits (GekkoNet's serializer) trips /W4 warnings; GekkoNet's own sources do the same.
  set_source_files_properties(Rollback/GekkoNetFuzzTest.cpp
    PROPERTIES COMPILE_FLAGS "/WX- /wd4018 /wd4189 /wd4244 /wd4267 /wd4389")
endif()
