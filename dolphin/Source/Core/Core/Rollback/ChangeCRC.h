// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/Hash.h"

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
}  // namespace Rollback
