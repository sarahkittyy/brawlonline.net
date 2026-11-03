// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstring>
#include <span>

#include "Common/CommonTypes.h"

namespace Rollback
{
// A save's copy jobs (DeltaSaveSlot::Save) each take `split` of the written granules, in scan
// order, and write the ones they keep from the start of their own range: job n kept kept[n]. A
// whole-machine save keeps only those whose bytes changed since the previous save; a region-mode
// save keeps them all, and nothing moves. This moves the kept granules together, in the same order,
// and returns how many there are. `data` holds `granule_size` bytes per granule, `indices` one
// index per granule.
inline u32 CompactKeptGranules(u32* indices, u8* data, size_t granule_size, u32 split,
                               std::span<const u32> kept)
{
  u32 write = 0;
  for (size_t n = 0; n < kept.size(); ++n)
  {
    const u32 from = static_cast<u32>(n) * split;
    const u32 count = kept[n];
    if (count != 0 && from != write)
    {
      std::memmove(indices + write, indices + from, count * sizeof(u32));
      std::memmove(data + static_cast<size_t>(write) * granule_size,
                   data + static_cast<size_t>(from) * granule_size,
                   static_cast<size_t>(count) * granule_size);
    }
    write += count;
  }
  return write;
}
}  // namespace Rollback
