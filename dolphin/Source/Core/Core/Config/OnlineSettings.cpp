// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Config/OnlineSettings.h"

namespace Config
{
namespace
{
constexpr char DEV_HOST[] = "127.0.0.1";
}  // namespace

const Info<std::string> ONLINE_MM_HOST{{System::Main, "Online", "MatchmakingHost"},
                                       "mm.brawlonline.net"};
const Info<int> ONLINE_MM_PORT{{System::Main, "Online", "MatchmakingPort"}, 43113};
const Info<bool> ONLINE_USE_DEV_SERVER{{System::Main, "Online", "UseDevServer"}, false};
const Info<std::string> ONLINE_ACCOUNTS_URL{{System::Main, "Online", "AccountsUrl"},
                                            "https://brawlonline.net"};
const Info<std::string> ONLINE_DEV_ACCOUNTS_URL{{System::Main, "Online", "DevAccountsUrl"},
                                                "http://127.0.0.1:8080"};

const Info<bool> ONLINE_FORCE_NETPLAY_PORT{{System::Main, "Online", "ForceNetplayPort"}, false};
const Info<int> ONLINE_NETPLAY_PORT{{System::Main, "Online", "NetplayPort"}, 2626};
const Info<bool> ONLINE_FORCE_LAN_IP{{System::Main, "Online", "ForceLanIP"}, false};
const Info<std::string> ONLINE_LAN_IP{{System::Main, "Online", "LanIP"}, ""};

// "gameplay": the gameplay-only (Slippi-style) session; "netplay": the whole-machine netplay
// session (synchronized reboot), kept as a fallback.
const Info<std::string> ONLINE_SESSION_BACKEND{{System::Main, "Online", "SessionBackend"},
                                               "gameplay"};

std::string GetMatchmakingHost()
{
  return Get(ONLINE_USE_DEV_SERVER) ? std::string(DEV_HOST) : Get(ONLINE_MM_HOST);
}

int GetMatchmakingPort()
{
  return Get(ONLINE_MM_PORT);
}

std::string GetAccountsUrl()
{
  std::string url =
      Get(ONLINE_USE_DEV_SERVER) ? Get(ONLINE_DEV_ACCOUNTS_URL) : Get(ONLINE_ACCOUNTS_URL);
  while (!url.empty() && url.back() == '/')
    url.pop_back();
  return url;
}
}  // namespace Config
