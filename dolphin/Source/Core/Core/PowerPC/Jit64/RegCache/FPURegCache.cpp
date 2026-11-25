// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/Jit64/RegCache/FPURegCache.h"

#include <array>

#include "Common/CPUDetect.h"
#include "Common/x64ABI.h"
#include "Common/x64Reg.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"

using namespace Gen;

FPURegCache::FPURegCache(Jit64& jit, XEmitter& emitter) : RegCache{jit, emitter}
{
}

bool FPURegCache::IsImm(preg_t preg) const
{
  return false;
}

u32 FPURegCache::Imm32(preg_t preg) const
{
  ASSERT_MSG(DYNA_REC, false, "FPURegCache doesn't support immediates");
  return 0;
}

s32 FPURegCache::SImm32(preg_t preg) const
{
  ASSERT_MSG(DYNA_REC, false, "FPURegCache doesn't support immediates");
  return 0;
}

OpArg FPURegCache::R(preg_t preg) const
{
  if (m_state.m_guests_in_host_register[preg])
  {
    return ::Gen::R(m_state.m_guests_host_register[preg]);
  }
  else
  {
    ASSERT_MSG(DYNA_REC, m_state.m_guests_in_ppc_state[preg], "FPR {} missing!", preg);
    return GetPPCStateLocation(preg);
  }
}

void FPURegCache::StoreRegister(preg_t preg, const OpArg& new_loc,
                                IgnoreDiscardedRegisters ignore_discarded_registers)
{
  if (m_state.m_guests_in_host_register[preg])
  {
    const X64Reg xr = m_state.m_guests_host_register[preg];
    if (m_state.m_guests_single_pair[preg] || m_state.m_guests_single_duplicated[preg])
    {
      m_jit.WidenSingleToDouble(XMM0, xr, m_state.m_guests_single_pair[preg],
                                m_state.m_guests_single_may_be_snan[preg]);
      m_emitter.MOVAPD(new_loc, XMM0);
    }
    else
    {
      m_emitter.MOVAPD(new_loc, xr);
    }
  }
  else
  {
    ASSERT_MSG(DYNA_REC, ignore_discarded_registers != IgnoreDiscardedRegisters::No,
               "FPR {} not in host register", preg);
  }
}

void FPURegCache::SetSingle(preg_t preg, bool is_pair, bool may_be_snan, bool raw_load)
{
  ASSERT_MSG(DYNA_REC, m_state.m_guests_in_host_register[preg], "FPR {} not in host register", preg);
  ASSERT_MSG(DYNA_REC, !(is_pair && may_be_snan), "SinglePair cannot have may_be_snan");
  m_state.m_guests_single_pair[preg] = is_pair;
  m_state.m_guests_single_duplicated[preg] = !is_pair;
  m_state.m_guests_single_may_be_snan[preg] = may_be_snan;
  m_state.m_guests_single_raw_load[preg] = raw_load || may_be_snan;
}

void FPURegCache::ConvertToDouble(preg_t preg)
{
  if (!IsSingle(preg))
    return;
  const X64Reg xr = m_state.m_guests_host_register[preg];
  m_jit.WidenSingleToDouble(xr, xr, m_state.m_guests_single_pair[preg],
                            m_state.m_guests_single_may_be_snan[preg]);
  m_state.m_guests_single_pair[preg] = false;
  m_state.m_guests_single_duplicated[preg] = false;
  m_state.m_guests_single_may_be_snan[preg] = false;
  m_state.m_guests_single_raw_load[preg] = false;
}

void FPURegCache::OnRealize(preg_t preg, bool load, bool dirty, bool require_double,
                            bool want_single_pair)
{
  if (!m_state.m_guests_in_host_register[preg])
    return;
  if (!load)
  {
    m_state.m_guests_single_pair[preg] = false;
    m_state.m_guests_single_duplicated[preg] = false;
    m_state.m_guests_single_may_be_snan[preg] = false;
    m_state.m_guests_single_raw_load[preg] = false;
    return;
  }
  if (require_double)
  {
    ConvertToDouble(preg);
    return;
  }
  ASSERT_MSG(DYNA_REC, IsSingle(preg), "BindSingle read on non-single FPR {}", preg);
  if (want_single_pair && m_state.m_guests_single_duplicated[preg])
  {
    const X64Reg xr = m_state.m_guests_host_register[preg];
    if (cpu_info.bSSE3)
      m_emitter.MOVSLDUP(xr, ::Gen::R(xr));
    else
      m_emitter.SHUFPS(xr, ::Gen::R(xr), 0x00);
    m_state.m_guests_single_duplicated[preg] = false;
    m_state.m_guests_single_pair[preg] = true;
  }
}

void FPURegCache::NormalizePinnedRegister(preg_t preg)
{
  ConvertToDouble(preg);
}

void FPURegCache::StorePinnedToPPCStateNoMutate(BitSetGuest mask)
{
  for (const preg_t i : (mask & GetPinnedGuestRegisters()))
  {
    StoreRegister(i, GetPPCStateLocation(i), IgnoreDiscardedRegisters::No);
  }
}

void FPURegCache::LoadRegister(preg_t preg, X64Reg new_loc)
{
  ASSERT_MSG(DYNA_REC, m_state.m_guests_in_ppc_state[preg], "FPR {} not in PPCState", preg);
  m_emitter.MOVAPD(new_loc, GetPPCStateLocation(preg));
}

void FPURegCache::DiscardImm(preg_t preg)
{
  // FPURegCache doesn't support immediates, so no need to do anything
}

// Keep f0-f3 permanently resident across JIT blocks in XMM12-XMM15 (canonical double format
// [ps0, ps1] at block boundaries; callee-saved on Windows x64, saved across C++ calls on SysV).
static constexpr std::array<RegCache::PinnedReg, 4> PINNED_FPRS = {{
    {0, XMM12},
    {1, XMM13},
    {2, XMM14},
    {3, XMM15},
}};

static constexpr std::array<X64Reg, 10> ALLOCATION_ORDER = {
    XMM6, XMM7, XMM8, XMM9, XMM10, XMM11, XMM2, XMM3, XMM4, XMM5};

std::span<const RegCache::PinnedReg> FPURegCache::GetPinnedRegisters() const
{
  return PINNED_FPRS;
}

BitSetGuest FPURegCache::GetPinnedGuestRegisters() const
{
  constexpr auto result = [&]() {
    BitSetGuest fregs;
    for (const auto& [guest, host] : PINNED_FPRS)
      fregs[guest] = true;
    return fregs;
  }();
  return result;
}

Gen::X64Reg FPURegCache::GetPinnedHostRegister(preg_t preg) const
{
  for (const auto& [guest, host] : PINNED_FPRS)
  {
    if (guest == preg)
      return host;
  }
  return INVALID_REG;
}

BitSet32 FPURegCache::GetPinnedHostRegisters()
{
  constexpr auto result = [&]() {
    BitSet32 hregs;
    for (const auto& [guest, host] : PINNED_FPRS)
      hregs[host + 16] = true;
    return hregs;
  }();
  return result;
}

BitSet32 FPURegCache::GetPinnedCallerSavedHostRegisters()
{
  return GetPinnedHostRegisters() & ABI_ALL_CALLER_SAVED;
}

void FPURegCache::StorePinnedToPPCState(XEmitter& emitter, BitSetGuest mask)
{
  for (const auto& [guest, host] : PINNED_FPRS)
  {
    if (mask[guest])
      emitter.MOVAPD(PPCSTATE_PS0(guest), host);
  }
}

void FPURegCache::LoadPinnedFromPPCState(XEmitter& emitter, BitSetGuest mask)
{
  for (const auto& [guest, host] : PINNED_FPRS)
  {
    if (mask[guest])
      emitter.MOVAPD(host, PPCSTATE_PS0(guest));
  }
}

std::span<const X64Reg> FPURegCache::GetAllocationOrder() const
{
  return ALLOCATION_ORDER;
}

BitSetHost FPURegCache::GetAllocatableRegisters() const
{
  constexpr auto result = [&]() {
    BitSetHost hregs;
    for (auto r : ALLOCATION_ORDER)
    {
      hregs[r] = true;
    }
    return hregs;
  }();
  return result;
}

OpArg FPURegCache::GetPPCStateLocation(preg_t preg) const
{
  return PPCSTATE_PS0(preg);
}

BitSetGuest FPURegCache::GetRegUtilization() const
{
  return m_jit.js.op->fprInXmm;
}

BitSetGuest FPURegCache::CountRegsIn(preg_t preg, u32 lookahead) const
{
  BitSetGuest regs_used;

  for (u32 i = 1; i < lookahead; i++)
  {
    BitSetGuest regs_in = m_jit.js.op[i].fregsIn;
    regs_used |= regs_in;
    if (regs_in[preg])
      return regs_used;
  }

  return regs_used;
}
