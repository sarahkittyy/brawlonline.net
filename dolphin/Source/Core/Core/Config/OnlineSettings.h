// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"
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
// "Force LAN IP": report this address as ipAddressLan instead of the detected one. The launcher's
// "Network for online games" sets it to an adapter's "a.b.c.d/prefix" (GetChosenNetwork).
extern const Info<bool> ONLINE_FORCE_LAN_IP;
extern const Info<std::string> ONLINE_LAN_IP;
// Ask the router (UPnP) to forward that port while searching, for routers that change the port
// for every destination (Matchmaking::StartMatchmaking). Not with a matchmaking server on this
// machine (the tests).
extern const Info<bool> ONLINE_UPNP;

// The session backend a connected match is handed to (Online::Session): "gameplay" (the
// gameplay-only, Slippi-style session, the default), "netplay" (whole-machine rollback netplay
// after a synchronized reboot, the fallback) or "none" (keep the P2P link; nothing starts).
extern const Info<std::string> ONLINE_SESSION_BACKEND;

// This player's input delay in online matches (the launcher's setting): a profile, picked per game
// from the round trip (Gprb::Session::ProfileInputDelay): 0 Normal, -1 Aggressive, -2 Smooth
// (Gprb::Session::DELAY_*); or 1-9 frames for this player's own inputs, as Slippi's delay frames,
// in Direct and rooms only (Ranked and Unranked play Normal for it).
extern const Info<int> ONLINE_INPUT_DELAY;

// The launcher's "Matchmaking Ping Limit" (ms; 100, 150 or 200): Unranked and Ranked tickets send
// it as pingLimit, and mm pairs this player with no one whose estimated ping is above it. 0 or
// less: no limit of our own.
extern const Info<int> ONLINE_MM_PING_LIMIT;

// The game's music (the launcher's setting). Off applies P+'s "[P+] Music Off" patch, which
// zeroes the music volume every frame (docs/gameplay-rollback-status.md, Phase 20).
extern const Info<bool> ONLINE_MUSIC;

// The Project+ release on the card the launcher gives this Dolphin ("v3.2.1"; empty: not said).
// Replays record it (INFO build.pplus), so the launcher plays each on its own release's files.
extern const Info<std::string> ONLINE_PPLUS_RELEASE;

// The chat window (docs/chat-protocol.md §6): shown while a match runs, and where it was put
// ("x y w h collapsed", the first four as fractions of the window; empty: the default place).
extern const Info<bool> ONLINE_CHAT_IN_MATCHES;
extern const Info<std::string> ONLINE_CHAT_WINDOW;
// The chat's text size, a percentage of its usual size (Options > Increase / Decrease font size).
extern const Info<int> ONLINE_CHAT_TEXT_SIZE;
// The key that opens the chat box (the launcher's setting): an input expression for the hotkeys'
// device, such as "`T`" (HotkeyManagerEmu::LoadConfig puts it on "Activate NetPlay Chat"). Empty:
// the hotkey keeps what Hotkeys.ini has (T by default).
extern const Info<std::string> ONLINE_CHAT_KEY;
// The launcher's "Disable Chat": no chat window anywhere (rooms, menus, matches), and the chat
// hotkey opens nothing.
extern const Info<bool> ONLINE_DISABLE_CHAT;

// Replays (docs/design/replays.md §4.5; the launcher's Settings > Replays writes them): every online
// game is saved into ReplayDir (empty: <User>/Replays), in a folder per month (YYYY-MM) when
// ReplayMonthlyFolders is on.
extern const Info<bool> ONLINE_SAVE_REPLAYS;
extern const Info<std::string> ONLINE_REPLAY_DIR;
// Spectating (docs/design/spectating.md): this player's game uploads its games' inputs to the
// server for spectators. The launcher's "Show my games on the Spectate tab". Off: the game sends
// nothing for spectating (the other players' games may still stream the match), except in Ranked,
// whose games are always streamed as the server's record and listed only when a player allows it.
extern const Info<bool> ONLINE_STREAM_GAMES;
extern const Info<bool> ONLINE_REPLAY_MONTHLY_FOLDERS;

// The matchmaking host and port in effect (the dev override applied).
std::string GetMatchmakingHost();
int GetMatchmakingPort();
// The accounts base URL in effect, without a trailing slash.
std::string GetAccountsUrl();

// The network the player chose in the launcher ("Network for online games": a VPN adapter's,
// such as ZeroTier's, for players whose NAT lets no one in): with Force LAN IP on and LanIP
// "a.b.c.d/prefix", the address to report as ours and its network. Players picked by name
// (Direct, rooms, Brawl Mods rooms) whose reported LAN address is on that network are reached
// there. A plain "a.b.c.d" (a hand-set Force LAN IP, as Slippi's) has no network (mask 0).
struct ChosenNetwork
{
  std::string address;  // empty: not forced
  u32 network = 0;
  u32 mask = 0;
  // Whether "a.b.c.d" is on the network.
  bool Contains(const std::string& ip) const;
};
ChosenNetwork GetChosenNetwork();
// "a.b.c.d" -> the address as a number (a in the top byte), nullopt for anything else.
std::optional<u32> ParseIpv4(const std::string& text);
}  // namespace Config
