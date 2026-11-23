# Brawl Online's in-game UI (docs/design/brawl-online-core.md §11): the on-screen messages, the chat
# window, the replay overlay, drawn with ImGui by Dolphin (VideoCommon/OnScreenUI) and by
# EmissaryRecomp (in its own ImGui frame). It needs an `imgui` target: Dolphin's Externals/imgui, or
# the recomp's (aurora's, the same version: 1.92.2b).

add_library(brawl_online_ui STATIC
  include/BrawlOnline/UI/Hooks.h
  include/BrawlOnline/UI/OnScreenDisplay.h
  include/BrawlOnline/UI/OnlineChatUI.h
  include/BrawlOnline/UI/ReplayOverlayUI.h
  include/BrawlOnline/UI/Style.h
  src/Hooks.cpp
  src/OnScreenDisplay.cpp
  src/OnlineChatUI.cpp
  src/ReplayOverlayUI.cpp
  src/Style.cpp
)
target_include_directories(brawl_online_ui PUBLIC include)
target_link_libraries(brawl_online_ui PUBLIC imgui fmt::fmt)

if(TARGET brawl_online_core)
  # Inside Dolphin's tree: compiled as the core is (Dolphin's definitions, include directories and
  # precompiled header).
  target_link_libraries(brawl_online_ui PUBLIC brawl_online_core)
  if(TARGET use_pch)
    target_link_libraries(brawl_online_ui PRIVATE use_pch)
  endif()
  if(TARGET common AND TARGET core)
    target_compile_definitions(brawl_online_ui PRIVATE $<TARGET_PROPERTY:core,COMPILE_DEFINITIONS>)
    target_include_directories(brawl_online_ui PRIVATE $<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>)
  endif()
elseif(TARGET brawl_online_core_standalone)
  target_link_libraries(brawl_online_ui PUBLIC brawl_online_core_standalone)
endif()
