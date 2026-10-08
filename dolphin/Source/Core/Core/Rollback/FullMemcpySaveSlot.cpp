// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/FullMemcpySaveSlot.h"

#include <cassert>
#include <cstring>

#include <Common/Assert.h>
#include "Core/Rollback/Perf.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Rollback
{

void FullMemcpySaveSlot::Init(uint8_t* mem1_ptr, size_t mem1_size, uint8_t* mem2_ptr,
                              size_t mem2_size, uint8_t* l1_cache_ptr, size_t l1_cache_size)
{
  m_mem1_ptr = mem1_ptr;
  m_mem1_size = mem1_size;
  m_mem2_ptr = mem2_ptr;
  m_mem2_size = mem2_size;
  m_l1_cache_ptr = l1_cache_ptr;
  m_l1_cache_size = l1_cache_size;

  m_mem1_snapshot.reset(mem1_size);
  if (mem2_ptr && mem2_size > 0)
    m_mem2_snapshot.reset(mem2_size);
  if (l1_cache_ptr && l1_cache_size > 0)
    m_l1_cache_snapshot.reset(l1_cache_size);

  m_has_state = false;
}

void FullMemcpySaveSlot::Reset()
{
  m_has_state = false;
  m_save_buffer.reset();
}

void FullMemcpySaveSlot::Save(Core::System& system)
{
  ROLLBACK_ZONE();

  ASSERT(m_mem1_ptr && m_mem1_snapshot.data());

  std::memcpy(m_mem1_snapshot.data(), m_mem1_ptr, m_mem1_size);
  if (m_mem2_ptr && m_mem2_size > 0 && m_mem2_snapshot.data())
    std::memcpy(m_mem2_snapshot.data(), m_mem2_ptr, m_mem2_size);
  if (m_l1_cache_ptr && m_l1_cache_size > 0 && m_l1_cache_snapshot.data())
    std::memcpy(m_l1_cache_snapshot.data(), m_l1_cache_ptr, m_l1_cache_size);

  auto& mgr = RollbackManager::Get();
  mgr.BeginDoState();
  State::SaveToBuffer(system, m_save_buffer);
  mgr.EndDoState();

  m_has_state = true;
}

void FullMemcpySaveSlot::Load(Core::System& system)
{
  ROLLBACK_ZONE();

  ASSERT(m_has_state);
  ASSERT(m_mem1_ptr && m_mem1_snapshot.data());

  std::memcpy(m_mem1_ptr, m_mem1_snapshot.data(), m_mem1_size);
  if (m_mem2_ptr && m_mem2_size > 0 && m_mem2_snapshot.data())
    std::memcpy(m_mem2_ptr, m_mem2_snapshot.data(), m_mem2_size);
  if (m_l1_cache_ptr && m_l1_cache_size > 0 && m_l1_cache_snapshot.data())
    std::memcpy(m_l1_cache_ptr, m_l1_cache_snapshot.data(), m_l1_cache_size);

  auto& mgr = RollbackManager::Get();
  mgr.BeginDoState();
  const bool ok =
      State::LoadFromBuffer(system, std::span<uint8_t>(m_save_buffer.data(), m_save_buffer.size()));
  mgr.EndDoState();

  if (ok)
    OSD::AddMessage("Rollback state loaded", 2000);
  else
    OSD::AddMessage("Rollback state load failed", 3000, OSD::Color::RED);
}

}  // namespace Rollback
