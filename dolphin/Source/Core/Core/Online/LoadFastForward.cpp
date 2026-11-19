// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/LoadFastForward.h"

#include <chrono>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/NetPlayProto.h"
#include "Core/Rollback/GameplayRollback.h"
#include "Core/Rollback/GameplaySession.h"
#include "Core/Rollback/MachineSession.h"
#include "Core/System.h"

namespace Online::LoadFastForward
{
namespace
{
using Clock = std::chrono::steady_clock;

// The longest the boot runs fast, in emulated seconds. At full speed the boot to the first menu
// is ~6 s (P+'s launcher, the strap, the save check). Past the cap the boot goes on at normal
// speed: a save prompt waiting for a press, a launcher that never starts the game
// (CoreTimingManager::Throttle ends it then, with no loop top running).
constexpr u32 BOOT_SECONDS = 20;

// The log's name for the frames with no scene.
constexpr std::string_view NO_SCENE = "(no scene)";

struct Boot
{
  bool on = false;  // the boot runs fast (it may have reached its cap)
  s64 until = 0;    // its cap, in emulated ticks
  u32 frames = 0;   // loop tops in it
  Clock::time_point since;
  s64 since_ticks = 0;
};

Boot s_boot;  // CPU thread

// Not in a whole-machine session or Dolphin netplay, where every frame's timing is shared with the
// other players, nor for a replay or a spectated game: a spectator plays a game as far behind the
// players as its load and the stream put it (docs/spectate-protocol.md), and a faster load would
// change that.
bool Allowed()
{
  return Config::Get(Config::ONLINE_LOAD_FAST_FORWARD) && !Machine::IsArmed() &&
         !NetPlay::IsNetPlayRunning() && !Gprb::Session::IsRunning() &&
         !Gprb::Session::IsPlayback();
}

// The current scene's name, or "" when there is none (the boot before the scene manager, or the
// frames between two scenes). Anything that is not a scene's name counts as none.
std::string SceneName(const Gprb::Guest& g)
{
  std::string name = Gprb::CurrentSceneName(g);
  if (name.size() < 3 || (!name.starts_with("sc") && !name.starts_with("mu")))
    return {};
  return name;
}

// The boot's scenes, which take no input: P+'s strap and save check, the memory swaps between
// them and the frames with no scene. The first other scene ends the boot: the CSS (P+'s Boot
// Directly to CSS), P+'s main menu with our plugin, the title with START held, Training with L or
// R held (docs/brawl-memory-map.md, Boot order under P+).
bool IsBootScene(std::string_view scene)
{
  return scene.empty() || scene == "scMemoryChange" || scene == "scStrap" || scene == "scBoot";
}

void End(Core::System& system, std::string_view to)
{
  if (!s_boot.on)
    return;
  auto& ct = system.GetCoreTiming();
  const s64 now = static_cast<s64>(ct.GetTicks());
  const bool capped = !ct.IsLoadFastForward() || now >= s_boot.until;
  ct.SetLoadFastForward(false, s_boot.until);
  s_boot.on = false;
  const double host_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - s_boot.since).count();
  const double emulated_ms = 1000.0 * static_cast<double>(now - s_boot.since_ticks) /
                             system.GetSystemTimers().GetTicksPerSecond();
  INFO_LOG_FMT(BRAWLBACK,
               "load fast-forward: boot -> {}: {} frames, {:.0f} ms of game in {:.0f} ms{}",
               to.empty() ? NO_SCENE : to, s_boot.frames, emulated_ms, host_ms,
               capped ? " (capped: the rest ran at normal speed)" : "");
}
}  // namespace

void OnBoot(Core::System& system)
{
  // CoreTimingManager::Init has turned any earlier boot's off.
  s_boot = {};
  // A Brawl disc, or a Brawl build's launcher DOL ("ID-Project+ Netplay Launcher"); not any DOL.
  // Dolphin hooks Brawl's loop top for any launcher DOL (Machine::IsBrawlGameId).
  const std::string game_id = SConfig::GetInstance().GetGameID();
  const bool brawl = game_id.starts_with("RSB") ||
                     (game_id.starts_with("ID-") && game_id.find("Launcher") != std::string::npos);
  if (!brawl || !Allowed())
    return;
  auto& ct = system.GetCoreTiming();
  const s64 now = static_cast<s64>(ct.GetTicks());
  s_boot = {.on = true,
            .until = now + s64{BOOT_SECONDS} * system.GetSystemTimers().GetTicksPerSecond(),
            .since = Clock::now(),
            .since_ticks = now};
  ct.SetLoadFastForward(true, s_boot.until);
}

void OnLoopTop(const Core::CPUThreadGuard& guard)
{
  if (!s_boot.on)
    return;
  auto& system = guard.GetSystem();
  if (!Allowed())
  {
    End(system, "a session or setting");
    return;
  }
  const std::string scene = SceneName(Gprb::Guest(system.GetMemory()));
  if (!IsBootScene(scene))
  {
    End(system, scene);
    return;
  }
  ++s_boot.frames;
}
}  // namespace Online::LoadFastForward
