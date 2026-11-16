// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Spectator chat in the launcher's playback Dolphin (docs/design/spectating.md §6.4,
// docs/spectate-protocol.md §4.1): everyone watching a live game chats together; the players of
// that game never see it, and the spectators never see the players' chat. It is not end-to-end
// encrypted (accounts reads it), so the window has no padlock; it shows an eye with how many
// watch, and hovering it lists them.
//
// The playback Dolphin has no account and no internet connection: the launcher holds the watch
// stream and writes the chat to <comm.json>.chat.json, which this follows (ReplayPlayback's loop,
// every 200 ms):
//   {"stream": "<12 hex>", "you": "<uid>", "over": false, "count": 3,
//    "spectators": [{"uid", "name", "code"}, ...],
//    "lines": [{"id": 7, "uid", "name", "code", "text"}, {"id": 8, "notice": "..."}, ...],
//    "hidden": ["<uid>", ...], "done": {"run": "<hex>", "seq": 4}}
// No file, or no "stream": no spectator chat (the window shows the players' chat as before, which
// a playback Dolphin never has). `id`s are the launcher's, increasing. What the spectator does goes
// back in the playback status (<comm.json>.status.json) as "chat":
//   {"run": "<hex, one per Dolphin run>",
//    "out": [{"seq": 1, "stream", "send": "<text>"}, {"seq": 2, "stream", "hide": "<uid>",
//             "hidden": true}, {"seq": 3, "stream", "report": <line id>}]}
// and the launcher says in "done" which it has taken (sent, or refused with a notice line). A line
// typed shows at once, until the launcher has taken it (the file then has it as the server sent
// it).
//
// Online::Chat's window functions (GetView, Send, SetHidden, IsHidden, Report) come here while a
// spectator chat is active. Everything in the file is checked and cleaned as peer text.
//
// All functions are thread-safe.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Online/Chat.h"

namespace Online::SpectatorChat
{
constexpr size_t MAX_FILE_BYTES = 256 * 1024;
constexpr size_t MAX_SPECTATORS_LISTED = 100;
constexpr size_t MAX_OUT = 16;

// Reads `path` again when its time or size changed (the playback loop). An empty path forgets
// the spectator chat.
void Follow(const std::string& path);
// Takes the file's contents (Follow, and the unit tests). A file that doesn't parse changes
// nothing.
void Apply(std::string_view text);
// A spectator chat is shown (the file names a stream).
bool Active();

std::optional<Chat::View> GetView(u64 known_serial);
Chat::SendResult Send(const std::string& text);
void SetHidden(const std::string& uid, bool hidden);
bool IsHidden(const std::string& uid);
bool Report(const std::string& uid);

// The status's "chat" (null while nothing was ever done).
picojson::value Outbox();
// For the harness (`chat_status`'s "spectate").
picojson::object Status();
void ResetForTesting();
}  // namespace Online::SpectatorChat
