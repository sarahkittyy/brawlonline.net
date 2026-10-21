// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

#include "Common/CommonTypes.h"

namespace Common
{
// The last N values of something that changes over time, each with the host time it took effect
// (a controller's state as each read of it found it). At(t) gives the value it had at an earlier
// time. Push times are expected in order. Not thread-safe: the owner locks.
template <typename T, std::size_t N>
class TimedHistory
{
public:
  void Push(Clock::time_point at, const T& value)
  {
    m_entries[m_next] = {at, value};
    m_next = (m_next + 1) % N;
    m_size = std::min(m_size + 1, N);
  }

  void Clear()
  {
    m_size = 0;
    m_next = 0;
  }

  bool Empty() const { return m_size == 0; }

  // The value at `t`: the newest one that took effect at or before it. nullptr when `t` is older
  // than every value kept (or none is).
  const T* At(Clock::time_point t) const
  {
    for (std::size_t i = 0; i < m_size; ++i)
    {
      const Entry& e = m_entries[(m_next + N - 1 - i) % N];
      if (e.at <= t)
        return &e.value;
    }
    return nullptr;
  }

private:
  struct Entry
  {
    Clock::time_point at{};
    T value{};
  };
  std::array<Entry, N> m_entries{};
  std::size_t m_next = 0;
  std::size_t m_size = 0;
};
}  // namespace Common
