// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Seeking in a replay (docs/design/replays.md §6.4): keyframes of the match taken while it plays,
// kept in memory only (never written to a file, never read from one).
//
// A keyframe is what a rollback restores, at a loop top: the gameplay region set's bytes and the
// main thread's registers (RollbackManager's region mode). Live rollbacks restore at most 7
// frames back; a keyframe is the same restore from further back, and the frames after it are run
// again from the replay's inputs, unthrottled and unpresented, to the frame wanted.
//
// The first keyframe is kept whole; every later one as its XOR against the first, compressed on a
// worker thread (most of the set does not change between them). The CPU thread only copies the
// bytes (tens of MB, a few ms); a restore waits for that keyframe's job.

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Rollback/DeltaSaveSlot.h"

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

  // CPU thread, at a loop top (no file read in flight): the state before frame `frame` runs.
  void Capture(Core::System& system, s64 frame);
  bool Has(s64 frame) const;
  // The newest keyframe at or before `frame`.
  std::optional<s64> AtOrBefore(s64 frame) const;
  // CPU thread, at a loop top: puts keyframe `frame` back. False if there is none.
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
    Rollback::DeltaSaveSlot::CPURegisters regs{};
    std::vector<u8> raw;     // until compressed (or the first keyframe's bytes)
    std::vector<u8> packed;  // XOR against the first, deflated
    u32 packed_raw = 0;
    bool ready = false;      // packed (or the first keyframe)
  };

  void Worker();
  void Pack(Key& key);
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
