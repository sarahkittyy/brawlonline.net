// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Hash.h"
#include "Core/Rollback/DeltaSaveSlot.h"

namespace Rollback
{
// The change CRC's feed (GitHub issue #14): the (page, granule bytes) stream of a save's changed
// granules. Entries collect in a buffer and go to crc32 in blocks instead of two calls (4 bytes,
// then 64) per granule: the byte stream, and so the CRC peers compare, is the same.
struct BatchedChangeCRC
{
  static constexpr size_t kGranuleSize = 64;
  static constexpr u32 kGranulesPerFlush = 128;
  static constexpr size_t kEntrySize = sizeof(u32) + kGranuleSize;

  void Feed(u32 page, const u8* granule)
  {
    if (m_len == m_buffer.size())
      Flush();
    std::memcpy(m_buffer.data() + m_len, &page, sizeof(page));
    std::memcpy(m_buffer.data() + m_len + sizeof(page), granule, kGranuleSize);
    m_len += kEntrySize;
  }

  // The CRC of everything fed so far.
  u32 Finish()
  {
    Flush();
    return m_crc;
  }

private:
  void Flush()
  {
    if (!m_len)
      return;
    m_crc = Common::UpdateCRC32(m_crc, m_buffer.data(), m_len);
    m_len = 0;
  }

  // Not value-initialized: only [0, m_len) is read.
  std::array<u8, kGranulesPerFlush * kEntrySize> m_buffer;
  size_t m_len = 0;
  u32 m_crc = Common::StartCRC32();
};

// The slice and full checksums' feed: the bytes of the granules they count, in order. A granule
// that starts where the one before it ended joins its run, and a run goes to crc32 in one call
// instead of one call per 64-byte granule: the byte stream, and so the CRC, is the same.
struct RunCRC
{
  explicit RunCRC(u32 crc = Common::StartCRC32()) : m_crc(crc) {}

  // `data` must stay as it is until Finish.
  void Feed(const u8* data, size_t len)
  {
    if (data != m_run + m_len)
    {
      Flush();
      m_run = data;
    }
    m_len += len;
  }

  // For a buffer reused after this call (a masked granule's copy): counted now.
  void FeedCopy(const u8* data, size_t len)
  {
    Flush();
    m_crc = Common::UpdateCRC32(m_crc, data, len);
  }

  // The CRC of everything fed so far.
  u32 Finish()
  {
    Flush();
    return m_crc;
  }

private:
  void Flush()
  {
    if (m_len)
      m_crc = Common::UpdateCRC32(m_crc, m_run, m_len);
    m_run = nullptr;
    m_len = 0;
  }

  const u8* m_run = nullptr;
  size_t m_len = 0;
  u32 m_crc;
};

// A granule's bytes as the checksums read them: `src`, or its copy in `scratch` with the bytes
// `masks` cover zeroed.
inline const u8* MaskedForChecksum(u32 phys, const u8* src, std::span<const MemoryRegion> masks,
                                   u8* scratch)
{
  const u8* out = src;
  for (const MemoryRegion& m : masks)
  {
    if (m.phys_start >= phys + ROLLBACK_PAGE_SIZE || m.phys_end <= phys)
      continue;
    if (out == src)
    {
      std::memcpy(scratch, src, ROLLBACK_PAGE_SIZE);
      out = scratch;
    }
    const u32 from = std::max(m.phys_start, phys) - phys;
    const u32 to = std::min<u32>(m.phys_end, phys + ROLLBACK_PAGE_SIZE) - phys;
    std::memset(scratch + from, 0, to - from);
  }
  return out;
}

// Guest RAM as the slice and full checksums count it. Granules are numbered MEM1's first, then
// MEM2's; a GPU-written granule (rendered output) and one an excluded region touches are left
// out, and one a mask touches counts with the masked bytes as zeros.
struct CountedRAM
{
  const u8* mem1;
  u32 mem1_granules;
  const u8* mem2;
  u32 mem2_granules;
  const u8* gpu;  // by page (physical address >> ROLLBACK_PAGE_SHIFT): nonzero is GPU-written
  std::span<const MemoryRegion> excluded;
  std::span<const MemoryRegion> masks;
};

// The bytes of granules [first, last) that count, in order, to `crc` and (when not null) `also`;
// the physical address of each granule left out as GPU-written to `gpu_skipped` (when not null).
// The regions are looked at once per stretch of granules none of them touches, not per granule,
// and such a stretch goes to the feeds as runs of adjacent granules.
inline void FeedCountedGranules(const CountedRAM& ram, u32 first, u32 last, RunCRC& crc,
                                RunCRC* also, std::vector<u32>* gpu_skipped)
{
  struct Area
  {
    u32 first_page, first_granule, granules;
    const u8* bytes;
  };
  const std::array<Area, 2> areas{{{0, 0, ram.mem1_granules, ram.mem1},
                                   {MEM2_FIRST_PAGE, ram.mem1_granules, ram.mem2_granules,
                                    ram.mem2}}};
  u8 scratch[ROLLBACK_PAGE_SIZE];
  for (const Area& area : areas)
  {
    const u32 begin = std::max(first, area.first_granule);
    const u32 end = std::min(last, area.first_granule + area.granules);
    const auto page_of = [&](u32 g) { return area.first_page + (g - area.first_granule); };
    // The granules [*lo, *hi) of this area `r` touches (none when *lo >= *hi).
    const auto touched = [&](const MemoryRegion& r, u32* lo, u32* hi) {
      const u64 area_end = u64{area.first_page} + area.granules;
      const u64 from = std::clamp<u64>(r.phys_start >> ROLLBACK_PAGE_SHIFT, area.first_page,
                                       area_end);
      const u64 to = std::clamp<u64>((u64{r.phys_end} + ROLLBACK_PAGE_SIZE - 1) >>
                                         ROLLBACK_PAGE_SHIFT,
                                     area.first_page, area_end);
      *lo = static_cast<u32>(from - area.first_page) + area.first_granule;
      *hi = static_cast<u32>(to - area.first_page) + area.first_granule;
    };
    for (u32 g = begin; g < end;)
    {
      // Granules [g, plain) are touched by no region, or (masked) only g is looked at.
      u32 plain = end;
      u32 skip_to = g;
      for (const MemoryRegion& r : ram.excluded)
      {
        u32 lo, hi;
        touched(r, &lo, &hi);
        if (lo <= g && g < hi)
          skip_to = std::max(skip_to, hi);
        else if (g < lo)
          plain = std::min(plain, lo);
      }
      if (skip_to > g)
      {
        skip_to = std::min(skip_to, end);
        for (; gpu_skipped && g < skip_to; ++g)
        {
          if (ram.gpu[page_of(g)])
            gpu_skipped->push_back(page_of(g) << ROLLBACK_PAGE_SHIFT);
        }
        g = skip_to;
        continue;
      }
      bool masked = false;
      for (const MemoryRegion& m : ram.masks)
      {
        u32 lo, hi;
        touched(m, &lo, &hi);
        if (lo <= g && g < hi)
          masked = true;
        else if (g < lo)
          plain = std::min(plain, lo);
      }
      if (masked)
        plain = g + 1;
      for (; g < plain; ++g)
      {
        const u32 page = page_of(g);
        if (ram.gpu[page])
        {
          if (gpu_skipped)
            gpu_skipped->push_back(page << ROLLBACK_PAGE_SHIFT);
          continue;
        }
        const u8* live =
            area.bytes + static_cast<size_t>(g - area.first_granule) * ROLLBACK_PAGE_SIZE;
        if (!masked)
        {
          crc.Feed(live, ROLLBACK_PAGE_SIZE);
          if (also)
            also->Feed(live, ROLLBACK_PAGE_SIZE);
          continue;
        }
        const u8* src =
            MaskedForChecksum(page << ROLLBACK_PAGE_SHIFT, live, ram.masks, scratch);
        crc.FeedCopy(src, ROLLBACK_PAGE_SIZE);
        if (also)
          also->FeedCopy(src, ROLLBACK_PAGE_SIZE);
      }
    }
  }
}
}  // namespace Rollback
