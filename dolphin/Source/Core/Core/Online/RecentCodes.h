// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Recent connect codes: the history behind the connect-code keypad's suggestions (Slippi's
// FETCH_CODE_SUGGESTION, 0xBE).
//
// Slippi keeps two lists, direct-codes.json and teams-codes.json, in its user folder (the one
// with user.json: slippi-rust-extensions user/src/lib.rs, UserManager::new). Each file is a JSON
// array of {"connectCode", "lastPlayed" (unix seconds)}; a code is added, or its time refreshed,
// when a Direct/Teams search starts (EXI_DeviceSlippi.cpp startFindMatch); the list is read
// newest first and has no size limit (user/src/direct_codes/mod.rs).
//
// We keep the same files and format, one set per account: <User>/Online/<uid>/direct-codes.json
// and teams-codes.json. (Slippi's folder is shared by every account that logs in on that
// install; ours keeps one account's opponents from showing up for another.) Codes are stored as
// upper-case ASCII ("ABCD#123"); Slippi stores the full-width text it got from the game.
//
// All functions are thread-safe.

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Online::RecentCodes
{
enum class Kind : u8
{
  Direct,
  Teams,
};

// Slippi's scroll constants (AutoComplete.s): what the request asks for.
enum class Scroll : u8
{
  None = 0,   // the suggestion at `index` if it still matches, else the next older match
  Older = 1,  // L: the next older match after `index`
  Newer = 2,  // R: the next newer match before `index`
  Reset = 3,  // the newest match (keypad opened, a character typed or deleted)
};

// <User>/Online/<uid>/{direct,teams}-codes.json; "" if the uid is empty.
std::string PathFor(Kind kind, const std::string& uid);

// Adds `code` (normalised to upper-case ASCII) or refreshes its time. No-op without a uid or code.
void Add(Kind kind, const std::string& uid, const std::string& code);

// The codes, newest first.
std::vector<std::string> List(Kind kind, const std::string& uid);

// Deletes the history (harness/tests).
void Clear(Kind kind, const std::string& uid);

struct Suggestion
{
  bool found = false;
  u32 index = 0;     // the suggestion's position in List() (newest = 0); the input index if none
  std::string code;  // the whole suggested code (upper-case ASCII)
};

// Slippi's handleNameEntryLoad: the newest/older/newer code that starts with `prefix`
// (case-insensitive, ASCII), starting from `index`. When the scroll runs off the list the
// suggestion at `index` stays, if it still matches.
Suggestion Suggest(Kind kind, const std::string& uid, const std::string& prefix, u32 index,
                   Scroll scroll);

// The same search over a given list (newest first); for tests.
Suggestion SuggestFrom(const std::vector<std::string>& codes, const std::string& prefix, u32 index,
                       Scroll scroll);
}  // namespace Online::RecentCodes
