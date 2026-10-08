// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Harness/Harness.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <mutex>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/Harness/HarnessInternal.h"
#include "Core/Online/NetPlaySession.h"
#include "Core/Online/OnlineClient.h"
#include "Core/Online/OnlineSession.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"

namespace Harness
{
namespace detail
{
std::atomic<bool> g_active{false};
std::atomic<u32> g_pad_override_mask{0};
}  // namespace detail

namespace
{
HostCallbacks s_host;

std::atomic<u64> s_frame{0};
std::atomic<u64> s_input_polls{0};
std::atomic<u64> s_si_polls{0};
// Index of the poll whose pad state is currently presented to the game.
u64 s_current_poll_index = 0;  // guarded by s_pad_mutex
// GamePadRead mode: whether an SI poll has already latched pad data for s_current_poll_index.
// If so, a change made now may not reach the game's next read any more.
bool s_latched_since_read = false;  // guarded by s_pad_mutex

// What input_polls counts. Decided at the first SI poll after each boot.
enum class PollSource : int
{
  Unknown,
  SerialInterface,  // every SI poll (generic; usually 2 per frame)
  GamePadRead,      // Brawl's per-frame pad read (gfPadSystem::updateLow -> updateLowGC)
};
std::atomic<PollSource> s_poll_source{PollSource::Unknown};

std::atomic<u64> s_break_at_frame{0};  // 0 = none

std::mutex s_wait_mutex;
std::condition_variable s_wait_cv;

Common::EventHook s_state_hook;

struct PortState
{
  std::optional<GCPadStatus> set;  // persistent pad_set value
  std::vector<GCPadStatus> script;
  u64 script_start = 0;
  bool script_active = false;
  bool neutral = false;  // released (not cleared) after a script finished without a pad_set
  // What the port presented when a script was replaced by one that starts later: kept until the
  // new script starts. Empty if the port was not overridden then (real controller).
  std::optional<GCPadStatus> gap;

  // Cached result for the current poll.
  bool overriding = false;
  GCPadStatus current{};
};

std::mutex s_pad_mutex;
std::array<PortState, 4> s_ports;

GCPadStatus NeutralPad()
{
  GCPadStatus pad{};
  pad.isConnected = true;
  return pad;
}

// s_pad_mutex must be held.
void RecomputePort(int port, u64 poll_index)
{
  PortState& state = s_ports[port];

  if (state.script_active && poll_index >= state.script_start + state.script.size())
  {
    state.script_active = false;
    state.script.clear();
    state.gap.reset();
    if (!state.set)
      state.neutral = true;
  }

  if (state.script_active && poll_index >= state.script_start)
  {
    state.overriding = true;
    state.current = state.script[poll_index - state.script_start];
  }
  else if (state.set)
  {
    state.overriding = true;
    state.current = *state.set;
  }
  else if (state.script_active && state.gap)
  {
    // A script scheduled for the future keeps presenting what the port presented when it was
    // submitted (the previous script's current entry, or neutral). Falling back to the real
    // controller here made Brawl see an unplugged pad and drop the player from the CSS.
    state.overriding = true;
    state.current = *state.gap;
  }
  else if (state.neutral || state.script_active)
  {
    // A port that was never overridden stays with the real controller until the script starts.
    state.overriding = state.neutral;
    state.current = NeutralPad();
  }
  else
  {
    state.overriding = false;
  }

  const u32 bit = 1u << port;
  if (state.overriding || state.script_active)
    detail::g_pad_override_mask.fetch_or(bit, std::memory_order_relaxed);
  else
    detail::g_pad_override_mask.fetch_and(~bit, std::memory_order_relaxed);
}

void OnStateChanged(Core::State state)
{
  if (state == Core::State::Starting)
  {
    s_frame.store(0);
    s_input_polls.store(0);
    s_si_polls.store(0);
    s_poll_source.store(PollSource::Unknown);
    s_break_at_frame.store(0);
    std::lock_guard lk(s_pad_mutex);
    s_current_poll_index = 0;
    s_latched_since_read = false;
    for (int port = 0; port < 4; ++port)
    {
      PortState& ps = s_ports[port];
      if (ps.script_active)
      {
        ps.script_active = false;
        ps.script.clear();
        if (!ps.set)
          ps.neutral = true;
      }
      RecomputePort(port, 0);
    }
  }
  Internal::NotifyWaiters();
}
}  // namespace

namespace detail
{
void ApplyPadOverrideSlow(int port, GCPadStatus* status)
{
  if (port < 0 || port > 3)
    return;
  std::lock_guard lk(s_pad_mutex);
  const PortState& state = s_ports[port];
  if (state.overriding)
    *status = state.current;
}

// Brawl (and P+, which runs on it) reads its pads once per game frame from gf_pad's updateLow,
// which this fork already hooks for Brawlback. The SI itself polls twice per frame there, so
// counting SI polls would make one pad_script entry last half a game frame. So input_polls counts
// SI polls only until the game's own per-frame pad read is first seen (early in boot, while the
// game is still on its strap screen), and from then on counts the game's reads.
// PPR_HARNESS_POLL_SOURCE=si forces the generic SI-poll counting for the whole session.
static bool ForceSIPollSource()
{
  static const bool forced = [] {
    const char* env = std::getenv("PPR_HARNESS_POLL_SOURCE");
    return env && std::string(env) == "si";
  }();
  return forced;
}

static PollSource DeterminePollSource()
{
  PollSource source = s_poll_source.load(std::memory_order_relaxed);
  if (source == PollSource::Unknown)
  {
    source = PollSource::SerialInterface;
    s_poll_source.store(source);
  }
  return source;
}

// A poll (as counted by input_polls) has completed: advance the counter and present the pad
// state for the next poll index.
static void AdvancePoll()
{
  {
    std::lock_guard lk(s_pad_mutex);
    s_current_poll_index = s_input_polls.fetch_add(1, std::memory_order_relaxed) + 1;
    s_latched_since_read = false;
    if (g_pad_override_mask.load(std::memory_order_relaxed) != 0)
    {
      for (int port = 0; port < 4; ++port)
        RecomputePort(port, s_current_poll_index);
    }
  }
  Internal::NotifyWaiters();
}

void OnSIPollSlow()
{
  s_si_polls.fetch_add(1, std::memory_order_relaxed);
  if (DeterminePollSource() != PollSource::SerialInterface)
  {
    std::lock_guard lk(s_pad_mutex);
    s_latched_since_read = true;
    return;
  }

  // SI mode: this SI poll reads the state for index input_polls; the next poll the next index.
  {
    std::lock_guard lk(s_pad_mutex);
    s_current_poll_index = s_input_polls.load(std::memory_order_relaxed);
    if (g_pad_override_mask.load(std::memory_order_relaxed) != 0)
    {
      for (int port = 0; port < 4; ++port)
        RecomputePort(port, s_current_poll_index);
    }
    s_input_polls.fetch_add(1, std::memory_order_relaxed);
  }
  Internal::NotifyWaiters();
}

void OnGamePadReadSlow()
{
  if (DeterminePollSource() != PollSource::GamePadRead)
  {
    // The hook sits at a fixed Brawl address; only trust it for Brawl.
    if (ForceSIPollSource() || !SConfig::GetInstance().GetGameID().starts_with("RSB"))
      return;
    s_poll_source.store(PollSource::GamePadRead);
    INFO_LOG_FMT(HARNESS, "input_polls now counts Brawl's per-frame pad reads (at poll {})",
                 s_input_polls.load());
  }
  // The game just consumed the latched pad data for poll index input_polls. Every SI poll from
  // now until the game's next read latches the state for the next index.
  AdvancePoll();
}

void OnVIFieldSlow(Core::System& system)
{
  const u64 frame = s_frame.fetch_add(1, std::memory_order_relaxed) + 1;
  const u64 target = s_break_at_frame.load(std::memory_order_relaxed);
  if (target != 0 && frame >= target)
  {
    s_break_at_frame.store(0);
    system.GetCPU().Break();
  }
  Internal::NotifyWaiters();
}
}  // namespace detail

std::optional<u16> ResolvePort(const std::string& command_line_value)
{
  std::string value = command_line_value;
  if (value.empty())
  {
    const char* env = std::getenv("PPR_HARNESS_PORT");
    if (env)
      value = env;
  }
  if (value.empty())
    return std::nullopt;

  u32 port = 0;
  if (!TryParse(value, &port) || port == 0 || port > 0xFFFF)
  {
    fprintf(stderr, "Invalid harness port '%s'\n", value.c_str());
    return std::nullopt;
  }
  return static_cast<u16>(port);
}

bool Start(u16 port, HostCallbacks callbacks)
{
  if (detail::g_active.load())
    return true;

  s_host = std::move(callbacks);

  // Session defaults (see docs/harness-protocol.md, "Defaults").
  const char* audio_env = std::getenv("PPR_HARNESS_AUDIO");
  if (!audio_env || std::string(audio_env) != "1")
    Config::SetCurrent(Config::MAIN_AUDIO_MUTED, true);

  // log_mark must always reach dolphin.log, regardless of Logger.ini.
  if (auto* log_manager = Common::Log::LogManager::GetInstance())
    log_manager->SetEnable(Common::Log::LogType::HARNESS, true);

  Internal::InstallCrashReporter();
  s_state_hook = Core::AddOnStateChangedCallback(OnStateChanged);

  if (!Internal::StartServer(port))
  {
    s_state_hook.reset();
    return false;
  }

  detail::g_active.store(true);
  Internal::RegisterOnlineTestBackends();
  // DolphinNoGUI has no netplay UI of its own: the harness gives the headless netplay session
  // its boot callback, which also registers the whole-machine online backend. (DolphinQt does
  // this itself once its main window exists, harness or not.)
  if (s_host.supports_netplay && s_host.boot)
  {
    Online::NetPlaySession::SetFrontend({s_host.frontend_name, s_host.boot, {}});
    Online::Session::SelectConfigured();
  }
  NOTICE_LOG_FMT(HARNESS, "Harness listening on 127.0.0.1:{} ({})", port, s_host.frontend_name);
  return true;
}

void Shutdown()
{
  if (!detail::g_active.load())
    return;

  Online::Client::Shutdown();
  Online::NetPlaySession::Shutdown();
  Internal::StopServer();

  detail::g_active.store(false);
  detail::g_pad_override_mask.store(0);
  s_state_hook.reset();
}

namespace Internal
{
const HostCallbacks& GetHost()
{
  return s_host;
}

u64 GetFrame()
{
  return s_frame.load(std::memory_order_relaxed);
}

u64 GetInputPolls()
{
  return s_input_polls.load(std::memory_order_relaxed);
}

u64 GetSIPolls()
{
  return s_si_polls.load(std::memory_order_relaxed);
}

const char* GetPollSourceName()
{
  switch (s_poll_source.load(std::memory_order_relaxed))
  {
  case PollSource::SerialInterface:
    return "si";
  case PollSource::GamePadRead:
    return "game";
  default:
    return "unknown";
  }
}

bool WaitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
  std::unique_lock lk(s_wait_mutex);
  // Also poll periodically so that conditions depending on state we are not notified about
  // (e.g. netplay connection state) are still noticed.
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred())
  {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      return pred();
    s_wait_cv.wait_until(lk, std::min(deadline, now + std::chrono::milliseconds(50)));
  }
  return true;
}

void NotifyWaiters()
{
  {
    std::lock_guard lk(s_wait_mutex);
  }
  s_wait_cv.notify_all();
}

void PadSet(int port, const GCPadStatus& status)
{
  std::lock_guard lk(s_pad_mutex);
  PortState& ps = s_ports[port];
  ps.set = status;
  ps.neutral = false;
  RecomputePort(port, s_current_poll_index);
}

void PadClear(int port)
{
  std::lock_guard lk(s_pad_mutex);
  PortState& ps = s_ports[port];
  ps = PortState{};
  RecomputePort(port, s_current_poll_index);
}

std::optional<std::string> PadScript(int port, std::vector<GCPadStatus> timeline,
                                     std::optional<u64> start, ScriptTiming* timing)
{
  std::lock_guard lk(s_pad_mutex);
  // The first poll whose pad data has not started being latched yet. In GamePadRead mode the SI
  // polls since the game's last read have already latched data for input_polls, so a script
  // starting there could lose its first entry; start one game read later instead.
  const bool latched =
      s_poll_source.load(std::memory_order_relaxed) == PollSource::GamePadRead &&
      s_latched_since_read;
  const u64 next_poll = s_input_polls.load(std::memory_order_relaxed) + (latched ? 1 : 0);
  const u64 starts_at = start.value_or(next_poll);
  if (starts_at < next_poll)
    return fmt::format("start {} is too early (the earliest poll that can still be scheduled is {})",
                       starts_at, next_poll);

  PortState& ps = s_ports[port];
  // Bridge the polls until the new script starts with what the port presents now.
  if (ps.overriding)
    ps.gap = ps.current;
  else
    ps.gap.reset();
  ps.script = std::move(timeline);
  ps.script_start = starts_at;
  ps.script_active = !ps.script.empty();
  if (!ps.script_active && !ps.set)
    ps.neutral = true;
  RecomputePort(port, s_current_poll_index);

  timing->starts_at = starts_at;
  timing->ends_at = starts_at + ps.script.size();
  return std::nullopt;
}

ScriptStatus PadScriptStatus(int port)
{
  std::lock_guard lk(s_pad_mutex);
  const PortState& ps = s_ports[port];
  if (!ps.script_active)
    return {false, 0};
  const u64 polls = s_input_polls.load(std::memory_order_relaxed);
  const u64 end = ps.script_start + ps.script.size();
  return {polls < end, polls < end ? end - polls : 0};
}

void RequestBreakAtFrame(u64 target)
{
  s_break_at_frame.store(target);
}

void CancelBreakAtFrame()
{
  s_break_at_frame.store(0);
}
}  // namespace Internal
}  // namespace Harness
