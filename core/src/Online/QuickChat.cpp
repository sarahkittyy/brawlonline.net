// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "BrawlOnline/Online/QuickChat.h"

#include <deque>
#include <filesystem>
#include <mutex>
#include <system_error>

#include <picojson.h>

#include "BrawlOnline/Online/Chat.h"
#include "BrawlOnline/Online/ChatText.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "InputCommon/GCPadStatus.h"

namespace Online::QuickChat
{
namespace
{
constexpr const char* FILE_NAME = "quick-chat.json";
// 16 messages of 300 bytes, JSON-escaped (a \uXXXX is 6 bytes for at most 3 of UTF-8).
constexpr u64 MAX_FILE = 64 * 1024;
constexpr size_t MAX_PRESSES = 8;
constexpr u16 DPAD_BITS = PAD_BUTTON_UP | PAD_BUTTON_RIGHT | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT;
constexpr int CANCEL = -1;

struct FileStamp
{
  bool exists = false;
  std::filesystem::file_time_type mtime{};
  std::uintmax_t size = 0;
  bool operator==(const FileStamp&) const = default;
};

struct State
{
  Messages messages;
  bool any = false;
  bool loaded = false;
  FileStamp stamp;

  bool on_css = false;
  bool shown = false;
  u64 shown_ms = 0;

  // Per GameCube port: the D-pad and B as last read, and B kept from the game until let go (it
  // was held while a group was open: the game would take its release from the menu as a press).
  std::array<u16, 4> prev{};
  std::array<bool, 4> hold_b{};
  std::deque<int> presses;

  View view;
  Result (*send)(const std::string&) = nullptr;
};

std::mutex s_mutex;
State s;

FileStamp StampOf(const std::string& path)
{
  std::error_code error;
  const std::filesystem::path p = StringToPath(path);
  FileStamp stamp;
  const auto mtime = std::filesystem::last_write_time(p, error);
  if (error)
    return stamp;
  const auto size = std::filesystem::file_size(p, error);
  if (error)
    return stamp;
  stamp.exists = true;
  stamp.mtime = mtime;
  stamp.size = size;
  return stamp;
}

bool AnyIn(const Messages& m)
{
  for (const std::string& text : m)
  {
    if (!text.empty())
      return true;
  }
  return false;
}

bool GroupHasAny(const Messages& m, int group)
{
  for (int d = 0; d < DIRECTIONS; ++d)
  {
    if (!m[4 * group + d].empty())
      return true;
  }
  return false;
}

bool UsableLocked(u64 now_ms)
{
  return s.any && s.on_css && s.shown && now_ms - s.shown_ms <= SHOWN_STALE_MS;
}

void SetMessagesLocked(const Messages& messages)
{
  s.messages = messages;
  s.any = AnyIn(messages);
  s.view.messages = messages;
}

Result ChatSend(const std::string& text)
{
  switch (Online::Chat::Send(text))
  {
  case Online::Chat::SendResult::Sent:
    return Result::Sent;
  case Online::Chat::SendResult::Empty:
    return Result::None;
  case Online::Chat::SendResult::NoGroup:
    return Result::NoGroup;
  case Online::Chat::SendResult::NoReaders:
    return Result::NoReaders;
  case Online::Chat::SendResult::TooFast:
    return Result::TooFast;
  }
  return Result::None;
}
}  // namespace

std::optional<Messages> Parse(std::string_view json)
{
  if (json.size() > MAX_FILE)
    return std::nullopt;
  picojson::value v;
  const std::string text(json);
  if (!picojson::parse(v, text).empty() || !v.is<picojson::object>())
    return std::nullopt;
  const auto& o = v.get<picojson::object>();
  const auto version = o.find("version");
  const auto list = o.find("messages");
  if (version == o.end() || !version->second.is<double>() || version->second.get<double>() != 1 ||
      list == o.end() || !list->second.is<picojson::array>())
  {
    return std::nullopt;
  }
  const auto& a = list->second.get<picojson::array>();
  if (a.size() != MESSAGES)
    return std::nullopt;
  Messages out;
  for (size_t i = 0; i < MESSAGES; ++i)
  {
    if (!a[i].is<std::string>())
      return std::nullopt;
    out[i] = ChatText::Clean(a[i].get<std::string>()).value_or("");
  }
  return out;
}

void PollFile(const std::string& online_dir)
{
  const std::string path = online_dir + FILE_NAME;
  const FileStamp stamp = StampOf(path);
  {
    std::lock_guard lk(s_mutex);
    if (s.loaded && stamp == s.stamp)
      return;
  }
  std::optional<Messages> messages;
  if (!stamp.exists)
  {
    messages = Messages{};
  }
  else
  {
    std::string text;
    if (stamp.size <= MAX_FILE && File::ReadFileToString(path, text))
      messages = Parse(text);
  }
  std::lock_guard lk(s_mutex);
  s.loaded = true;
  if (!messages)
  {
    if (stamp != s.stamp)
      WARN_LOG_FMT(BRAWLBACK, "quick-chat.json unreadable or not version 1 with 16 messages: kept");
    s.stamp = stamp;
    return;
  }
  s.stamp = stamp;
  SetMessagesLocked(*messages);
  size_t count = 0;
  for (const std::string& m : *messages)
    count += m.empty() ? 0 : 1;
  INFO_LOG_FMT(BRAWLBACK, "quick chat: {} messages", count);
}

void SetMessages(const Messages& messages)
{
  std::lock_guard lk(s_mutex);
  SetMessagesLocked(messages);
}

Messages GetMessages()
{
  std::lock_guard lk(s_mutex);
  return s.messages;
}

void SetOnCss(bool on_css)
{
  std::lock_guard lk(s_mutex);
  s.on_css = on_css;
}

void SetChatShown(bool shown, u64 now_ms)
{
  std::lock_guard lk(s_mutex);
  s.shown = shown;
  s.shown_ms = now_ms;
}

void FilterPad(int chan, u16* buttons, u64 now_ms)
{
  if (chan < 0 || chan >= 4 || !buttons)
    return;
  std::lock_guard lk(s_mutex);
  const u16 now_bits = *buttons & (DPAD_BITS | PAD_BUTTON_B);
  const u16 pressed = now_bits & ~s.prev[chan];
  s.prev[chan] = now_bits;
  const bool open = s.view.state == View::State::Open;
  if (open && (*buttons & PAD_BUTTON_B))
    s.hold_b[chan] = true;
  else if (!(*buttons & PAD_BUTTON_B))
    s.hold_b[chan] = false;
  if (s.hold_b[chan])
    *buttons &= ~PAD_BUTTON_B;
  if (!UsableLocked(now_ms))
    return;
  *buttons &= ~DPAD_BITS;
  const auto push = [](int p) {
    if (s.presses.size() < MAX_PRESSES)
      s.presses.push_back(p);
  };
  if (pressed & PAD_BUTTON_UP)
    push(Up);
  if (pressed & PAD_BUTTON_RIGHT)
    push(Right);
  if (pressed & PAD_BUTTON_DOWN)
    push(Down);
  if (pressed & PAD_BUTTON_LEFT)
    push(Left);
  if (open && (pressed & PAD_BUTTON_B))
    push(CANCEL);
}

View Tick(u64 now_ms, Result* result)
{
  std::unique_lock lk(s_mutex);
  View& v = s.view;
  if (v.state == View::State::Open && now_ms - v.last_press_ms >= OPEN_MS)
    v.state = View::State::Closed;
  if (!UsableLocked(now_ms))
  {
    s.presses.clear();
    if (v.state == View::State::Open)
      v.state = View::State::Closed;
  }
  while (!s.presses.empty())
  {
    const int p = s.presses.front();
    s.presses.pop_front();
    if (p == CANCEL)
    {
      if (v.state == View::State::Open)
        v.state = View::State::Closed;
      continue;
    }
    if (v.state != View::State::Open)
    {
      // A group with nothing in it doesn't open.
      if (!GroupHasAny(s.messages, p))
        continue;
      v.state = View::State::Open;
      v.group = p;
      v.since_ms = now_ms;
      v.last_press_ms = now_ms;
      continue;
    }
    v.last_press_ms = now_ms;
    const std::string text = s.messages[4 * v.group + p];
    // An unset slot (the menu's dash) closes the menu, as B does.
    if (text.empty())
    {
      v.state = View::State::Closed;
      continue;
    }
    // Sent without the lock: the pad's reads (FilterPad) don't wait for the encryption.
    const auto send = s.send;
    lk.unlock();
    const Result r = send ? send(text) : ChatSend(text);
    lk.lock();
    if (result)
      *result = r;
    // Sent: the menu goes away (the message is in the chat window). Refused: it stays open, with
    // why under it.
    if (r == Result::Sent)
      v.state = View::State::Closed;
  }
  return v;
}

void ResetForTesting(Result (*send)(const std::string& text))
{
  std::lock_guard lk(s_mutex);
  s = State{};
  s.send = send;
}

void PressForTesting(int direction)
{
  std::lock_guard lk(s_mutex);
  s.presses.push_back(direction);
}

bool Usable(u64 now_ms)
{
  std::lock_guard lk(s_mutex);
  return UsableLocked(now_ms);
}
}  // namespace Online::QuickChat
