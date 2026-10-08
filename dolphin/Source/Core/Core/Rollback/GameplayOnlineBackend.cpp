// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/GameplayOnlineBackend.h"

#include <mutex>
#include <optional>
#include <utility>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Rollback/GameplaySession.h"

namespace Gprb
{
namespace
{
std::mutex s_options_mutex;
OnlineBackendOptions s_options;

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
    if (match.players.size() != 2)
      return fmt::format("the gameplay session supports 1 on 1 only ({} players)",
                         match.players.size());
    // The address the P2P connection came up with is the one the peer's NAT maps.
    const Online::Endpoint& peer = match.connected.empty() ? match.remotes[0] : match.connected[0];

    Session::ConnectOptions o;
    o.host = match.is_host;
    o.local_port = match.local_port;
    // Both sides know the other's address: the host sends from the start too (keeps the NAT
    // mapping toward the guest open, as Slippi connects from both sides).
    o.remote_host = peer.ip;
    o.remote_port = peer.port;
    o.region_set = options.region_set;
    o.delay = options.delay;
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
}
}  // namespace Gprb
