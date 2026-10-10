// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Chat with the other players of a room or a match (docs/chat-protocol.md, docs/design/chat.md).
// mm relays end-to-end encrypted boxes over the online connection that Online/Rooms.cpp owns:
// the Rooms thread hands every chat-* message to OnServerMessage and sends what TakeOutbox
// returns, so decryption, signing and every check of what other players send run on that thread.
// The window (VideoCommon/OnlineChatUI.cpp) reads GetView and calls Send, SetHidden and Report.
//
// Everything that arrives is hostile until checked: the server's JSON (size, depth, types), the
// members (uids, keys, signatures), each box (length, authentication, version, sequence number,
// signature) and finally the text (ChatText::Clean).
//
// All functions are thread-safe.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Online::Chat
{
constexpr size_t MAX_LINES = 100;
constexpr size_t MAX_MEMBERS = 4;
constexpr size_t MAX_HIDDEN = 500;
constexpr size_t MAX_REPORTED_MESSAGES = 20;
// Per sender, on top of mm's limit (5 in 5 s): what arrives faster is dropped.
constexpr int RECEIVE_BURST = 8;
constexpr double RECEIVE_PER_SECOND = 1.5;

// ---- The online connection (Rooms thread) ----

// The identity public key for `hello` (64 hex digits), made and stored on first use in
// <User>/Online/chat-identity.key. Empty if the key can't be read or written (no chat then).
std::string IdentityPublicKeyHex();
// A message from mm. Returns false if it is not a chat message (the caller handles it).
bool OnServerMessage(const std::string& packet);
// JSON messages to send now (group keys, encrypted messages, leaves, reports). Called every
// tick; it also notices when a match's session has connected (the group is shown from then).
std::vector<std::string> TakeOutbox();
// The connection dropped or was closed: the group is gone (mm sends it again after a reconnect).
void OnConnectionLost();

// ---- The online session (OnlineClient) ----

// The match's online session ended (Client::Cleanup): leave its chat group, if in one.
void LeaveMatchGroup();

// ---- The window ----

struct Member
{
  std::string uid;
  std::string name;  // cleaned
  std::string code;  // cleaned
  bool is_me = false;
  bool has_key = false;  // can read what is sent (its group key checked out)
  bool hidden = false;
  int color = 0;  // 0-3, by joining order
};

struct Line
{
  enum class Kind : u8
  {
    Message,  // from another member
    Own,      // sent by this player
    System,   // joins, leaves, errors
  };
  u64 id = 0;  // unique in this Dolphin run
  Kind kind = Kind::System;
  std::string uid;   // sender (Message, Own)
  std::string name;  // sender's name when it arrived
  std::string text;  // cleaned
  int color = 0;
};

struct View
{
  // In a room's group, or a match's group whose session is connected.
  bool active = false;
  bool room = false;  // a room's group (else a match's)
  std::string title;  // "Room KFQB" / the other player's name and code
  std::vector<Member> members;
  std::vector<Line> lines;  // oldest first, at most MAX_LINES
  u64 serial = 0;           // changes whenever anything above changes
  u64 last_message_ms = 0;  // Common::Timer::NowMs() of the newest Message or Own line
};
// The view, or nullopt if `known_serial` is still current (nothing to copy).
std::optional<View> GetView(u64 known_serial);

enum class SendResult
{
  Sent,
  Empty,      // nothing left after cleaning
  NoGroup,    // not in a room or match
  NoReaders,  // nobody else's group key is known yet
  TooFast,    // over the local limit (mm's: 5 in 5 s)
};
SendResult Send(const std::string& text);

void SetHidden(const std::string& uid, bool hidden);
bool IsHidden(const std::string& uid);
// Reports this member's recent messages (at most MAX_REPORTED_MESSAGES) to mm.
bool Report(const std::string& uid, const std::string& reason);

// ---- Unit tests (UnitTests/Core/Online/ChatTest.cpp) ----
// Forgets everything (the identity too, which is read again from the User folder) and takes
// `local_uid` as this player's uid instead of the logged-in user's.
void ResetForTesting(const std::string& local_uid);

// ---- Harness (`chat_status`, `chat_send`, `chat_hide`, `chat_report`, `chat_leave`) ----
picojson::object Status();
std::optional<std::string> HarnessRequest(const std::string& command,
                                          const picojson::object& args);
}  // namespace Online::Chat
