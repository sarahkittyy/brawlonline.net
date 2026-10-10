// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/Config/Config.h"

namespace Config
{
// [Online] in Dolphin.ini. The launcher syncs its own keys into the same section (ReplayDir,
// SaveReplays, ReplayMonthlyFolders, InputDelay; see launcher/src/common/product.ts). The key names of the
// port and LAN settings are Slippi's ([Slippi] ForceNetplayPort etc.).

// Matchmaking server. Slippi hard-codes mm.slippi.gg:43113 (mm2.slippi.gg for dev builds); ours
// comes from here so the dev and test setups can point at a local server.
extern const Info<std::string> ONLINE_MM_HOST;
extern const Info<int> ONLINE_MM_PORT;
// Dev override (Slippi's dev host): matchmaking on 127.0.0.1:MatchmakingPort and the accounts
// service at DevAccountsUrl.
extern const Info<bool> ONLINE_USE_DEV_SERVER;
// Accounts service base URL, for the users-rest lookup after a login (`GET /user/{uid}`).
extern const Info<std::string> ONLINE_ACCOUNTS_URL;
extern const Info<std::string> ONLINE_DEV_ACCOUNTS_URL;

// "Force Netplay Port": bind this UDP port for matchmaking and the P2P connection instead of a
// random one in 41000-50999.
extern const Info<bool> ONLINE_FORCE_NETPLAY_PORT;
extern const Info<int> ONLINE_NETPLAY_PORT;
// "Force LAN IP": report this address as ipAddressLan instead of the detected one.
extern const Info<bool> ONLINE_FORCE_LAN_IP;
extern const Info<std::string> ONLINE_LAN_IP;

// The session backend a connected match is handed to (Online::Session): "gameplay" (the
// gameplay-only, Slippi-style session, the default), "netplay" (whole-machine rollback netplay
// after a synchronized reboot, the fallback) or "none" (keep the P2P link; nothing starts).
extern const Info<std::string> ONLINE_SESSION_BACKEND;

// This player's input delay in online matches (the launcher's setting): 0 picks it per game from
// the round trip (Gprb::Session::AutoInputDelay), 1-9 sets it. Each player's applies to their own
// inputs, as Slippi's delay frames do.
extern const Info<int> ONLINE_INPUT_DELAY;

// The chat window (docs/chat-protocol.md §6): shown while a match runs, and where it was put
// ("x y w h collapsed", the first four as fractions of the window; empty: the default place).
extern const Info<bool> ONLINE_CHAT_IN_MATCHES;
extern const Info<std::string> ONLINE_CHAT_WINDOW;

// The matchmaking host and port in effect (the dev override applied).
std::string GetMatchmakingHost();
int GetMatchmakingPort();
// The accounts base URL in effect, without a trailing slash.
std::string GetAccountsUrl();
}  // namespace Config
