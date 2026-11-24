// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Rollback/ReplayKeyframes.h (core/standalone only): seeking in
// a replay (docs/design/replays.md §6.4) with keyframes of the match taken while it plays, kept in
// memory only, as Dolphin's are.
//
// A keyframe is what Dolphin's keyframe holds, at a loop top: the gameplay region set's bytes (the
// host's guest memory, BrawlOnline::Host) and the main loop's registers (the engine's guest
// registers, BrawlOnline::Engine::SaveGuestRegs). A restore writes the bytes back through the
// host, each changed block marked dirty (Host::MarkDirty: the engine's snapshots track host
// writes), then the registers. The first keyframe is kept whole; every later one as its XOR
// against the first, compressed on a worker thread, as in Dolphin.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "BrawlOnline/Engine.h"
#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace Gprb::Replay
{
class Keyframes
{
public:
  Keyframes();
  ~Keyframes();
  Keyframes(const Keyframes&) = delete;
  Keyframes& operator=(const Keyframes&) = delete;

  // A new match: the ranges (effective addresses) a keyframe holds. Forgets every keyframe.
  void Reset(std::vector<std::pair<u32, u32>> ranges);
  void Clear();
  bool Empty() const;

  // The game's thread, at a loop top: the state before frame `frame` runs.
  void Capture(Core::System& system, s64 frame);
  bool Has(s64 frame) const;
  // The newest keyframe at or before `frame`.
  std::optional<s64> AtOrBefore(s64 frame) const;
  // The game's thread, at a loop top: puts keyframe `frame` back. False if there is none.
  bool Restore(Core::System& system, s64 frame);

  // Memory the keyframes take (the first one's raw bytes included).
  u64 Bytes() const;
  size_t Count() const;
  // Over this, the oldest keyframes but the first are dropped (every other one).
  static constexpr u64 MAX_BYTES = 768ull << 20;

private:
  struct Key
  {
    s64 frame = 0;
    BrawlOnline::GuestRegs regs{};
    std::vector<u8> raw;     // until compressed (or the first keyframe's bytes)
    std::vector<u8> packed;  // XOR against the first, deflated
    u32 packed_raw = 0;
    bool ready = false;      // packed (or the first keyframe)
  };

  void Worker();
  void Trim();

  mutable std::mutex m_mutex;
  std::condition_variable m_cv, m_done_cv;
  std::vector<std::pair<u32, u32>> m_ranges;
  u64 m_range_bytes = 0;
  std::map<s64, std::shared_ptr<Key>> m_keys;
  std::shared_ptr<Key> m_base;
  std::deque<std::shared_ptr<Key>> m_jobs;
  bool m_quit = false;
  std::thread m_thread;
};
}  // namespace Gprb::Replay
