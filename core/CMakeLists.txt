# Brawl Online core (docs/design/brawl-online-core.md): the online code Dolphin and EmissaryRecomp
# share. Built as part of Dolphin's tree (dolphin/CMakeLists.txt adds this folder), which gives it
# fmt and picojson; EmissaryRecomp fetches this folder alone and provides them itself.

add_library(brawl_online_core STATIC
  include/BrawlOnline/CommonTypes.h
  include/BrawlOnline/Config/OnlineSettings.h
  include/BrawlOnline/Engine.h
  include/BrawlOnline/Online/Chat.h
  include/BrawlOnline/Online/ChatCrypto.h
  include/BrawlOnline/Online/ChatText.h
  include/BrawlOnline/Online/GameAlive.h
  include/BrawlOnline/Online/GameBridge.h
  include/BrawlOnline/Online/GameSetup.h
  include/BrawlOnline/Online/Ipv6.h
  include/BrawlOnline/Online/MatchRules.h
  include/BrawlOnline/Online/Matchmaking.h
  include/BrawlOnline/Online/OnlineClient.h
  include/BrawlOnline/Online/OnlineSession.h
  include/BrawlOnline/Online/Ranked.h
  include/BrawlOnline/Online/RecentCodes.h
  include/BrawlOnline/Online/RoomMessages.h
  include/BrawlOnline/Online/Rooms.h
  include/BrawlOnline/Online/SetFormat.h
  include/BrawlOnline/Online/Spectate.h
  include/BrawlOnline/Online/Timeouts.h
  include/BrawlOnline/Online/User.h
  include/BrawlOnline/Rollback/GameplayRollback.h
  include/BrawlOnline/Rollback/GameplaySession.h
  include/BrawlOnline/Rollback/LoopWatch.h
  include/BrawlOnline/Rollback/PeerData.h
  include/BrawlOnline/Rollback/ReplayFile.h
  include/BrawlOnline/Rollback/ReplayRecorder.h
  src/Config/OnlineSettings.cpp
  src/Engine.cpp
  src/Online/Chat.cpp
  src/Online/ChatCrypto.cpp
  src/Online/ChatText.cpp
  src/Online/GameAlive.cpp
  src/Online/GameBridge.cpp
  src/Online/GameSetup.cpp
  src/Online/Ipv6.cpp
  src/Online/MatchRules.cpp
  src/Online/Matchmaking.cpp
  src/Online/OnlineClient.cpp
  src/Online/OnlineSession.cpp
  src/Online/Ranked.cpp
  src/Online/RecentCodes.cpp
  src/Online/RoomMessages.cpp
  src/Online/Rooms.cpp
  src/Online/SetFormat.cpp
  src/Online/Spectate.cpp
  src/Online/User.cpp
  src/Rollback/GameplayRollback.cpp
  src/Rollback/GameplaySession.cpp
  src/Rollback/LoopWatch.cpp
  src/Rollback/PeerData.cpp
  src/Rollback/ReplayFile.cpp
  src/Rollback/ReplayRecorder.cpp
)
target_include_directories(brawl_online_core PUBLIC include)
target_compile_features(brawl_online_core PUBLIC cxx_std_20)
target_link_libraries(brawl_online_core PUBLIC fmt::fmt monocypher)

# Inside Dolphin's tree: the same precompiled header as Dolphin's own code, so moving code here
# changes nothing in what the compiler makes of it.
if(TARGET use_pch)
  target_link_libraries(brawl_online_core PRIVATE use_pch)
endif()

# Still calling into Dolphin (step 2 of the design: Dolphin's Common helpers, its online settings, the
# gameplay session) until EmissaryRecomp's own versions of those headers exist; static libraries
# that use each other, which CMake repeats on the link line where the linker needs it.
if(TARGET common AND TARGET core)
  target_link_libraries(brawl_online_core PUBLIC common enet::enet core)
  # Compiled exactly as Dolphin's core: its definitions (GEKKONET_STATIC, ...) and include directories.
  target_compile_definitions(brawl_online_core PRIVATE $<TARGET_PROPERTY:core,COMPILE_DEFINITIONS>)
  target_include_directories(brawl_online_core PRIVATE $<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>)
endif()
