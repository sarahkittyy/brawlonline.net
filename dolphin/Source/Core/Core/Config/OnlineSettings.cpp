// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Config/OnlineSettings.h"

#include <algorithm>

#include "Common/StringUtil.h"

namespace Config
{
namespace
{
constexpr char DEV_HOST[] = "127.0.0.1";

bool IsAsciiDigit(char c)
{
  return c >= '0' && c <= '9';
}

bool AllDigits(const std::string& text, size_t max_size)
{
  return !text.empty() && text.size() <= max_size &&
         std::all_of(text.begin(), text.end(), IsAsciiDigit);
}
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
const Info<bool> ONLINE_UPNP{{System::Main, "Online", "UPnP"}, true};

// "gameplay": the gameplay-only (Slippi-style) session; "netplay": the whole-machine netplay
// session (synchronized reboot), kept as a fallback.
const Info<std::string> ONLINE_SESSION_BACKEND{{System::Main, "Online", "SessionBackend"},
                                               "gameplay"};
const Info<int> ONLINE_INPUT_DELAY{{System::Main, "Online", "InputDelay"}, 0};
const Info<int> ONLINE_MM_PING_LIMIT{{System::Main, "Online", "MatchmakingPingLimit"}, 150};
const Info<bool> ONLINE_MUSIC{{System::Main, "Online", "Music"}, true};
const Info<bool> ONLINE_CHAT_IN_MATCHES{{System::Main, "Online", "ChatInMatches"}, true};
const Info<std::string> ONLINE_CHAT_WINDOW{{System::Main, "Online", "ChatWindow"}, ""};
const Info<int> ONLINE_CHAT_TEXT_SIZE{{System::Main, "Online", "ChatTextSize"}, 100};
const Info<std::string> ONLINE_CHAT_KEY{{System::Main, "Online", "ChatKey"}, ""};
const Info<bool> ONLINE_DISABLE_CHAT{{System::Main, "Online", "DisableChat"}, false};
const Info<bool> ONLINE_SAVE_REPLAYS{{System::Main, "Online", "SaveReplays"}, true};
const Info<std::string> ONLINE_REPLAY_DIR{{System::Main, "Online", "ReplayDir"}, ""};
const Info<bool> ONLINE_STREAM_GAMES{{System::Main, "Online", "StreamGames"}, true};
const Info<std::string> ONLINE_PPLUS_RELEASE{{System::Main, "Online", "PPlusRelease"}, ""};
const Info<bool> ONLINE_REPLAY_MONTHLY_FOLDERS{{System::Main, "Online", "ReplayMonthlyFolders"},
                                              true};

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

std::optional<u32> ParseIpv4(const std::string& text)
{
  const auto parts = SplitString(text, '.');
  if (parts.size() != 4)
    return std::nullopt;
  u32 v = 0;
  for (const auto& p : parts)
  {
    if (!AllDigits(p, 3) || std::stoi(p) > 255)
      return std::nullopt;
    v = (v << 8) | static_cast<u32>(std::stoi(p));
  }
  return v;
}

bool ChosenNetwork::Contains(const std::string& ip) const
{
  const std::optional<u32> v = ParseIpv4(ip);
  return mask != 0 && v && (*v & mask) == network;
}

ChosenNetwork GetChosenNetwork()
{
  ChosenNetwork n;
  if (!Get(ONLINE_FORCE_LAN_IP))
    return n;
  const auto parts = SplitString(Get(ONLINE_LAN_IP), '/');
  n.address = parts.empty() ? std::string() : parts[0];
  const std::optional<u32> ip = ParseIpv4(n.address);
  if (parts.size() != 2 || !ip || !AllDigits(parts[1], 2))
    return n;
  int prefix = std::stoi(parts[1]);
  // Tailscale's adapter has a /32 address out of 100.64.0.0/10, the range all its peers get.
  if (prefix == 32 && (*ip & 0xFFC00000) == 0x64400000)
    prefix = 10;
  // No network of one address, nor one as wide as the internet.
  if (prefix < 8 || prefix > 30)
    return n;
  n.mask = ~u32{0} << (32 - prefix);
  n.network = *ip & n.mask;
  return n;
}

}  // namespace Config
