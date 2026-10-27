// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/SpecialAssets.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>

#include <fmt/format.h>

#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/Rollback/MachineSDCard.h"

namespace Online::SpecialAssets
{
namespace
{
// P+'s main-menu archive on its card, then where an older card layout kept it.
constexpr std::array<std::string_view, 2> ARCHIVE_PATHS = {"Project+/pf/menu2/mu_menumain.pac",
                                                            "pf/menu2/mu_menumain.pac"};

std::mutex s_mutex;
State s_state = State::None;
// When the last build failed: a request a few seconds later tries again (the launcher may still
// be writing the card, or the card was replaced).
std::chrono::steady_clock::time_point s_failed_at;
constexpr auto RETRY_AFTER = std::chrono::seconds(3);
std::vector<u8> s_bundle;
std::string s_error;

u32 BE32(const u8* p)
{
  return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3];
}
u16 BE16(const u8* p)
{
  return static_cast<u16>(p[0] << 8 | p[1]);
}
void Put32(std::vector<u8>& b, size_t at, u32 v)
{
  b[at] = static_cast<u8>(v >> 24);
  b[at + 1] = static_cast<u8>(v >> 16);
  b[at + 2] = static_cast<u8>(v >> 8);
  b[at + 3] = static_cast<u8>(v);
}
void Put16(std::vector<u8>& b, size_t at, u16 v)
{
  b[at] = static_cast<u8>(v >> 8);
  b[at + 1] = static_cast<u8>(v);
}

bool Contains(std::span<const u8> data, std::string_view needle)
{
  return std::search(data.begin(), data.end(), needle.begin(), needle.end()) != data.end();
}

// An ARC's entries: {type, index, data}. Empty for anything that is not one (or is cut short).
struct ArcEntry
{
  s16 type;
  s16 index;
  std::span<const u8> data;
};
std::vector<ArcEntry> ArcEntries(std::span<const u8> arc)
{
  std::vector<ArcEntry> out;
  if (arc.size() < 0x40 || std::memcmp(arc.data(), "ARC\0", 4) != 0)
    return out;
  const u16 count = BE16(&arc[6]);
  size_t off = 0x40;
  for (u16 i = 0; i < count; ++i)
  {
    if (off + 0x20 > arc.size())
      return {};
    const s16 type = static_cast<s16>(BE16(&arc[off]));
    const s16 index = static_cast<s16>(BE16(&arc[off + 2]));
    const u32 size = BE32(&arc[off + 4]);
    if (size > arc.size() - off - 0x20)
      return {};
    out.push_back({type, index, arc.subspan(off + 0x20, size)});
    off = (off + 0x20 + size + 0x1F) & ~size_t{0x1F};
  }
  return out;
}

void Build(std::string path)
{
  std::string why;
  std::optional<std::vector<u8>> bundle;
  // Shared: the emulated SD slot has the image open for reading and writing (SDCardFileStorage);
  // a plain open asks to deny writers and fails while the game runs (staging, 2026-10-27).
  File::IOFile image(path, "rb", File::SharedAccess::ReadWrite);
  if (!image.IsOpen())
  {
    why = fmt::format("no SD card image at {}", path);
  }
  else
  {
    std::optional<std::vector<u8>> packed;
    for (const std::string_view p : ARCHIVE_PATHS)
    {
      packed = Machine::SDCard::ReadCardFile(image, image.GetSize(), p, MAX_ARCHIVE);
      if (packed)
        break;
    }
    if (!packed)
    {
      why = "no mu_menumain.pac on the SD card";
    }
    else
    {
      const auto archive = DecompressLz11(*packed, 64ull << 20);
      const std::span<const u8> arc =
          archive ? std::span<const u8>(*archive) : std::span<const u8>(*packed);
      bundle = BuildBundle(arc, &why);
    }
  }
  std::lock_guard lk(s_mutex);
  if (bundle)
  {
    s_bundle = std::move(*bundle);
    s_state = State::Ready;
    NOTICE_LOG_FMT(NETPLAY, "SpecialAssets: the Special Versus box is ready ({} bytes)", s_bundle.size());
  }
  else
  {
    s_error = why;
    s_state = State::Unavailable;
    s_failed_at = std::chrono::steady_clock::now();
    WARN_LOG_FMT(NETPLAY, "SpecialAssets: no Special Versus box: {}", why);
  }
}
}  // namespace

std::optional<std::vector<u8>> DecompressLz11(std::span<const u8> in, u64 max_out)
{
  if (in.size() < 4 || (in[0] != 0x10 && in[0] != 0x11))
    return std::nullopt;
  const bool lz11 = in[0] == 0x11;
  size_t p = 4;
  u64 size = u64(in[1]) | u64(in[2]) << 8 | u64(in[3]) << 16;
  if (size == 0)
  {
    if (in.size() < 8)
      return std::nullopt;
    size = u64(in[4]) | u64(in[5]) << 8 | u64(in[6]) << 16 | u64(in[7]) << 24;
    p = 8;
  }
  if (size > max_out)
    return std::nullopt;
  std::vector<u8> out;
  out.reserve(static_cast<size_t>(size));
  while (out.size() < size)
  {
    if (p >= in.size())
      return std::nullopt;
    const u8 flags = in[p++];
    for (int bit = 7; bit >= 0 && out.size() < size; --bit)
    {
      if (!(flags & (1 << bit)))
      {
        if (p >= in.size())
          return std::nullopt;
        out.push_back(in[p++]);
        continue;
      }
      size_t len, disp;
      if (!lz11)
      {
        if (p + 2 > in.size())
          return std::nullopt;
        const u32 x = u32(in[p]) << 8 | in[p + 1];
        p += 2;
        len = (x >> 12) + 3;
        disp = (x & 0xFFF) + 1;
      }
      else
      {
        if (p >= in.size())
          return std::nullopt;
        const u8 ind = in[p] >> 4;
        if (ind == 0)
        {
          if (p + 3 > in.size())
            return std::nullopt;
          len = ((size_t(in[p] & 0xF) << 4) | (in[p + 1] >> 4)) + 0x11;
          disp = ((size_t(in[p + 1] & 0xF) << 8) | in[p + 2]) + 1;
          p += 3;
        }
        else if (ind == 1)
        {
          if (p + 4 > in.size())
            return std::nullopt;
          len = ((size_t(in[p] & 0xF) << 12) | (size_t(in[p + 1]) << 4) | (in[p + 2] >> 4)) + 0x111;
          disp = ((size_t(in[p + 2] & 0xF) << 8) | in[p + 3]) + 1;
          p += 4;
        }
        else
        {
          if (p + 2 > in.size())
            return std::nullopt;
          len = ind + 1;
          disp = ((size_t(in[p] & 0xF) << 8) | in[p + 1]) + 1;
          p += 2;
        }
      }
      if (disp > out.size())
        return std::nullopt;
      for (size_t i = 0; i < len && out.size() < size; ++i)
        out.push_back(out[out.size() - disp]);
    }
  }
  return out;
}

std::optional<std::vector<u8>> BuildBundle(std::span<const u8> archive, std::string* why)
{
  // The menu's own ARC is the one named MenuMain_* inside the archive (the language's).
  std::array<std::span<const u8>, ROLE_COUNT> found{};
  std::array<s16, ROLE_COUNT> types{};
  bool menu = false;
  for (const ArcEntry& top : ArcEntries(archive))
  {
    if (top.data.size() < 0x20 || std::memcmp(top.data.data(), "ARC\0", 4) != 0 ||
        std::memcmp(top.data.data() + 0x10, "MenuMain", 8) != 0)
    {
      continue;
    }
    menu = true;
    for (const ArcEntry& e : ArcEntries(top.data))
    {
      if (e.data.size() < 4)
        continue;
      const bool bres = std::memcmp(e.data.data(), "bres", 4) == 0;
      if (bres && e.type == 3 && !found[ROLE_TEXTURES].data() &&
          Contains(e.data, std::string_view("MenMainSpecial04Frame\0", 22)))
      {
        found[ROLE_TEXTURES] = e.data;
        types[ROLE_TEXTURES] = e.type;
        continue;
      }
      if (bres && e.type == 2)
      {
        for (u16 r = ROLE_MODEL_FIRST; r <= ROLE_MODEL_LAST; ++r)
        {
          const std::string name = fmt::format("MenMainSpecial{:04}_TopN", r);
          if (!found[r].data() && Contains(e.data, std::string_view(name.c_str(), name.size() + 1)) &&
              Contains(e.data, "MDL0"))
          {
            found[r] = e.data;
            types[r] = e.type;
            break;
          }
        }
        continue;
      }
      // The descriptions (the box's own msbin, 60 strings): its first offset is 4 * the count.
      if (!bres && e.type == 1 && e.index == 1 && !found[ROLE_MESSAGES].data() &&
          BE32(e.data.data()) == 0xF0 && e.data.size() > 0xF0)
      {
        found[ROLE_MESSAGES] = e.data;
        types[ROLE_MESSAGES] = e.type;
      }
    }
    break;
  }
  if (!menu)
  {
    *why = "not P+'s main-menu archive (no MenuMain ARC)";
    return std::nullopt;
  }
  for (u16 r = 0; r < ROLE_COUNT; ++r)
  {
    if (!found[r].data())
    {
      *why = fmt::format("the archive has no Special Versus part {}", r);
      return std::nullopt;
    }
  }
  std::vector<u8> out(HEADER + ROLE_COUNT * ENTRY, 0);
  out.resize((out.size() + 0x1F) & ~size_t{0x1F});
  for (u16 r = 0; r < ROLE_COUNT; ++r)
  {
    const size_t at = out.size();
    out.insert(out.end(), found[r].begin(), found[r].end());
    out.resize((out.size() + 0x1F) & ~size_t{0x1F});
    const size_t e = HEADER + r * ENTRY;
    Put16(out, e, r);
    Put16(out, e + 2, static_cast<u16>(types[r]));
    Put32(out, e + 4, static_cast<u32>(at));
    Put32(out, e + 8, static_cast<u32>(found[r].size()));
  }
  if (out.size() > MAX_BUNDLE)
  {
    *why = fmt::format("the box is {} bytes, more than {}", out.size(), MAX_BUNDLE);
    return std::nullopt;
  }
  Put32(out, 0, MAGIC);
  Put32(out, 4, static_cast<u32>(out.size()));
  Put32(out, 8, ROLE_COUNT);
  return out;
}

State Request(const std::string& image_path)
{
  std::lock_guard lk(s_mutex);
  if (s_state == State::None ||
      (s_state == State::Unavailable && std::chrono::steady_clock::now() - s_failed_at >= RETRY_AFTER))
  {
    s_state = State::Loading;
    std::thread(Build, image_path).detach();
  }
  return s_state;
}

State GetState()
{
  std::lock_guard lk(s_mutex);
  return s_state;
}

std::vector<u8> Bundle()
{
  std::lock_guard lk(s_mutex);
  return s_state == State::Ready ? s_bundle : std::vector<u8>{};
}

std::string Error()
{
  std::lock_guard lk(s_mutex);
  return s_error;
}
}  // namespace Online::SpecialAssets
