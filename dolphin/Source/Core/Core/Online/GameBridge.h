// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The game <-> Dolphin channel of docs/backend-design.md 5.2: Dolphin's side of the PPOM mailbox
// that our game plugin (game-code/PPOnline, include/ppom.h) keeps in its own .data. It is what
// Slippi's EXI device does with the game's DMA commands (GET_ONLINE_STATUS, FIND_OPPONENT,
// GET_MATCH_STATE, CLEANUP_CONNECTION), done with memory instead of EXI so that nothing reaches
// the simulation except at one frame boundary:
//
// - Locate: the plugin is a REL with a fixed module id (PPOM_MODULE_ID). Its block is found by
//   walking the game's own OSModuleInfo list (0x800030C8) to that module and checking its data
//   sections for the "PPOM" header. That happens once per boot (retried every 30 frames until the
//   plugin is loaded); after that each frame checks one word (the magic) at the known address.
// - Service: OnFrameEnd() runs from the BrawlbackGekkoNetFrameEnd HLE hook on the CPU thread, once
//   per game frame. It consumes requests in order and writes at most one response per frame, and
//   only after the game has taken the previous one (respSeen == respCount), so no response is
//   ever overwritten unread.
// - Rollback: the mailbox is excluded from rollback state (RollbackManager::SetMailboxRegion), and
//   it is never serviced while a netplay session runs: during a session nothing that differs
//   between the machines may reach game memory.
//
// Requests map onto Online::Client: 0xB9 -> the User (name, code, app state), 0xB4 ->
// Client::FindMatch (mode + code, hand-off to the session backend), 0xB3 -> the matchmaking state
// (Slippi's ProcessState, peer name and code, the server's error text, session phase), 0xBA ->
// Client::Cleanup.

#pragma once

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Online::GameBridge
{
// Our plugin's REL module id (game-code/PPOnline/Makefile RELID).
constexpr u32 PPOM_MODULE_ID = 20560;

// 3-4 player matches (docs/nplayer/setup.md). Offsets in SESSION (ppom.h Session), which is part
// of the rolled-back state:
// - SESSION_GONE: u8[4] by in-game port. The session writes 1 for a player who dropped out of a
//   running match at the start of every frame from the agreed frame on (resimulated frames too,
//   like an input; never cleared during the match). The game then removes that fighter.
// - player i's elimination order at SESSION_PLAYERS_OFF + i * SESSION_PLAYER_SIZE +
//   SESSION_PLAYER_OUT (u8, game-written every frame: 1 = first out, shared by ports out on the
//   same frame, 0 = still in), read at the end of a game for the placings (GameSetup::Outcome).
constexpr u32 SESSION_GONE = 0x20C;
constexpr u32 SESSION_PLAYERS_OFF = 0x0C;
constexpr u32 SESSION_PLAYER_SIZE = 0x80;
constexpr u32 SESSION_PLAYER_OUT = 0x37;
// - SESSION_QUIT_PORTS: u8, bit p = port p left the match with the pause screen's quit
//   (L+R+A+START), game-written in the frame of the quit; SESSION_QUIT_STAYS: the quits that left
//   one player or team in (the game set ends the session for everyone, the quitter included).
constexpr u32 SESSION_QUIT_PORTS = 0x21B;
constexpr u32 SESSION_QUIT_STAYS = 0x21C;
// - SESSION_QUIT_BY: u8, the in-game port + 1 of the player whose pause-screen quit ended the
//   match for everyone (a 1v1, a 2v2 set), 0 none; SESSION_QUIT_RUNBACK: 1 when L+R+Y were held
//   with it or up to 10 frames after (P+'s salty runback). Game-written in the frame of the quit
//   (quitRunback up to 10 frames later), rolled back with the match, cleared at its setup.
constexpr u32 SESSION_QUIT_BY = 0x21D;
constexpr u32 SESSION_QUIT_RUNBACK = 0x21E;
// A game played again after a desync (Gprb::Session::Restore; docs/game-code.md, desync
// recovery), from the setup, the same on every machine: SESSION_RESTORE u8 1, then per player its
// stocks (u8, 0 out of the game) and damage (f32) and the match's time limit in frames (u32 in the
// rules, 0 the rules' own).
constexpr u32 SESSION_RESTORE = 0x21F;
constexpr u32 SESSION_PLAYER_RESTORE_STOCKS = 0x3C;
constexpr u32 SESSION_PLAYER_RESTORE_DAMAGE = 0x7C;
constexpr u32 SESSION_RESTORE_FRAMES = 0x234;
// SESSION's sizes: the oldest plugin's (PPOM v4/v5) and the newest's (v9). A replay's recorded
// SESSION is any size in between (the recorder's plugin's).
constexpr u32 SESSION_MIN_SIZE = 0x220;
constexpr u32 SESSION_MAX_SIZE = 0x420;
// SESSION's address in game memory, 0 before the plugin's block is found, and its size (the
// plugin's PPOM version's).
u32 SessionAddress();
u32 SessionSize();

// From the frame-end HLE hook (CPU thread), every game frame.
void OnFrameEnd(const Core::CPUThreadGuard& guard);
// A new boot: forget the block.
void Reset();

// Servicing on/off (on by default; the harness can turn it off to play Dolphin's part itself).
void SetEnabled(bool enabled);
// Whether FIND_OPPONENT hands the connected match to the session backend (default true).
void SetHandOff(bool hand_off);

// For the harness (`game_bridge_status`).
picojson::object Status();
}  // namespace Online::GameBridge
