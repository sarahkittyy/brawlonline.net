// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/GameplayOnlineBackend.h"

#include <mutex>
#include <optional>
#include <utility>

#include <fmt/format.h>

#include "Common/HookableEvent.h"
#include "Common/Logging/Log.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/Core.h"
#include "Core/Rollback/GameplaySession.h"

namespace Gprb
{
namespace
{
std::mutex s_options_mutex;
OnlineBackendOptions s_options;

// Closing the game window stops emulation (MainWindow::ForceStop), but the leave used to go out
// only when the process exited (Online::Client::Shutdown() from ~MainWindow), seconds later; the
// other players stalled on the missing input meanwhile, up to the 7.2 s silence timeout. The game
// is gone once emulation stops, so the session is left then. Core::Stop notifies Stopping before
// it stops the CPU: the leave goes out while the match loop still runs.
Common::EventHook s_stop_hook;

class GameplayOnlineBackend final : public Online::SessionBackend
{
public:
  const char* Name() const override { return "gameplay"; }

  std::optional<std::string> Start(const Online::Match& match, Online::P2PLink link,
                                   const picojson::object& selections) override
  {
    OnlineBackendOptions options;
    {
      std::lock_guard lk(s_options_mutex);
      options = s_options;
    }
    if (match.connected.empty() && match.remotes.empty())
      return std::string("the match has no remote player");
    if (match.players.size() < 2 ||
        match.players.size() > static_cast<size_t>(Session::MAX_LOBBY_PLAYERS))
    {
      return fmt::format("the gameplay session supports 2-{} players ({} players)",
                         Session::MAX_LOBBY_PLAYERS, match.players.size());
    }
    // The address the P2P connection came up with is the one the peer's NAT maps.
    const auto endpoint = [&](size_t i) -> const Online::Endpoint* {
      if (i < match.connected.size())
        return &match.connected[i];
      return i < match.remotes.size() ? &match.remotes[i] : nullptr;
    };
    const Online::Endpoint& peer = *endpoint(0);

    Session::ConnectOptions o;
    o.host = match.is_host;
    o.local_port = match.local_port;
    // Both sides know the other's address: the host sends from the start too (keeps the NAT
    // mapping toward the guest open, as Slippi connects from both sides).
    o.remote_host = peer.ip;
    o.remote_port = peer.port;
    // A room's game: every player on their slot also with 2 players (slots are ports, gaps
    // allowed) and the room's host decides (docs/rooms-protocol.md §3), not whoever claims it.
    const bool room = match.room_host_port >= 1 &&
                      match.room_host_port <= Session::MAX_LOBBY_PLAYERS;
    if (match.players.size() > 2 || room)
    {
      // 3-4 players: everyone on their server port (1-4), every pair connected directly (the
      // remotes and connected lists follow `players` without the local one).
      size_t remote = 0;
      u32 ports = 0;
      for (const auto& p : match.players)
      {
        if (p.port < 1 || p.port > Session::MAX_LOBBY_PLAYERS || (ports & (1u << (p.port - 1))))
          return fmt::format("bad player port {} in the match", p.port);
        ports |= 1u << (p.port - 1);
        if (p.is_local)
        {
          o.local_slot = p.port - 1;
          continue;
        }
        const Online::Endpoint* e = endpoint(remote++);
        if (!e)
          return std::string("a remote player has no address");
        o.peers.push_back({p.port - 1, e->ip, e->port});
      }
      if (o.local_slot < 0)
        return std::string("the match has no local player");
      if (room)
      {
        if (!(ports & (1u << (match.room_host_port - 1))))
          return fmt::format("the room's host P{} is not in the match", match.room_host_port);
        o.host_slot = match.room_host_port - 1;
        o.host = o.local_slot == o.host_slot;
        if (o.host != match.is_host)
        {
          WARN_LOG_FMT(BRAWLBACK, "gprb: the server's isHost ({}) is not the room's host P{}; "
                                  "the room's host decides",
                       match.is_host, match.room_host_port);
        }
        o.teams = match.room_teams;
        o.initial_pickers = match.room_pickers & ports;
      }
    }
    o.region_set = options.region_set;
    // The harness's option, else the player's setting (0: automatic).
    o.delay = options.delay;
    if (const int delay = Config::Get(Config::ONLINE_INPUT_DELAY);
        !o.delay && delay > 0 && delay <= Session::MAX_INPUT_DELAY)
    {
      o.delay = delay;
    }
    o.dedupe_resim_sounds = options.dedupe_resim_sounds;
    // The match's stages: the server's list for this mode (get-ticket-resp `stages`), else P+'s
    // legal list (the session's DefaultStages(), as Slippi falls back to its default list).
    o.stages = match.stages;
    if (match.stages.empty())
      WARN_LOG_FMT(BRAWLBACK, "gprb: the server sent no stage list; using P+'s legal list");
    for (const auto& p : match.players)
    {
      if (p.is_local)
        o.name = p.display_name;
    }

    // Free the punched port (polite ENet disconnect, acknowledged), then bind it again.
    link.Release();
    if (auto err = Session::Connect(o))
      return err;
    if (auto err = Session::SetSelections(selections))
      return err;
    {
      std::lock_guard lk(m_mutex);
      m_match = match;
    }
    NOTICE_LOG_FMT(BRAWLBACK, "gprb: online match {} as {} on port {}, peer {}", match.match_id,
                   match.is_host ? "host" : "guest", match.local_port, peer.ToString());
    return std::nullopt;
  }

  void Stop() override { Session::Stop(); }

  Online::SessionStatus Status() const override
  {
    Online::SessionStatus status;
    const picojson::value st = Session::Status();
    if (st.is<picojson::object>())
    {
      const auto& o = st.get<picojson::object>();
      status.detail = o;
      if (const auto it = o.find("phase"); it != o.end() && it->second.is<std::string>())
        status.phase = it->second.get<std::string>();
      if (const auto it = o.find("error"); it != o.end() && it->second.is<std::string>())
        status.error = it->second.get<std::string>();
    }
    std::lock_guard lk(m_mutex);
    if (m_match)
    {
      status.detail["match_id"] = picojson::value(m_match->match_id);
      status.detail["is_host"] = picojson::value(m_match->is_host);
    }
    return status;
  }

private:
  mutable std::mutex m_mutex;
  std::optional<Online::Match> m_match;
};
}  // namespace

void SetOnlineBackendOptions(const OnlineBackendOptions& options)
{
  std::lock_guard lk(s_options_mutex);
  s_options = options;
}

std::unique_ptr<Online::SessionBackend> MakeOnlineBackend()
{
  return std::make_unique<GameplayOnlineBackend>();
}

void RegisterOnlineBackend()
{
  Online::Session::RegisterFactory("gameplay", MakeOnlineBackend);
  if (!s_stop_hook)
  {
    s_stop_hook = Core::AddOnStateChangedCallback([](Core::State state) {
      if (state != Core::State::Stopping || !Session::GetLobby().active)
        return;
      NOTICE_LOG_FMT(BRAWLBACK, "gprb: emulation is stopping; leaving the session");
      Session::Stop();
    });
  }
}
}  // namespace Gprb
