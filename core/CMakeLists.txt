# Brawl Online core (docs/design/brawl-online-core.md): the online code Dolphin and EmissaryRecomp
# share. Built as part of Dolphin's tree (dolphin/CMakeLists.txt adds this folder), which gives it
# fmt and picojson; EmissaryRecomp fetches this folder alone and provides them itself.

add_library(brawl_online_core STATIC
  include/BrawlOnline/CommonTypes.h
  include/BrawlOnline/Online/ChatText.h
  include/BrawlOnline/Online/GameAlive.h
  include/BrawlOnline/Online/MatchRules.h
  include/BrawlOnline/Online/RoomMessages.h
  include/BrawlOnline/Online/SetFormat.h
  include/BrawlOnline/Online/Timeouts.h
  include/BrawlOnline/Rollback/PeerData.h
  src/Online/ChatText.cpp
  src/Online/GameAlive.cpp
  src/Online/MatchRules.cpp
  src/Online/RoomMessages.cpp
  src/Online/SetFormat.cpp
  src/Rollback/PeerData.cpp
)
target_include_directories(brawl_online_core PUBLIC include)
target_compile_features(brawl_online_core PUBLIC cxx_std_20)
target_link_libraries(brawl_online_core PUBLIC fmt::fmt)

# Inside Dolphin's tree: the same precompiled header as Dolphin's own code, so moving code here
# changes nothing in what the compiler makes of it.
if(TARGET use_pch)
  target_link_libraries(brawl_online_core PRIVATE use_pch)
endif()
