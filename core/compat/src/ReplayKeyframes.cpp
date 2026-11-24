// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Replay keyframes outside Dolphin (core/compat/include/Core/Rollback/ReplayKeyframes.h): Dolphin's
// Core/Rollback/ReplayKeyframes.cpp over BrawlOnline::Host's guest memory and the engine's guest
// registers.

#include "Core/Rollback/ReplayKeyframes.h"

#include <algorithm>
#include <cstring>

#include <zlib.h>

#include "BrawlOnline/Host.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"

namespace Gprb::Replay
{
namespace
{
// A restore writes back only the blocks that differ, each marked dirty: the engine's snapshots
// then see what changed (and only that) at the next save.
constexpr size_t RESTORE_BLOCK = 4096;

void WriteBack(u32 addr, const u8* image, u32 size)
{
  auto& host = BrawlOnline::GetHost();
  u8* p = host.GuestPointer(addr, size);
  if (!p)
    return;
  for (u32 at = 0; at < size; at += RESTORE_BLOCK)
  {
    const u32 n = std::min<u32>(RESTORE_BLOCK, size - at);
    if (std::memcmp(p + at, image + at, n) == 0)
      continue;
    std::memcpy(p + at, image + at, n);
    host.MarkDirty(addr + at, n);
  }
}
}  // namespace

Keyframes::Keyframes() = default;

Keyframes::~Keyframes()
{
  {
    std::lock_guard lk(m_mutex);
    m_quit = true;
  }
  m_cv.notify_all();
  if (m_thread.joinable())
    m_thread.join();
}

void Keyframes::Reset(std::vector<std::pair<u32, u32>> ranges)
{
  std::lock_guard lk(m_mutex);
  m_keys.clear();
  m_base.reset();
  m_jobs.clear();
  m_ranges = std::move(ranges);
  m_range_bytes = 0;
  for (const auto& [addr, size] : m_ranges)
    m_range_bytes += size;
}

void Keyframes::Clear()
{
  std::lock_guard lk(m_mutex);
  m_keys.clear();
  m_base.reset();
  m_jobs.clear();
  m_ranges.clear();
  m_range_bytes = 0;
}

bool Keyframes::Empty() const
{
  std::lock_guard lk(m_mutex);
  return m_keys.empty();
}

void Keyframes::Capture(Core::System&, s64 frame)
{
  auto key = std::make_shared<Key>();
  key->frame = frame;
  {
    std::lock_guard lk(m_mutex);
    if (m_ranges.empty() || m_keys.count(frame))
      return;
  }
  BrawlOnline::GetEngine().SaveGuestRegs(&key->regs);
  // The copy is the only work on the game's thread.
  key->raw.resize(m_range_bytes);
  auto& host = BrawlOnline::GetHost();
  size_t at = 0;
  for (const auto& [addr, size] : m_ranges)
  {
    if (const u8* p = host.GuestPointer(addr, size))
      std::memcpy(key->raw.data() + at, p, size);
    else
      std::memset(key->raw.data() + at, 0, size);
    at += size;
  }
  std::lock_guard lk(m_mutex);
  if (!m_base)
  {
    key->ready = true;
    m_base = key;
  }
  else
  {
    m_jobs.push_back(key);
    if (!m_thread.joinable())
      m_thread = std::thread([this] { Worker(); });
    m_cv.notify_one();
  }
  m_keys[frame] = key;
  Trim();
}

void Keyframes::Worker()
{
  Common::SetCurrentThreadName("Replay keyframes");
  std::unique_lock lk(m_mutex);
  while (true)
  {
    m_cv.wait(lk, [this] { return m_quit || !m_jobs.empty(); });
    if (m_quit)
      return;
    std::shared_ptr<Key> key = m_jobs.front();
    m_jobs.pop_front();
    std::shared_ptr<Key> base = m_base;
    lk.unlock();
    if (base && base->raw.size() == key->raw.size())
    {
      for (size_t i = 0; i < key->raw.size(); ++i)
        key->raw[i] ^= base->raw[i];
      uLongf size = compressBound(static_cast<uLong>(key->raw.size()));
      std::vector<u8> packed(size);
      if (compress2(packed.data(), &size, key->raw.data(), static_cast<uLong>(key->raw.size()), 1) ==
          Z_OK)
      {
        packed.resize(size);
        packed.shrink_to_fit();
        lk.lock();
        key->packed = std::move(packed);
        key->packed_raw = static_cast<u32>(key->raw.size());
        key->raw.clear();
        key->raw.shrink_to_fit();
        key->ready = true;
        m_done_cv.notify_all();
        Trim();
        continue;
      }
      // (Unpacked: keep the XOR as it is.)
      lk.lock();
      key->packed = std::move(key->raw);
      key->packed_raw = 0;
      key->ready = true;
      m_done_cv.notify_all();
      continue;
    }
    lk.lock();
    m_keys.erase(key->frame);  // the first keyframe changed under it (a new match)
    m_done_cv.notify_all();
  }
}

void Keyframes::Trim()
{
  // (m_mutex held.) Over the budget: every other keyframe of the older half goes (never the
  // first), so the ones near where the replay is stay dense.
  const auto bytes = [this] {
    u64 n = 0;
    for (const auto& [f, k] : m_keys)
      n += k->raw.size() + k->packed.size();
    return n;
  };
  while (bytes() > MAX_BYTES && m_keys.size() > 2)
  {
    std::vector<s64> frames;
    for (const auto& [f, k] : m_keys)
    {
      if (k != m_base && k->ready)
        frames.push_back(f);
    }
    if (frames.size() < 2)
      return;
    const size_t half = std::max<size_t>(frames.size() / 2, 1);
    for (size_t i = 0; i < half; i += 2)
      m_keys.erase(frames[i]);
  }
}

bool Keyframes::Has(s64 frame) const
{
  std::lock_guard lk(m_mutex);
  return m_keys.count(frame) != 0;
}

std::optional<s64> Keyframes::AtOrBefore(s64 frame) const
{
  std::lock_guard lk(m_mutex);
  auto it = m_keys.upper_bound(frame);
  if (it == m_keys.begin())
    return std::nullopt;
  return std::prev(it)->first;
}

bool Keyframes::Restore(Core::System&, s64 frame)
{
  std::unique_lock lk(m_mutex);
  const auto it = m_keys.find(frame);
  if (it == m_keys.end() || !m_base)
    return false;
  std::shared_ptr<Key> key = it->second;
  std::shared_ptr<Key> base = m_base;
  m_done_cv.wait(lk, [&] { return key->ready || !m_keys.count(frame); });
  if (!key->ready)
    return false;
  const auto ranges = m_ranges;
  lk.unlock();

  std::vector<u8> bytes;
  const std::vector<u8>* image = &base->raw;
  if (key != base)
  {
    if (key->packed_raw)
    {
      bytes.resize(key->packed_raw);
      uLongf size = key->packed_raw;
      if (uncompress(bytes.data(), &size, key->packed.data(), static_cast<uLong>(key->packed.size())) !=
              Z_OK ||
          size != key->packed_raw)
      {
        ERROR_LOG_FMT(BRAWLBACK, "replay: keyframe {} does not decompress", frame);
        return false;
      }
    }
    else
    {
      bytes = key->packed;
    }
    if (bytes.size() != base->raw.size())
      return false;
    for (size_t i = 0; i < bytes.size(); ++i)
      bytes[i] ^= base->raw[i];
    image = &bytes;
  }
  size_t at = 0;
  for (const auto& [addr, size] : ranges)
  {
    WriteBack(addr, image->data() + at, size);
    at += size;
  }
  BrawlOnline::GetEngine().RestoreGuestRegs(key->regs);
  return true;
}

u64 Keyframes::Bytes() const
{
  std::lock_guard lk(m_mutex);
  u64 n = 0;
  for (const auto& [f, k] : m_keys)
    n += k->raw.size() + k->packed.size();
  return n;
}

size_t Keyframes::Count() const
{
  std::lock_guard lk(m_mutex);
  return m_keys.size();
}
}  // namespace Gprb::Replay
