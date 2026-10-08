// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/RecentCodes.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>

#include <picojson.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

namespace Online::RecentCodes
{
namespace
{
struct Entry
{
  std::string code;
  s64 last_played = 0;
};

std::mutex s_mutex;
// Loaded files by path, newest first.
std::map<std::string, std::vector<Entry>> s_cache;

std::string Normalize(const std::string& code)
{
  std::string out;
  for (size_t i = 0; i < code.size(); ++i)
  {
    const u8 c = static_cast<u8>(code[i]);
    // Full-width forms (U+FF01..FF5E, UTF-8 EF BC 81 .. EF BD 9E) -> ASCII.
    if (c == 0xEF && i + 2 < code.size())
    {
      const u32 cp = ((c & 0x0F) << 12) | ((code[i + 1] & 0x3F) << 6) | (code[i + 2] & 0x3F);
      if (cp >= 0xFF01 && cp <= 0xFF5E)
      {
        out.push_back(static_cast<char>(cp - 0xFF01 + 0x21));
        i += 2;
        continue;
      }
    }
    if (c >= 0x80 || c <= ' ')
      continue;
    out.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c));
  }
  return out;
}

std::string SafeUid(const std::string& uid)
{
  std::string out;
  for (const char c : uid)
  {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_')
    {
      out.push_back(c);
    }
  }
  return out;
}

void SortNewestFirst(std::vector<Entry>& entries)
{
  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry& a, const Entry& b) { return a.last_played > b.last_played; });
}

std::vector<Entry>& LoadLocked(const std::string& path)
{
  auto it = s_cache.find(path);
  if (it != s_cache.end())
    return it->second;
  std::vector<Entry> entries;
  std::string contents;
  if (File::ReadFileToString(path, contents))
  {
    picojson::value root;
    const std::string error = picojson::parse(root, contents);
    if (!error.empty() || !root.is<picojson::array>())
    {
      ERROR_LOG_FMT(NETPLAY, "Online: unable to parse {}: {}", path, error);
    }
    else
    {
      for (const auto& v : root.get<picojson::array>())
      {
        if (!v.is<picojson::object>())
          continue;
        const auto& o = v.get<picojson::object>();
        // Slippi writes connectCode/lastPlayed and still reads connect_code/last_played.
        auto field = [&o](const char* a, const char* b) -> const picojson::value* {
          if (auto f = o.find(a); f != o.end())
            return &f->second;
          if (auto f = o.find(b); f != o.end())
            return &f->second;
          return nullptr;
        };
        const picojson::value* code = field("connectCode", "connect_code");
        const picojson::value* time = field("lastPlayed", "last_played");
        if (!code || !code->is<std::string>())
          continue;
        Entry e;
        e.code = Normalize(code->get<std::string>());
        if (time && time->is<double>())
          e.last_played = static_cast<s64>(time->get<double>());
        if (!e.code.empty())
          entries.push_back(std::move(e));
      }
    }
  }
  SortNewestFirst(entries);
  return s_cache.emplace(path, std::move(entries)).first->second;
}

void WriteLocked(const std::string& path, const std::vector<Entry>& entries)
{
  picojson::array arr;
  for (const auto& e : entries)
  {
    picojson::object o;
    o["connectCode"] = picojson::value(e.code);
    o["lastPlayed"] = picojson::value(static_cast<double>(e.last_played));
    arr.emplace_back(std::move(o));
  }
  File::CreateFullPath(path);
  if (!File::WriteStringToFile(path, picojson::value(arr).serialize()))
    ERROR_LOG_FMT(NETPLAY, "Online: unable to write {}", path);
}

bool Matches(const std::string& code, const std::string& prefix)
{
  return code.size() >= prefix.size() && code.compare(0, prefix.size(), prefix) == 0;
}
}  // namespace

std::string PathFor(Kind kind, const std::string& uid)
{
  const std::string safe = SafeUid(uid);
  if (safe.empty())
    return {};
  return File::GetUserPath(D_ONLINE_IDX) + safe + DIR_SEP +
         (kind == Kind::Teams ? "teams-codes.json" : "direct-codes.json");
}

void Add(Kind kind, const std::string& uid, const std::string& code)
{
  const std::string path = PathFor(kind, uid);
  const std::string normalized = Normalize(code);
  if (path.empty() || normalized.empty())
    return;
  const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  std::lock_guard lk(s_mutex);
  auto& entries = LoadLocked(path);
  // Two searches within one second keep their order: the latest one becomes the newest.
  s64 stamp = now;
  if (!entries.empty())
  {
    const Entry& newest = entries.front();
    stamp = std::max(now, newest.last_played + (newest.code == normalized ? 0 : 1));
  }
  auto it = std::find_if(entries.begin(), entries.end(),
                         [&](const Entry& e) { return e.code == normalized; });
  if (it != entries.end())
    it->last_played = stamp;
  else
    entries.push_back(Entry{normalized, stamp});
  SortNewestFirst(entries);
  WriteLocked(path, entries);
  INFO_LOG_FMT(NETPLAY, "Online: recent {} code {} ({} in {})",
               kind == Kind::Teams ? "teams" : "direct", normalized, entries.size(), path);
}

std::vector<std::string> List(Kind kind, const std::string& uid)
{
  const std::string path = PathFor(kind, uid);
  if (path.empty())
    return {};
  std::lock_guard lk(s_mutex);
  std::vector<std::string> out;
  for (const auto& e : LoadLocked(path))
    out.push_back(e.code);
  return out;
}

void Clear(Kind kind, const std::string& uid)
{
  const std::string path = PathFor(kind, uid);
  if (path.empty())
    return;
  std::lock_guard lk(s_mutex);
  s_cache[path].clear();
  File::Delete(path);
}

Suggestion SuggestFrom(const std::vector<std::string>& codes, const std::string& raw_prefix,
                       u32 index, Scroll scroll)
{
  const std::string prefix = Normalize(raw_prefix);
  const s64 n = static_cast<s64>(codes.size());
  s64 i = index;
  switch (scroll)
  {
  case Scroll::Older:
    ++i;
    break;
  case Scroll::Newer:
    if (i > 0)
      --i;
    break;
  case Scroll::Reset:
    i = 0;
    break;
  case Scroll::None:
    break;
  }
  const s64 step = scroll == Scroll::Newer ? -1 : 1;
  while (i >= 0 && i < n && !Matches(codes[i], prefix))
    i += step;

  Suggestion s;
  s.index = index;
  if (i >= 0 && i < n)
  {
    s.found = true;
    s.index = static_cast<u32>(i);
    s.code = codes[i];
  }
  else if (static_cast<s64>(index) < n && Matches(codes[index], prefix))
  {
    // Ran off the list: keep the current suggestion (Slippi: "preserve that suggestion").
    s.found = true;
    s.code = codes[index];
  }
  return s;
}

Suggestion Suggest(Kind kind, const std::string& uid, const std::string& prefix, u32 index,
                   Scroll scroll)
{
  return SuggestFrom(List(kind, uid), prefix, index, scroll);
}
}  // namespace Online::RecentCodes
