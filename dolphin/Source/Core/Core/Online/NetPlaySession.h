// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A Dolphin netplay session (NetPlayServer + NetPlayClient) with no netplay UI, and the
// whole-machine online session backend built on it.
//
// Online play hands a matched peer to a session backend (OnlineSession.h). The whole-machine
// backend ("netplay", the synchronized boot of docs/backend-design.md 5.1 option A) turns the
// match into an ordinary rollback netplay session: the decider hosts on its punched port and
// boots the game once the guest has joined; the guest joins from its punched port. Like Slippi,
// no netplay window appears: the session is driven from here, with a NetPlayUI that has no UI.
//
// A frontend makes this available by calling SetFrontend() with a way to boot a game. DolphinQt
// does it at start-up (its MainWindow boots the game, so the render window is the usual one);
// DolphinNoGUI does it under the automation harness, which also drives Host/Join/Start directly
// (netplay_host, netplay_join, netplay_start).

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <picojson.h>

#include "Common/CommonTypes.h"

struct BootParameters;

namespace Online
{
class SessionBackend;
}

namespace Online::NetPlaySession
{
struct Frontend
{
  // "DolphinQt", "DolphinNoGUI"
  std::string name;
  // Boots the game netplay starts. Called on the host thread. Returns false on failure.
  std::function<bool(std::unique_ptr<BootParameters>)> boot;
  // Whether the frontend runs a netplay session of its own (DolphinQt's NetPlay dialog); this
  // module then refuses to start one. Optional.
  std::function<bool()> other_session_active;
};

// Makes headless sessions available and registers the "netplay" session backend factory with
// Online::Session. Call once the frontend can boot games.
void SetFrontend(Frontend frontend);
bool HasFrontend();

std::optional<std::string> Host(u16 port, const std::string& game, const std::string& name,
                                bool rollback, std::optional<int> delay);
// local_port: UDP port the client binds (0 = any; online play passes its punched port).
std::optional<std::string> Join(const std::string& host, u16 port, const std::string& name,
                                const std::string& game_hint, u16 local_port = 0);
std::optional<std::string> Start();
std::optional<std::string> Leave();
bool IsActive();
// Tears down the session (host thread).
void ShutdownSession();

// JSON for the harness's netplay_status (null without a session).
picojson::value Status();
// Rollback pad history (see NetPlayClient::GetPadHistory) from `since_frame` on; null without a
// netplay client.
picojson::value PadHistory(s64 since_frame, bool with_raw = false);
// Rollback chunk hashes for one frame (see NetPlayClient::GetChunkHashes); null if not kept.
picojson::value ChunkHashes(s64 frame);

// Options of the whole-machine online backend (the harness sets them per search).
struct OnlineOptions
{
  std::string game;          // what the host boots; default: Launcher/Project+ Netplay Launcher.dol
  std::optional<int> delay;  // rollback input delay (host); default: NetPlay.RollbackDelay
  bool auto_start = true;    // host: start the game when the guest is in
};
void SetOnlineOptions(const OnlineOptions& options);

// The whole-machine backend. Also what the "netplay" factory makes.
std::unique_ptr<SessionBackend> MakeOnlineBackend();

// Process exit: unregisters the session backend and ends the session. Call after
// Online::Client::Shutdown(). Safe to call more than once.
void Shutdown();
}  // namespace Online::NetPlaySession
