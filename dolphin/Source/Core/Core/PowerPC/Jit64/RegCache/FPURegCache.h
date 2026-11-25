// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"

class Jit64;

class FPURegCache final : public RegCache
{
public:
  explicit FPURegCache(Jit64& jit, Gen::XEmitter& emitter);

  bool IsImm(preg_t preg) const override;
  u32 Imm32(preg_t preg) const override;
  s32 SImm32(preg_t preg) const override;

  bool IsSingle(preg_t preg) const
  {
    return m_state.m_guests_in_host_register[preg] &&
           (m_state.m_guests_single_pair[preg] || m_state.m_guests_single_duplicated[preg]);
  }
  bool IsSinglePair(preg_t preg) const
  {
    return m_state.m_guests_in_host_register[preg] && m_state.m_guests_single_pair[preg];
  }
  bool IsSingleDuplicated(preg_t preg) const
  {
    return m_state.m_guests_in_host_register[preg] && m_state.m_guests_single_duplicated[preg];
  }
  bool MayBeSNaN(preg_t preg) const
  {
    return m_state.m_guests_in_host_register[preg] && m_state.m_guests_single_may_be_snan[preg];
  }
  bool IsRawSingleLoad(preg_t preg) const
  {
    return m_state.m_guests_in_host_register[preg] && m_state.m_guests_single_raw_load[preg];
  }

  RCX64Reg BindSingle(preg_t preg, RCMode mode, bool want_pair)
  {
    m_guests_constraints.AddBindSingle(preg, mode, want_pair);
    return RCX64Reg{this, preg};
  }
  RCX64Reg RevertableBindSingle(preg_t preg, RCMode mode)
  {
    m_guests_constraints.AddRevertableBindSingle(preg, mode);
    return RCX64Reg{this, preg};
  }
  void SetSingle(preg_t preg, bool is_pair, bool may_be_snan = false, bool raw_load = false);
  void ConvertToDouble(preg_t preg);

  std::span<const PinnedReg> GetPinnedRegisters() const override;
  BitSetGuest GetPinnedGuestRegisters() const override;
  Gen::X64Reg GetPinnedHostRegister(preg_t preg) const override;
  static BitSet32 GetPinnedHostRegisters();
  static BitSet32 GetPinnedCallerSavedHostRegisters();
  static void StorePinnedToPPCState(Gen::XEmitter& emitter,
                                    BitSetGuest mask = BitSetGuest::AllTrue());
  static void LoadPinnedFromPPCState(Gen::XEmitter& emitter,
                                     BitSetGuest mask = BitSetGuest::AllTrue());
  void StorePinnedToPPCStateNoMutate(BitSetGuest mask);

protected:
  Gen::OpArg R(preg_t preg) const override;
  Gen::OpArg GetPPCStateLocation(preg_t preg) const override;
  void StoreRegister(preg_t preg, const Gen::OpArg& newLoc,
                     IgnoreDiscardedRegisters ignore_discarded_registers) override;
  void LoadRegister(preg_t preg, Gen::X64Reg newLoc) override;
  void DiscardImm(preg_t preg) override;
  void OnRealize(preg_t preg, bool load, bool dirty, bool require_double,
                 bool want_single_pair) override;
  void NormalizePinnedRegister(preg_t preg) override;
  std::span<const Gen::X64Reg> GetAllocationOrder() const override;
  BitSetHost GetAllocatableRegisters() const override;
  BitSetGuest GetRegUtilization() const override;
  BitSetGuest CountRegsIn(preg_t preg, u32 lookahead) const override;
};
