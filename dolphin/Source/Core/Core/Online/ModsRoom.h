// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Brawl Mods rooms (docs/design/brawl-mods.md §3, §5), Dolphin's side. A mod room has no rules or
// start: its host's whole-machine session (Rollback/MachineSession.h) runs from boot, alone, and
// every member who joins the room boots the same build, joins the session and is plugged in.
//
// - Dolphin is armed before boot from PPR_MODS_ROOM (the launcher sets it, or the harness): the
//   room to open or join and the session's options (FromEnvironment);
// - the room's requests and pushed state go over the online connection Rooms.cpp keeps (`hello`
//   first): mods-create or mods-join once it is up, again on a new connection (the same room and
//   slot; a new room when it is gone);
// - every mods-state becomes the session's members (Machine::SetRoom), and while mm has no
//   endpoint for this player the session probes mm from its UDP socket (Machine::ProbeEndpoint);
// - leaving the room (or being removed) ends the session here.
//
// Thread-safe. OnServerMessage and TakeOutbox are called on the Rooms thread without its lock.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Machine
{
struct Options;
}

namespace Online::ModsRoom
{
// Before boot (Machine's environment check). PPR_MODS_ROOM, JSON:
//   {"host": true, "build": {"hash": "<64 hex>", "name": "Brawl Minus"}, "public": true,
//    "controller": "gc" | "wiimote", "delay": 2, "port": 0, "pad": 0}
//   ("pad" left out: the first plugged-in controller of the kind, picked again as they come and go)
//   {"join": "KFQB", "build": {"hash": ..}, "controller": .., "delay": .., "port": .., "pad": ..}
// Set: the room to open or join and the options to arm the session with (alone as the room's
// host, or a joiner who takes its slot from the room). Not set: nullopt with *error empty.
std::optional<Machine::Options> FromEnvironment(std::string* error);
// The --mods-room command line option (the same JSON), taken instead of PPR_MODS_ROOM. Before boot.
void SetCommandLine(std::string json);

// UI thread: the launcher asked for Dolphin's Controllers settings (a "configure" request);
// true once per request.
bool TakeConfigureRequest();
// UI thread: Dolphin should close (a join that failed before any game was shared; the launcher
// shows why from mods-room.json, written first). True once.
bool TakeQuitRequest();
// Rooms thread: mm dropped this account's connection for another game's ("Signed in from another
// game."): out of the room, the session ends (the game keeps running locally).
void OnSignedInElsewhere();

// Rooms thread. A mods-* message from mm: handled (true); anything else: false.
bool OnServerMessage(const std::string& text);
// What to send on online connection `connection` (a new number for every connection: the room is
// created or joined again on a new one). Nothing when there is no room to be in.
std::vector<std::string> TakeOutbox(u64 connection);

// Rooms thread, every half second: the launcher's hand-off in Dolphin's online folder `dir`.
// `mods-request.json` in (launcher -> Dolphin: {version: 1, id, op: "leave" | "kick" | "slot" |
// "public" | "controller", slot, open, public, controller, createdAt}; read and deleted),
// `mods-room.json` out (Dolphin -> launcher: {version: 1, kind, state, error, room, session,
// input: {controller, index, device, plugged, real}, lastRequest: {id, result, error}, pid,
// updatedAt}; on every change and at least every 5 s). Nothing while no room was asked for.
void Files(const std::string& dir);

// Harness (`mods_status`, `mods_request`): the room as mm shows it, and the host's and member's
// requests: {"op": "leave"}, {"op": "kick", "slot": 1-4}, {"op": "slot", "slot", "open"},
// {"op": "public", "public"}.
picojson::object Status();
std::optional<std::string> Request(const picojson::object& args);
}  // namespace Online::ModsRoom
