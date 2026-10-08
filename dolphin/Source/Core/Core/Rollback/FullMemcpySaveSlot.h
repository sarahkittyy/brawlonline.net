// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

#include "Common/Buffer.h"
#include "Core/Rollback/IRollbackSaveSlot.h"

namespace Rollback
{

// Full-memcpy rollback slot: captures a complete snapshot of RAM + non-RAM state
class FullMemcpySaveSlot final : public IRollbackSaveSlot
{
public:
  void Init(uint8_t* mem1_ptr, size_t mem1_size, uint8_t* mem2_ptr, size_t mem2_size,
            uint8_t* l1_cache_ptr, size_t l1_cache_size);

  bool HasState() const override { return m_has_state; }
  void Reset() override;
  void Save(Core::System& system) override;
  void Load(Core::System& system) override;

private:
  uint8_t* m_mem1_ptr = nullptr;
  size_t m_mem1_size = 0;
  uint8_t* m_mem2_ptr = nullptr;
  size_t m_mem2_size = 0;
  uint8_t* m_l1_cache_ptr = nullptr;
  size_t m_l1_cache_size = 0;

  Common::UniqueBuffer<uint8_t> m_mem1_snapshot;
  Common::UniqueBuffer<uint8_t> m_mem2_snapshot;
  Common::UniqueBuffer<uint8_t> m_l1_cache_snapshot;

  Common::UniqueBuffer<uint8_t> m_save_buffer;

  bool m_has_state = false;
};

}  // namespace Rollback
