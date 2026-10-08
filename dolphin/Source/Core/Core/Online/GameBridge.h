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
