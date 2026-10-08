// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Online-play backends that only exist for tests.

#include <mutex>
#include <optional>
#include <string>

#include <picojson.h>

#include "Common/Logging/Log.h"
#include "Core/Harness/HarnessInternal.h"
#include "Core/Online/OnlineSession.h"

namespace Harness::Internal
{
namespace
{
// A backend for tests of the game's online menus: it records the hand-off and holds the P2P
// link open, so the game stays on its character select with the opponent shown instead of being
// stopped for the netplay boot. Stop() closes the link.
class RecordOnlineBackend final : public Online::SessionBackend
{
public:
  const char* Name() const override { return "record"; }

  std::optional<std::string> Start(const Online::Match& match, Online::P2PLink link,
                                   const picojson::object& selections) override
  {
    std::lock_guard lk(m_mutex);
    m_match = match;
    m_selections = selections;
    m_link = std::move(link);
    m_phase = "held";
    ++m_starts;
    NOTICE_LOG_FMT(NETPLAY, "Online session (record): Start match {} ({})", match.match_id,
                   match.is_host ? "host" : "guest");
    return std::nullopt;
  }

  void Stop() override
  {
    Online::P2PLink link;
    {
      std::lock_guard lk(m_mutex);
      link = std::move(m_link);
      if (!m_phase.empty())
        m_phase = "ended";
    }
    if (link.IsOpen())
      link.Release(200);
  }

  Online::SessionStatus Status() const override
  {
    Online::SessionStatus status;
    std::lock_guard lk(m_mutex);
    status.phase = m_phase;
    status.detail["starts"] = picojson::value(static_cast<double>(m_starts));
    status.detail["link_open"] = picojson::value(m_link.IsOpen());
    if (m_match)
    {
      status.detail["role"] = picojson::value(m_match->is_host ? "host" : "guest");
      status.detail["local_port"] = picojson::value(static_cast<double>(m_match->local_port));
      status.detail["match_id"] = picojson::value(m_match->match_id);
      for (const auto& p : m_match->players)
      {
        if (!p.is_local)
        {
          status.detail["peer_name"] = picojson::value(p.display_name);
          status.detail["peer_code"] = picojson::value(p.connect_code);
        }
      }
      status.detail["selections"] = picojson::value(m_selections);
    }
    return status;
  }

private:
  mutable std::mutex m_mutex;
  std::optional<Online::Match> m_match;
  picojson::object m_selections;
  Online::P2PLink m_link;
  std::string m_phase;
  int m_starts = 0;
};
}  // namespace

void RegisterOnlineTestBackends()
{
  Online::Session::RegisterFactory("record", [] { return std::make_unique<RecordOnlineBackend>(); });
}
}  // namespace Harness::Internal
