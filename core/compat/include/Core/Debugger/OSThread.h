// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Debugger/OSThread.h (core/standalone only): a guest OSThread
// read from RAM for the core's stall reports (the fields at their OSThread offsets).

#pragma once

#include "Common/CommonTypes.h"
#include "Core/PowerPC/MMU.h"

namespace Core::Debug
{
struct OSContext
{
  u32 gpr[32]{};
  u32 lr = 0;
  u32 srr0 = 0;
  u32 srr1 = 0;
};

struct OSThread
{
  OSContext context;
  u16 state = 0;
  u16 suspend_attr = 0;
  s32 suspend = 0;
  s32 effective_priority = 0;
  s32 base_priority = 0;
  u32 queue_addr = 0;
};

class OSThreadView
{
public:
  OSThreadView(const Core::CPUThreadGuard& guard, u32 addr)
  {
    const auto r32 = [&](u32 off) { return PowerPC::MMU::HostRead<u32>(guard, addr + off); };
    for (int i = 0; i < 32; ++i)
      m_thread.context.gpr[i] = r32(4 * i);
    m_thread.context.lr = r32(0x84);
    m_thread.context.srr0 = r32(0x198);
    m_thread.context.srr1 = r32(0x19C);
    m_thread.state = PowerPC::MMU::HostRead<u16>(guard, addr + 0x2C8);
    m_thread.suspend = static_cast<s32>(r32(0x2CC));
    m_thread.effective_priority = static_cast<s32>(r32(0x2D0));
    m_thread.base_priority = static_cast<s32>(r32(0x2D4));
    m_thread.queue_addr = r32(0x2DC);
  }
  const OSThread& Data() const { return m_thread; }

private:
  OSThread m_thread;
};
}  // namespace Core::Debug
