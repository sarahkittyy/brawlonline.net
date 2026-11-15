// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/HW/Memmap.h (core/standalone only): guest RAM through the
// host (BrawlOnline::Host), big-endian bytes as on the console.

#pragma once

#include <cstddef>
#include <span>

#include "Common/CommonTypes.h"

namespace Memory
{
class MemoryManager
{
public:
  u32 GetRamSizeReal() const;
  u32 GetExRamSizeReal() const;
  u8* GetRAM() const;
  u8* GetEXRAM() const;

  std::span<u8> GetSpanForAddress(u32 address) const;
  u8* GetPointerForRange(u32 address, size_t size) const;
  void CopyFromEmu(void* data, u32 address, size_t size) const;
  void CopyToEmu(u32 address, const void* data, size_t size);

  u8 Read_U8(u32 address) const;
  u16 Read_U16(u32 address) const;
  u32 Read_U32(u32 address) const;
  void Write_U8(u8 var, u32 address);
  void Write_U16(u16 var, u32 address);
  void Write_U32(u32 var, u32 address);
};
}  // namespace Memory
