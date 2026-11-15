// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

#include "Common/CommonTypes.h"

namespace Rollback
{
// Which pages of a buffer a load must copy back from an earlier full copy of it (a "capture"), so
// that a load of a mostly unwritten buffer copies those pages and not all of it.
//
// Every write to the buffer is reported (Mark). A capture gets a number (Capture), kept with its
// copy. A page carries the number of captures taken when it was last written, so for every capture
// ever taken, in use or not: a page stamped below the capture's number holds the bytes the capture
// has. RestoreTo names the other pages; the caller copies each from the capture, and they count as
// written now (their bytes change, and another capture may hold what they had).
//
// Not thread-safe. The numbers are 64-bit and never come round.
template <size_t PageCount, size_t PageSize>
class WrittenPages
{
public:
  // [offset, offset + size) of the buffer was written (or may have been).
  void Mark(size_t offset, size_t size)
  {
    if (size == 0 || offset >= PageCount * PageSize)
      return;
    const size_t last = std::min(offset + size - 1, PageCount * PageSize - 1) / PageSize;
    for (size_t page = offset / PageSize; page <= last; ++page)
      m_stamp[page] = m_captures;
  }

  void MarkAll() { m_stamp.fill(m_captures); }

  // A full copy of the buffer is taken now: its number.
  u64 Capture() { return ++m_captures; }

  // The buffer goes back to the capture numbered `capture`: copy_page(page) for each page that may
  // differ from it. Returns how many. A number this never gave out names every page.
  template <typename F>
  size_t RestoreTo(u64 capture, F&& copy_page)
  {
    const bool all = capture == 0 || capture > m_captures;
    size_t pages = 0;
    for (size_t page = 0; page < PageCount; ++page)
    {
      if (!all && m_stamp[page] < capture)
        continue;
      copy_page(page);
      m_stamp[page] = m_captures;
      ++pages;
    }
    return pages;
  }

private:
  std::array<u64, PageCount> m_stamp{};
  u64 m_captures = 0;
};
}  // namespace Rollback
