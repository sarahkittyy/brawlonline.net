// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/Jit64/Jit.h"

#include <optional>

#include "Common/CPUDetect.h"
#include "Common/MsgHandler.h"
#include "Common/x64Emitter.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"

using namespace Gen;

void Jit64::ps_mr(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);

  int d = inst.FD;
  int b = inst.FB;
  if (d == b)
    return;

  if (fpr.IsSingle(b))
  {
    const bool is_pair = fpr.IsSinglePair(b);
    const bool may_be_snan = fpr.MayBeSNaN(b);
    const bool raw_load = fpr.IsRawSingleLoad(b);
    RCX64Reg Rb = fpr.BindSingle(b, RCMode::Read, is_pair);
    RCX64Reg Rd = fpr.BindSingle(d, RCMode::Write, is_pair);
    RegCache::Realize(Rb, Rd);
    MOVAPS(X64Reg(Rd), OpArg(Rb));
    fpr.SetSingle(d, is_pair && !may_be_snan, may_be_snan, raw_load);
    return;
  }

  RCOpArg Rb = fpr.Use(b, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RegCache::Realize(Rb, Rd);
  MOVAPD(Rd, Rb);
}

void Jit64::ps_sum(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  int d = inst.FD;
  int a = inst.FA;
  int b = inst.FB;
  int c = inst.FC;

  if (jo.accurateSinglePrecision && cpu_info.bSSE4_1 && fpr.IsSingle(a) && fpr.IsSingle(b) &&
      fpr.IsSingle(c) && !fpr.IsRawSingleLoad(c))
  {
    RCX64Reg Ra = fpr.BindSingle(a, RCMode::Read, /*want_pair=*/false);
    RCX64Reg Rb = fpr.BindSingle(b, RCMode::Read, /*want_pair=*/true);
    RCX64Reg Rc = fpr.BindSingle(c, RCMode::Read, /*want_pair=*/true);
    RCX64Reg Rd = fpr.BindSingle(d, RCMode::Write, /*want_pair=*/true);
    RegCache::Realize(Ra, Rb, Rc, Rd);

    MOVSHDUP(XMM1, Rb);  // {b.ps1, b.ps1}
    ADDSS(XMM1, Ra);     // {a.ps0 + b.ps1, ...}
    switch (inst.SUBOP5)
    {
    case 10:  // ps_sum0: {a.ps0 + b.ps1, c.ps1}
      if (d == c)
        MOVSS(Rd, R(XMM1));
      else if (cpu_info.bAVX)
        VBLENDPS(Rd, XMM1, Rc, 0b0010);
      else
      {
        MOVAPS(Rd, R(XMM1));
        BLENDPS(Rd, Rc, 0b0010);
      }
      break;
    case 11:  // ps_sum1: {c.ps0, a.ps0 + b.ps1}
      avx_op(&XEmitter::VUNPCKLPS, &XEmitter::UNPCKLPS, Rd, Rc, R(XMM1));
      break;
    default:
      PanicAlertFmt("ps_sum WTF!!!");
    }
    SetFPRFIfNeeded(R(Rd), true);
    fpr.SetSingle(d, /*is_pair=*/true, /*may_be_snan=*/false);
    return;
  }

  RCOpArg Ra = fpr.Use(a, RCMode::Read);
  RCOpArg Rb = fpr.Use(b, RCMode::Read);
  RCOpArg Rc = fpr.Use(c, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RegCache::Realize(Ra, Rb, Rc, Rd);

  X64Reg tmp = XMM1;
  MOVDDUP(tmp, Ra);  // {a.ps0, a.ps0}
  ADDPD(tmp, Rb);    // {a.ps0 + b.ps0, a.ps0 + b.ps1}
  switch (inst.SUBOP5)
  {
  case 10:  // ps_sum0: {a.ps0 + b.ps1, c.ps1}
    UNPCKHPD(tmp, Rc);
    break;
  case 11:  // ps_sum1: {c.ps0, a.ps0 + b.ps1}
    if (Rc.IsSimpleReg())
      MOVSD(tmp, Rc);
    else
      MOVLPD(tmp, Rc);
    break;
  default:
    PanicAlertFmt("ps_sum WTF!!!");
  }
  // We're intentionally not calling HandleNaNs here.
  // For addition and subtraction specifically, x86's NaN behavior matches PPC's.
  FinalizeSingleResult(d, Rd, R(tmp));
}

void Jit64::ps_muls(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  int d = inst.FD;
  int a = inst.FA;
  int c = inst.FC;

  if (jo.accurateSinglePrecision && cpu_info.bSSE3 && fpr.IsSingle(a) && fpr.IsSingle(c))
  {
    const bool preserve_d = m_accurate_nans && (a == d || c == d);
    RCX64Reg Ra = fpr.BindSingle(a, RCMode::Read, /*want_pair=*/true);
    RCX64Reg Rc = fpr.BindSingle(c, RCMode::Read, /*want_pair=*/true);
    RCX64Reg Rd = fpr.BindSingle(d, RCMode::Write, /*want_pair=*/true);
    RegCache::Realize(Ra, Rc, Rd);

    const X64Reg result_xmm = preserve_d ? XMM1 : X64Reg(Rd);
    RCX64Reg Rc_dup_guard;
    X64Reg dup_reg = XMM1;
    if (preserve_d)
    {
      Rc_dup_guard = fpr.Scratch();
      RegCache::Realize(Rc_dup_guard);
      dup_reg = X64Reg(Rc_dup_guard);
    }

    switch (inst.SUBOP5)
    {
    case 12:  // ps_muls0
      MOVSLDUP(dup_reg, Rc);
      break;
    case 13:  // ps_muls1
      MOVSHDUP(dup_reg, Rc);
      break;
    default:
      PanicAlertFmt("ps_muls WTF!!!");
    }

    if (cpu_info.bAVX)
    {
      VMULPS(result_xmm, dup_reg, Ra);
    }
    else if (result_xmm == X64Reg(Ra))
    {
      MULPS(result_xmm, R(dup_reg));
    }
    else
    {
      if (result_xmm != dup_reg)
        MOVAPS(result_xmm, R(dup_reg));
      MULPS(result_xmm, Ra);
    }

    if (m_accurate_nans)
    {
      const FixupBranch handled_nans =
          HandleNaNsSingle(inst, result_xmm, XMM0, Ra, std::nullopt, R(dup_reg));
      SetJumpTarget(handled_nans);
    }

    if (result_xmm != X64Reg(Rd))
      MOVAPS(Rd, R(result_xmm));
    SetFPRFIfNeeded(R(Rd), true);
    fpr.SetSingle(d, /*is_pair=*/true, /*may_be_snan=*/false);
    return;
  }

  bool round_input = !js.op->fprIsSingle[c];

  RCOpArg Ra = fpr.Use(a, RCMode::Read);
  RCOpArg Rc = fpr.Use(c, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RCX64Reg Rc_duplicated = m_accurate_nans ? fpr.Scratch() : fpr.Scratch(XMM1);
  RegCache::Realize(Ra, Rc, Rd, Rc_duplicated);

  switch (inst.SUBOP5)
  {
  case 12:  // ps_muls0
    MOVDDUP(Rc_duplicated, Rc);
    break;
  case 13:  // ps_muls1
    avx_op(&XEmitter::VSHUFPD, &XEmitter::SHUFPD, Rc_duplicated, Rc, Rc, 3);
    break;
  default:
    PanicAlertFmt("ps_muls WTF!!!");
  }

  if (round_input)
    Force25BitPrecision(XMM1, R(Rc_duplicated), XMM0);
  else if (XMM1 != Rc_duplicated)
    MOVAPD(XMM1, Rc_duplicated);
  MULPD(XMM1, Ra);

  if (m_accurate_nans)
  {
    const FixupBranch handled_nans = HandleNaNs(inst, XMM1, XMM0, Ra, std::nullopt, Rc_duplicated);
    SetJumpTarget(handled_nans);
  }

  FinalizeSingleResult(d, Rd, R(XMM1));
}

void Jit64::ps_mergeXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);

  int d = inst.FD;
  int a = inst.FA;
  int b = inst.FB;

  RCOpArg Ra = fpr.Use(a, RCMode::Read);
  RCOpArg Rb = fpr.Use(b, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RegCache::Realize(Ra, Rb, Rd);

  switch (inst.SUBOP10)
  {
  case 528:
    avx_op(&XEmitter::VUNPCKLPD, &XEmitter::UNPCKLPD, Rd, Ra, Rb);
    break;  // 00
  case 560:
    if (d != b)
      avx_op(&XEmitter::VSHUFPD, &XEmitter::SHUFPD, Rd, Ra, Rb, 2);
    else if (Ra.IsSimpleReg())
      MOVSD(Rd, Ra);
    else
      MOVLPD(Rd, Ra);
    break;  // 01
  case 592:
    avx_op(&XEmitter::VSHUFPD, &XEmitter::SHUFPD, Rd, Ra, Rb, 1);
    break;  // 10
  case 624:
    avx_op(&XEmitter::VUNPCKHPD, &XEmitter::UNPCKHPD, Rd, Ra, Rb);
    break;  // 11
  default:
    ASSERT_MSG(DYNA_REC, 0, "ps_merge - invalid op");
  }
}

void Jit64::ps_rsqrte(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || jo.div_by_zero_exceptions);
  int b = inst.FB;
  int d = inst.FD;

  RCX64Reg scratch_guard = gpr.Scratch(RSCRATCH_EXTRA);
  RCX64Reg Rb = fpr.Bind(b, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RegCache::Realize(scratch_guard, Rb, Rd);

  MOVSD(XMM0, Rb);
  CALL(asm_routines.frsqrte);
  MOVSD(Rd, XMM0);

  MOVHLPS(XMM0, Rb);
  CALL(asm_routines.frsqrte);
  MOVLHPS(Rd, XMM0);

  FinalizeSingleResult(d, Rd, Rd);
}

void Jit64::ps_res(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || jo.div_by_zero_exceptions);
  int b = inst.FB;
  int d = inst.FD;

  RCX64Reg scratch_guard = gpr.Scratch(RSCRATCH_EXTRA);
  RCX64Reg Rb = fpr.Bind(b, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RegCache::Realize(scratch_guard, Rb, Rd);

  MOVSD(XMM0, Rb);
  CALL(asm_routines.fres);
  MOVSD(Rd, XMM0);

  MOVHLPS(XMM0, Rb);
  CALL(asm_routines.fres);
  MOVLHPS(Rd, XMM0);

  FinalizeSingleResult(d, Rd, Rd);
}

void Jit64::ps_cmpXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(jo.fp_exceptions);

  FloatCompare(inst, !!(inst.SUBOP10 & 64));
}
