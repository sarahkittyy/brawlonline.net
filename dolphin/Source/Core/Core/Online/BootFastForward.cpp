// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/BootFastForward.h"

#include <chrono>
#include <string>

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
#include "Core/Rollback/MachineSession.h"
#include "Core/System.h"

namespace Online::BootFastForward
{
namespace
{
using Clock = std::chrono::steady_clock;

// The longest the boot runs fast, in emulated seconds: ~6 s at full speed to the first menu. Past
// it the boot goes on at normal speed (a save prompt waiting for a press, a launcher that never
// starts the game: CoreTimingManager::Throttle ends it then, with no loop top running).
constexpr u32 CAP_SECONDS = 20;

bool s_on = false;  // CPU thread
Clock::time_point s_since;

bool Allowed()
{
  return Config::Get(Config::ONLINE_BOOT_FAST_FORWARD) && !Machine::IsArmed() &&
         !NetPlay::IsNetPlayRunning();
}

// The boot's own scenes: none yet (the launcher DOL, the frames between two scenes), P+'s strap,
// the save check, the memory swap between them. Anything that is not a scene's name counts as none.
bool IsBootScene(const std::string& scene)
{
  if (scene.size() < 3 || (!scene.starts_with("sc") && !scene.starts_with("mu")))
    return true;
  return scene == "scMemoryChange" || scene == "scStrap" || scene == "scBoot";
}

void End(Core::System& system, const std::string& to)
{
  auto& ct = system.GetCoreTiming();
  const bool capped = !ct.IsBootFastForward();
  ct.SetBootFastForward(false, 0);
  s_on = false;
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - s_since).count();
  const double game_ms =
      1000.0 * static_cast<double>(ct.GetTicks()) / system.GetSystemTimers().GetTicksPerSecond();
  INFO_LOG_FMT(BRAWLBACK, "boot fast-forward: to {}: {:.0f} ms of game in {:.0f} ms{}",
               to.empty() ? "(no scene)" : to, game_ms, ms,
               capped ? " (capped: the rest ran at normal speed)" : "");
}
}  // namespace

void OnBoot(Core::System& system)
{
  s_on = false;
  // A Brawl disc, or a Brawl build's launcher DOL ("ID-Project+ Netplay Launcher"); not any DOL.
  const std::string game_id = SConfig::GetInstance().GetGameID();
  const bool brawl = game_id.starts_with("RSB") ||
                     (game_id.starts_with("ID-") && game_id.find("Launcher") != std::string::npos);
  if (!brawl || !Allowed())
    return;
  auto& ct = system.GetCoreTiming();
  s_on = true;
  s_since = Clock::now();
  ct.SetBootFastForward(true, static_cast<s64>(ct.GetTicks()) +
                                  s64{CAP_SECONDS} * system.GetSystemTimers().GetTicksPerSecond());
}

void OnLoopTop(const Core::CPUThreadGuard& guard)
{
  if (!s_on)
    return;
  auto& system = guard.GetSystem();
  if (!Allowed())
  {
    End(system, "a session or setting");
    return;
  }
  const std::string scene = Gprb::CurrentSceneName(Gprb::Guest(system.GetMemory()));
  if (!IsBootScene(scene))
    End(system, scene);
}
}  // namespace Online::BootFastForward
