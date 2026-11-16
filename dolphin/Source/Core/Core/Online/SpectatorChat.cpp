// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/SpectatorChat.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Core/Online/ChatText.h"
#include "Core/Rollback/PeerData.h"

namespace Online::SpectatorChat
{
namespace
{
using Clock = std::chrono::steady_clock;
using Chat::Line;

constexpr size_t MAX_NAME_BYTES = 64;
constexpr size_t MAX_CODE_BYTES = 32;
constexpr size_t MAX_NOTICE_CODE_POINTS = 200;
constexpr size_t MAX_HIDDEN = Chat::MAX_HIDDEN;
// The local send limit, as accounts' (docs/spectate-protocol.md §3: 5 in 5 s, 30 a minute).
constexpr size_t SEND_BURST = 5;
constexpr auto SEND_BURST_WINDOW = std::chrono::seconds(5);
constexpr size_t SEND_PER_MINUTE = 30;
// Spectator serials start far above Online::Chat's, so the window always sees a change when one
// takes over from the other.
constexpr u64 SERIAL_BASE = u64{1} << 48;

struct FileLine
{
  u64 id = 0;  // the launcher's
  bool notice = false;
  std::string uid, name, code, text;
};

struct Spectator
{
  std::string uid, name, code;
};

struct OutItem
{
  u64 seq = 0;
  std::string stream;
  enum class Kind : u8
  {
    Send,
    Hide,
    Report,
  } kind = Kind::Send;
  std::string text;  // Send: the cleaned text; Hide: the uid
  bool hidden = false;
  u64 line = 0;  // Report: the launcher's line id
  u64 local_id = 0;  // Send: its line, shown until the launcher has taken it
};

struct State
{
  std::string path;
  bool stamped = false;
  std::filesystem::file_time_type mtime{};
  u64 size = 0;

  // From the file.
  std::string stream;
  std::string you;
  bool over = false;
  u32 count = 0;
  std::vector<Spectator> spectators;
  std::vector<FileLine> lines;
  std::set<std::string> hidden;

  // The launcher's line ids as this run numbers them (Line::id: unique, increasing).
  std::map<u64, u64> local_ids;
  u64 next_local = 1;

  std::string run;
  u64 next_seq = 1;
  u64 done = 0;
  std::deque<OutItem> out;
  std::deque<Clock::time_point> sent_times;

  u64 serial = SERIAL_BASE;
  u64 last_message_ms = 0;
  u64 files = 0, bad_files = 0;
};

std::mutex s_mutex;
State s;

void Touch()
{
  ++s.serial;
}

bool IsUid(std::string_view v)
{
  if (v.size() != 36)
    return false;
  for (size_t i = 0; i < v.size(); ++i)
  {
    const char c = v[i];
    if (i == 8 || i == 13 || i == 18 || i == 23)
    {
      if (c != '-')
        return false;
    }
    else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
    {
      return false;
    }
  }
  return true;
}

bool IsStream(std::string_view v)
{
  const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
  return v.size() == 12 && std::ranges::all_of(v, hex);
}

const picojson::value* Get(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}

std::string Str(const picojson::object& o, const char* key)
{
  const picojson::value* v = Get(o, key);
  return v && v->is<std::string>() ? v->get<std::string>() : std::string();
}

std::optional<u64> Id(const picojson::object& o, const char* key)
{
  const picojson::value* v = Get(o, key);
  if (!v || !v->is<double>())
    return std::nullopt;
  const double d = v->get<double>();
  if (!(d >= 1 && d <= 9007199254740991.0))
    return std::nullopt;
  return static_cast<u64>(d);
}

std::string CleanName(const std::string& text, size_t max_bytes, const char* fallback)
{
  return ChatText::Clean(text, ChatText::MAX_NAME_CODE_POINTS, max_bytes).value_or(fallback);
}

// A member's colour (0-3): the same for a uid every time (the window has no joining order here).
int ColorOf(std::string_view uid)
{
  u32 h = 2166136261u;
  for (const char c : uid)
    h = (h ^ static_cast<u8>(c)) * 16777619u;
  return static_cast<int>(h % Chat::MAX_MEMBERS);
}

u64 LocalId(u64 launcher_id)
{
  const auto [it, added] = s.local_ids.try_emplace(launcher_id, s.next_local);
  if (added)
    ++s.next_local;
  return it->second;
}

std::string RunId()
{
  if (s.run.empty())
  {
    std::random_device rd;
    s.run = fmt::format("{:08x}{:08x}", rd(), rd());
  }
  return s.run;
}

void QueueOut(OutItem item)
{
  RunId();
  item.seq = s.next_seq++;
  item.stream = s.stream;
  s.out.push_back(std::move(item));
  while (s.out.size() > MAX_OUT)
    s.out.pop_front();
}

void Forget()
{
  if (s.stream.empty() && s.lines.empty())
    return;
  s.stream.clear();
  s.you.clear();
  s.over = false;
  s.count = 0;
  s.spectators.clear();
  s.lines.clear();
  s.local_ids.clear();
  Touch();
}

void ApplyLocked(std::string_view text)
{
  picojson::value root;
  if (!Gprb::PeerData::JsonSafeToParse(text, 6) ||
      !picojson::parse(root, std::string(text)).empty() || !root.is<picojson::object>())
  {
    ++s.bad_files;
    return;
  }
  ++s.files;
  const auto& o = root.get<picojson::object>();
  const std::string stream = Str(o, "stream");
  if (!IsStream(stream))
  {
    Forget();
    return;
  }
  if (stream != s.stream)
  {
    // Another game: its own chat (lines typed for the last one are not shown here).
    Forget();
    s.stream = stream;
  }
  const std::string you = Str(o, "you");
  s.you = IsUid(you) ? you : std::string();
  const picojson::value* over = Get(o, "over");
  s.over = over && over->is<bool>() && over->get<bool>();
  const picojson::value* count = Get(o, "count");
  s.count = count && count->is<double>() ?
                static_cast<u32>(std::clamp(count->get<double>(), 0.0, 1e6)) :
                0;

  s.spectators.clear();
  if (const picojson::value* list = Get(o, "spectators"); list && list->is<picojson::array>())
  {
    std::set<std::string> seen;
    for (const picojson::value& v : list->get<picojson::array>())
    {
      if (s.spectators.size() >= MAX_SPECTATORS_LISTED || !v.is<picojson::object>())
        continue;
      const auto& so = v.get<picojson::object>();
      Spectator sp{Str(so, "uid"), CleanName(Str(so, "name"), MAX_NAME_BYTES, "?"),
                   CleanName(Str(so, "code"), MAX_CODE_BYTES, "")};
      if (!IsUid(sp.uid) || !seen.insert(sp.uid).second)
        continue;
      s.spectators.push_back(std::move(sp));
    }
  }
  s.count = std::max<u32>(s.count, static_cast<u32>(s.spectators.size()));

  std::vector<FileLine> lines;
  if (const picojson::value* list = Get(o, "lines"); list && list->is<picojson::array>())
  {
    u64 last = 0;
    for (const picojson::value& v : list->get<picojson::array>())
    {
      if (!v.is<picojson::object>())
        continue;
      const auto& lo = v.get<picojson::object>();
      const auto id = Id(lo, "id");
      // Oldest first, each once.
      if (!id || *id <= last)
        continue;
      FileLine l;
      l.id = *id;
      if (const std::string notice = Str(lo, "notice"); !notice.empty())
      {
        auto clean = ChatText::Clean(notice, MAX_NOTICE_CODE_POINTS, MAX_NOTICE_CODE_POINTS * 4);
        if (!clean)
          continue;
        l.notice = true;
        l.text = std::move(*clean);
      }
      else
      {
        l.uid = Str(lo, "uid");
        auto clean = ChatText::Clean(Str(lo, "text"));
        if (!IsUid(l.uid) || !clean)
          continue;
        l.text = std::move(*clean);
        l.name = CleanName(Str(lo, "name"), MAX_NAME_BYTES, "?");
        l.code = CleanName(Str(lo, "code"), MAX_CODE_BYTES, "");
      }
      last = l.id;
      lines.push_back(std::move(l));
    }
  }
  while (lines.size() > Chat::MAX_LINES)
    lines.erase(lines.begin());
  if (!lines.empty() && (s.lines.empty() || lines.back().id != s.lines.back().id) &&
      !lines.back().notice)
  {
    s.last_message_ms = Common::Timer::NowMs();
  }
  s.lines = std::move(lines);
  // (Only the lines shown keep their numbers.)
  std::erase_if(s.local_ids, [](const auto& kv) {
    return std::ranges::none_of(s.lines, [&](const FileLine& l) { return l.id == kv.first; });
  });
  for (const FileLine& l : s.lines)
    LocalId(l.id);

  std::set<std::string> hidden;
  if (const picojson::value* list = Get(o, "hidden"); list && list->is<picojson::array>())
  {
    for (const picojson::value& v : list->get<picojson::array>())
    {
      if (hidden.size() < MAX_HIDDEN && v.is<std::string>() && IsUid(v.get<std::string>()))
        hidden.insert(v.get<std::string>());
    }
  }

  if (const picojson::value* done = Get(o, "done"); done && done->is<picojson::object>())
  {
    const auto& d = done->get<picojson::object>();
    if (!s.run.empty() && Str(d, "run") == s.run)
    {
      if (const auto seq = Id(d, "seq"))
        s.done = std::max(s.done, *seq);
    }
  }
  std::erase_if(s.out, [](const OutItem& i) { return i.seq <= s.done; });
  // An ignore not taken yet stays as it was clicked.
  for (const OutItem& i : s.out)
  {
    if (i.kind != OutItem::Kind::Hide)
      continue;
    if (i.hidden)
      hidden.insert(i.text);
    else
      hidden.erase(i.text);
  }
  s.hidden = std::move(hidden);
  Touch();
}
}  // namespace

void Follow(const std::string& path)
{
  std::lock_guard lk(s_mutex);
  if (path != s.path)
  {
    s.path = path;
    s.stamped = false;
    if (path.empty())
      Forget();
  }
  if (s.path.empty())
    return;
  std::error_code error;
  const std::filesystem::path p = StringToPath(s.path);
  const auto mtime = std::filesystem::last_write_time(p, error);
  const u64 size = error ? 0 : std::filesystem::file_size(p, error);
  if (error)
  {
    // No file (yet, or the launcher removed it): no spectator chat.
    if (s.stamped)
    {
      s.stamped = false;
      Forget();
    }
    return;
  }
  if (s.stamped && mtime == s.mtime && size == s.size)
    return;
  if (size == 0 || size > MAX_FILE_BYTES)
    return;
  std::string text;
  if (!File::ReadFileToString(s.path, text) || text.size() > MAX_FILE_BYTES)
    return;  // the launcher is renaming it in place: read again next time
  s.stamped = true;
  s.mtime = mtime;
  s.size = size;
  ApplyLocked(text);
}

void Apply(std::string_view text)
{
  std::lock_guard lk(s_mutex);
  ApplyLocked(text);
}

bool Active()
{
  std::lock_guard lk(s_mutex);
  return !s.stream.empty();
}

std::optional<Chat::View> GetView(u64 known_serial)
{
  std::lock_guard lk(s_mutex);
  if (known_serial == s.serial)
    return std::nullopt;
  Chat::View v;
  v.serial = s.serial;
  v.active = !s.stream.empty();
  v.spectate = true;
  v.spectators = s.count;
  v.title = "Spectators";
  v.last_message_ms = s.last_message_ms;
  // (Another number for each stream: the window's per-group state starts again.)
  v.group_number =
      s.stream.empty() ? 0 : SERIAL_BASE + std::hash<std::string>{}(s.stream) % SERIAL_BASE;

  const auto member = [&](const std::string& uid, const std::string& name,
                          const std::string& code) {
    Chat::Member m;
    m.uid = uid;
    m.name = name;
    m.code = code;
    m.is_me = uid == s.you;
    m.has_key = true;
    m.hidden = s.hidden.contains(uid);
    m.color = ColorOf(uid);
    return m;
  };
  std::set<std::string> listed;
  for (const Spectator& sp : s.spectators)
  {
    v.watching.push_back(member(sp.uid, sp.name, sp.code));
    v.members.push_back(v.watching.back());
    listed.insert(sp.uid);
  }
  // Whoever wrote a line can be clicked (ignore, report), watching still or not.
  for (const FileLine& l : s.lines)
  {
    if (!l.notice && listed.insert(l.uid).second)
      v.members.push_back(member(l.uid, l.name, l.code));
  }

  for (const FileLine& l : s.lines)
  {
    Line out;
    out.id = s.local_ids.contains(l.id) ? s.local_ids.at(l.id) : 0;
    if (l.notice)
    {
      out.kind = Line::Kind::System;
    }
    else
    {
      out.kind = l.uid == s.you ? Line::Kind::Own : Line::Kind::Message;
      out.uid = l.uid;
      out.name = l.name;
      out.color = ColorOf(l.uid);
    }
    out.text = l.text;
    v.lines.push_back(std::move(out));
  }
  // What was typed and the launcher hasn't taken yet.
  std::string my_name = "You";
  for (const Spectator& sp : s.spectators)
  {
    if (sp.uid == s.you)
      my_name = sp.name;
  }
  for (const OutItem& i : s.out)
  {
    if (i.kind != OutItem::Kind::Send || i.stream != s.stream)
      continue;
    Line out;
    out.id = i.local_id;
    out.kind = Line::Kind::Own;
    out.uid = s.you;
    out.name = my_name;
    out.color = ColorOf(s.you);
    out.text = i.text;
    v.lines.push_back(std::move(out));
  }
  while (v.lines.size() > Chat::MAX_LINES)
    v.lines.erase(v.lines.begin());
  return v;
}

Chat::SendResult Send(const std::string& text)
{
  auto cleaned = ChatText::Clean(text);
  if (!cleaned)
    return Chat::SendResult::Empty;
  const auto now = Clock::now();
  std::lock_guard lk(s_mutex);
  if (s.stream.empty() || s.over)
    return Chat::SendResult::NoGroup;
  while (!s.sent_times.empty() && now - s.sent_times.front() > std::chrono::minutes(1))
    s.sent_times.pop_front();
  const size_t recent = static_cast<size_t>(std::ranges::count_if(
      s.sent_times, [&](Clock::time_point t) { return now - t < SEND_BURST_WINDOW; }));
  const size_t waiting = static_cast<size_t>(
      std::ranges::count_if(s.out, [](const OutItem& i) { return i.kind == OutItem::Kind::Send; }));
  if (recent >= SEND_BURST || s.sent_times.size() >= SEND_PER_MINUTE || waiting >= SEND_BURST)
    return Chat::SendResult::TooFast;
  s.sent_times.push_back(now);
  OutItem item;
  item.kind = OutItem::Kind::Send;
  item.text = std::move(*cleaned);
  item.local_id = s.next_local++;
  QueueOut(std::move(item));
  s.last_message_ms = Common::Timer::NowMs();
  Touch();
  return Chat::SendResult::Sent;
}

void SetHidden(const std::string& uid, bool hidden)
{
  if (!IsUid(uid))
    return;
  std::lock_guard lk(s_mutex);
  if (s.stream.empty() || hidden == s.hidden.contains(uid))
    return;
  if (hidden)
    s.hidden.insert(uid);
  else
    s.hidden.erase(uid);
  OutItem item;
  item.kind = OutItem::Kind::Hide;
  item.text = uid;
  item.hidden = hidden;
  QueueOut(std::move(item));
  Touch();
}

bool IsHidden(const std::string& uid)
{
  std::lock_guard lk(s_mutex);
  return s.hidden.contains(uid);
}

bool Report(const std::string& uid)
{
  std::lock_guard lk(s_mutex);
  if (s.stream.empty() || uid == s.you)
    return false;
  // accounts keeps the line and the 20 before it: the newest of theirs is enough.
  const auto it = std::find_if(s.lines.rbegin(), s.lines.rend(),
                               [&](const FileLine& l) { return !l.notice && l.uid == uid; });
  if (it == s.lines.rend())
    return false;
  OutItem item;
  item.kind = OutItem::Kind::Report;
  item.line = it->id;
  QueueOut(std::move(item));
  NOTICE_LOG_FMT(BRAWLBACK, "spectator chat: reported {} (line {})", uid, it->id);
  return true;
}

picojson::value Outbox()
{
  std::lock_guard lk(s_mutex);
  if (s.run.empty())
    return {};
  picojson::object o;
  o["run"] = picojson::value(s.run);
  picojson::array out;
  for (const OutItem& i : s.out)
  {
    picojson::object io;
    io["seq"] = picojson::value(static_cast<double>(i.seq));
    io["stream"] = picojson::value(i.stream);
    switch (i.kind)
    {
    case OutItem::Kind::Send:
      io["send"] = picojson::value(i.text);
      break;
    case OutItem::Kind::Hide:
      io["hide"] = picojson::value(i.text);
      io["hidden"] = picojson::value(i.hidden);
      break;
    case OutItem::Kind::Report:
      io["report"] = picojson::value(static_cast<double>(i.line));
      break;
    }
    out.emplace_back(std::move(io));
  }
  o["out"] = picojson::value(std::move(out));
  return picojson::value(std::move(o));
}

picojson::object Status()
{
  std::lock_guard lk(s_mutex);
  picojson::object o;
  o["active"] = picojson::value(!s.stream.empty());
  o["stream"] = picojson::value(s.stream);
  o["you"] = picojson::value(s.you);
  o["over"] = picojson::value(s.over);
  o["count"] = picojson::value(static_cast<double>(s.count));
  picojson::array spectators;
  for (const Spectator& sp : s.spectators)
    spectators.emplace_back(sp.name);
  o["spectators"] = picojson::value(std::move(spectators));
  picojson::array lines;
  for (const FileLine& l : s.lines)
  {
    picojson::object lo;
    lo["id"] = picojson::value(static_cast<double>(l.id));
    lo["uid"] = picojson::value(l.uid);
    lo["name"] = picojson::value(l.name);
    lo["text"] = picojson::value(l.text);
    lo["notice"] = picojson::value(l.notice);
    lines.emplace_back(std::move(lo));
  }
  o["lines"] = picojson::value(std::move(lines));
  o["waiting"] = picojson::value(static_cast<double>(s.out.size()));
  o["files"] = picojson::value(static_cast<double>(s.files));
  o["bad_files"] = picojson::value(static_cast<double>(s.bad_files));
  return o;
}

void ResetForTesting()
{
  std::lock_guard lk(s_mutex);
  s = State{};
}
}  // namespace Online::SpectatorChat
