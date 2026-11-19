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

// The longest a stretch runs fast, in emulated seconds. At full speed the boot to the first menu
// is ~6 s (P+'s launcher, the strap, the save check), a scene change ~1-1.5 s and a match scene's
// load before its first frame under 1 s. Past the cap the stretch goes on at normal speed: a
// strap or a save prompt waiting for a press, a load that never ends, a launcher that never
// starts the game (CoreTimingManager::Throttle ends it then, with no loop top running).
constexpr u32 BOOT_SECONDS = 20;
constexpr u32 STRETCH_SECONDS = 10;

// The log's name for the frames with no scene.
constexpr std::string_view NO_SCENE = "(no scene)";

struct Stretch
{
  bool on = false;  // a stretch is running (it may have reached its cap)
  s64 until = 0;    // its cap, in emulated ticks
  u32 frames = 0;   // loop tops in it
  Clock::time_point since;
  s64 since_ticks = 0;
  std::string from;  // the scene it started in ("boot" from the boot)
};

Stretch s_stretch;  // CPU thread
// OnBoot recognized a Brawl build. Dolphin hooks Brawl's loop top for any launcher DOL
// (Machine::IsBrawlGameId), so the hooks act only when this is set.
bool s_brawl = false;

bool Allowed()
{
  return Config::Get(Config::ONLINE_LOAD_FAST_FORWARD) && !Machine::IsArmed() &&
         !NetPlay::IsNetPlayRunning();
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

// The memory swap between two scenes, or the frames with no scene.
bool IsBetweenScenes(std::string_view scene)
{
  return scene.empty() || scene == "scMemoryChange";
}

// The game shows nothing that takes input and only loads: the boot's scenes (P+'s strap and save
// check), the memory swap between two scenes, and a match scene before its first frame. In a match
// scene the match frame counter (g_GameFrame+4) stays 0 while the scene loads and is 1 on the
// match's first frame, the persistent counter (+0x14, counting since the scene began) then ahead
// of it (Gprb::Session's IsSimStart). The online waiting room is a one-player match: its load too.
bool IsLoading(const Gprb::Guest& g, std::string_view scene)
{
  if (IsBetweenScenes(scene) || scene == "scStrap" || scene == "scBoot")
    return true;
  if (scene != "scMelee")
    return false;
  const u32 frame = g.U32(Gprb::Addr::GAME_FRAME + 4).value_or(0);
  const u32 persistent = g.U32(Gprb::Addr::GAME_FRAME + 0x14).value_or(0);
  return !(frame >= 1 && persistent > frame);
}

void Start(Core::System& system, std::string_view from, u32 seconds)
{
  auto& ct = system.GetCoreTiming();
  const s64 now = static_cast<s64>(ct.GetTicks());
  s_stretch = {.on = true,
               .until = now + s64{seconds} * system.GetSystemTimers().GetTicksPerSecond(),
               .since = Clock::now(),
               .since_ticks = now,
               .from = std::string(from.empty() ? NO_SCENE : from)};
  ct.SetLoadFastForward(true, s_stretch.until);
}

void End(Core::System& system, std::string_view to)
{
  if (!s_stretch.on)
    return;
  auto& ct = system.GetCoreTiming();
  const s64 now = static_cast<s64>(ct.GetTicks());
  const bool capped = !ct.IsLoadFastForward() || now >= s_stretch.until;
  ct.SetLoadFastForward(false, s_stretch.until);
  s_stretch.on = false;
  const double host_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - s_stretch.since).count();
  const double emulated_ms = 1000.0 * static_cast<double>(now - s_stretch.since_ticks) /
                             system.GetSystemTimers().GetTicksPerSecond();
  INFO_LOG_FMT(BRAWLBACK,
               "load fast-forward: {} -> {}: {} frames, {:.0f} ms of game in {:.0f} ms{}",
               s_stretch.from, to.empty() ? NO_SCENE : to, s_stretch.frames, emulated_ms, host_ms,
               capped ? " (capped: the rest ran at normal speed)" : "");
}

// Whether a hook may act: a Brawl build, the setting on, and no whole-machine session, netplay or
// running gameplay session (its first frame ends any load). Nor a replay or spectated game: a
// spectator plays a game as far behind the players as its load and the stream put it
// (docs/spectate-protocol.md), and a faster load would change that. Ends the stretch when not
// allowed.
bool MayAct(Core::System& system)
{
  if (!s_brawl)
    return false;
  if (Gprb::Session::IsRunning() || Gprb::Session::IsPlayback() || !Allowed())
  {
    End(system, "a session or setting");
    return false;
  }
  return true;
}
}  // namespace

void OnBoot(Core::System& system)
{
  // CoreTimingManager::Init has turned any earlier stretch off.
  s_stretch = {};
  // A Brawl disc, or a Brawl build's launcher DOL ("ID-Project+ Netplay Launcher"); not any DOL.
  const std::string game_id = SConfig::GetInstance().GetGameID();
  s_brawl = game_id.starts_with("RSB") ||
            (game_id.starts_with("ID-") && game_id.find("Launcher") != std::string::npos);
  if (!s_brawl || !Allowed())
    return;
  Start(system, "boot", BOOT_SECONDS);
}

void OnField(Core::System& system)
{
  // Between two scenes the game's main loop can stay in one pass for half a second (the old
  // scene's teardown and the new one's first loads, waiting on emulated disc and card reads):
  // no loop top comes to start the stretch, but the fields do. Only starts one; the loop top
  // ends it. Not in a match scene: its first frame (the barrier) is the loop top's to see.
  if (!MayAct(system) || s_stretch.on)
    return;
  const std::string scene = SceneName(Gprb::Guest(system.GetMemory()));
  if (IsBetweenScenes(scene))
    Start(system, scene, STRETCH_SECONDS);
}

void OnLoopTop(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  if (!MayAct(system))
    return;
  const Gprb::Guest g(system.GetMemory());
  const std::string scene = SceneName(g);
  if (!IsLoading(g, scene))
  {
    End(system, scene);
    return;
  }
  if (!s_stretch.on)
    Start(system, scene, STRETCH_SECONDS);
  ++s_stretch.frames;
}
}  // namespace Online::LoadFastForward
