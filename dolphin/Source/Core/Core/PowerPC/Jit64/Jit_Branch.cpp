// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/Jit64/Jit.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"
#include "Core/Debugger/BranchWatch.h"
#include "Core/HLE/HLE.h"
#include "Core/PowerPC/BranchProfile.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

// The branches are known good, or at least reasonably good.
// No need for a disable-mechanism.

// If defined, clears CR0 at blr and bl-s. If the assumption that
// flags never carry over between functions holds, then the task for
// an optimizer becomes much easier.

// #define ACID_TEST

// Zelda and many more games seem to pass the Acid Test.

using namespace Gen;

void Jit64::sc(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITBranchOff);

  gpr.Flush();
  fpr.Flush();
  MOV(32, PPCSTATE(pc), Imm32(js.compilerPC + 4));
  LOCK();
  OR(32, PPCSTATE(Exceptions), Imm32(EXCEPTION_SYSCALL));
  WriteExceptionExit();
}

void Jit64::rfi(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITBranchOff);

  gpr.Flush();
  fpr.Flush();

  // See Interpreter rfi for details
  const u32 mask = 0x87C0FFFF;
  const u32 clearMSR13 = 0xFFFBFFFF;  // Mask used to clear the bit MSR[13]
  // MSR = ((MSR & ~mask) | (SRR1 & mask)) & clearMSR13;
  AND(32, PPCSTATE(msr), Imm32((~mask) & clearMSR13));
  MOV(32, R(RSCRATCH), PPCSTATE_SRR1);
  AND(32, R(RSCRATCH), Imm32(mask & clearMSR13));
  OR(32, PPCSTATE(msr), R(RSCRATCH));

  // Call MSRUpdated to update feature_flags. Only the bits that come from SRR1
  // are relevant for this, so it's fine to pass in RSCRATCH in place of msr.
  MSRUpdated(R(RSCRATCH), RSCRATCH2);

  // NPC = SRR0;
  MOV(32, R(RSCRATCH), PPCSTATE_SRR0);
  WriteRfiExitDestInRSCRATCH();
}

template <bool condition>
void Jit64::WriteBranchWatch(u32 origin, u32 destination, UGeckoInstruction inst,
                             BitSet32 caller_save)
{
  if (IsBranchWatchEnabled())
  {
    const BitSet32 regs = caller_save | FPURegCache::GetPinnedCallerSavedHostRegisters();
    ABI_PushRegistersAndAdjustStack(regs, 0);
    MOV(64, R(ABI_PARAM1), ImmPtr(&m_branch_watch));
    MOV(64, R(ABI_PARAM2), Imm64(Core::FakeBranchWatchCollectionKey{origin, destination}));
    MOV(32, R(ABI_PARAM3), Imm32(inst.hex));
    ABI_CallFunction(m_ppc_state.msr.IR ? (condition ? &Core::BranchWatch::HitVirtualTrue_fk :
                                                       &Core::BranchWatch::HitVirtualFalse_fk) :
                                          (condition ? &Core::BranchWatch::HitPhysicalTrue_fk :
                                                       &Core::BranchWatch::HitPhysicalFalse_fk));
    ABI_PopRegistersAndAdjustStack(regs, 0);
  }
}

template void Jit64::WriteBranchWatch<true>(u32, u32, UGeckoInstruction, BitSet32);
template void Jit64::WriteBranchWatch<false>(u32, u32, UGeckoInstruction, BitSet32);

void Jit64::WriteBranchWatchDestInRSCRATCH(u32 origin, UGeckoInstruction inst, BitSet32 caller_save)
{
  if (IsBranchWatchEnabled())
  {
    // Assert RSCRATCH won't be clobbered before it is moved from.
    static_assert(ABI_PARAM1 != RSCRATCH);

    const BitSet32 regs = caller_save | FPURegCache::GetPinnedCallerSavedHostRegisters();
    ABI_PushRegistersAndAdjustStack(regs, 0);
    MOV(64, R(ABI_PARAM1), ImmPtr(&m_branch_watch));
    MOV(32, R(ABI_PARAM3), R(RSCRATCH));
    MOV(32, R(ABI_PARAM2), Imm32(origin));
    MOV(32, R(ABI_PARAM4), Imm32(inst.hex));
    ABI_CallFunction(m_ppc_state.msr.IR ? &Core::BranchWatch::HitVirtualTrue :
                                          &Core::BranchWatch::HitPhysicalTrue);
    ABI_PopRegistersAndAdjustStack(regs, 0);
  }
}

void Jit64::WriteNotTakenExit(const FixupBranch& not_taken, u32 branch_address,
                              UGeckoInstruction inst)
{
  // The analyzer went on at the branch's target (BranchProfile.h), so the code that follows is
  // the taken side's. Not taken is the rare side: it leaves the block from the far code.
  SwitchToFarCode();
  SetJumpTarget(not_taken);
  {
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();
    gpr.Flush();
    fpr.Flush();
    WriteBranchWatch<false>(branch_address, branch_address + 4, inst, {});
    WriteExit(branch_address + 4);
  }
  SwitchToNearCode();
}

void Jit64::CountBranch(u32* counter)
{
  MOV(64, R(RSCRATCH), ImmPtr(counter));
  ADD(32, MatR(RSCRATCH), Imm8(1));
}

void Jit64::bx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITBranchOff);

  // We must always process the following sentence
  // even if the blocks are merged by PPCAnalyst::Flatten().
  if (inst.LK)
    MOV(32, PPCSTATE_LR, Imm32(js.compilerPC + 4));

  // If this is not the last instruction of a block,
  // we will skip the rest process.
  // Because PPCAnalyst::Flatten() merged the blocks.
  if (!js.isLastInstruction)
  {
    WriteBranchWatch<true>(js.compilerPC, js.op->branchTo, inst, CallerSavedRegistersInUse());
    if (inst.LK && !js.op->skipLRStack)
    {
      // We have to fake the stack as the RET instruction was not
      // found in the same block. This is a big overhead, but still
      // better than calling the dispatcher.
      FakeBLCall(js.compilerPC + 4);
    }
    return;
  }

  gpr.Flush();
  fpr.Flush();

  WriteBranchWatch<true>(js.compilerPC, js.op->branchTo, inst, {});
#ifdef ACID_TEST
  if (inst.LK)
    AND(32, PPCSTATE(cr), Imm32(~(0xFF000000)));
#endif
  if (js.op->branchIsIdleLoop)
  {
    WriteIdleExit(js.op->branchTo);
  }
  else
  {
    WriteExit(js.op->branchTo, inst.LK, js.compilerPC + 4);
  }
}

// TODO - optimize to hell and beyond
// TODO - make nice easy to optimize special cases for the most common
// variants of this instruction.
void Jit64::bcx(UGeckoInstruction inst)
{
  INSTRUCTION_START

  // Ahead of JITDISABLE: the instructions after this one are the taken side's, and a fallback to
  // the interpreter goes on in the block when the branch is not taken.
  if (js.op->followTaken)
  {
    WriteNotTakenExit(
        JumpIfCRFieldBit(inst.BI >> 2, 3 - (inst.BI & 3), !(inst.BO_2 & BO_BRANCH_IF_TRUE)),
        js.compilerPC, inst);
    WriteBranchWatch<true>(js.compilerPC, js.op->branchTo, inst, CallerSavedRegistersInUse());
    return;
  }

  JITDISABLE(bJITBranchOff);

  if (TryCompileBranchlessDiamond(0))
    return;

  // USES_CR

  FixupBranch pCTRDontBranch;
  if ((inst.BO & BO_DONT_DECREMENT_FLAG) == 0)  // Decrement and test CTR
  {
    SUB(32, PPCSTATE_CTR, Imm8(1));
    if (inst.BO & BO_BRANCH_IF_CTR_0)
      pCTRDontBranch = J_CC(CC_NZ, Jump::Near);
    else
      pCTRDontBranch = J_CC(CC_Z, Jump::Near);
  }

  FixupBranch pConditionDontBranch;
  if ((inst.BO & BO_DONT_CHECK_CONDITION) == 0)  // Test a CR bit
  {
    pConditionDontBranch =
        JumpIfCRFieldBit(inst.BI >> 2, 3 - (inst.BI & 3), !(inst.BO_2 & BO_BRANCH_IF_TRUE));
  }

  if (inst.LK)
    MOV(32, PPCSTATE_LR, Imm32(js.compilerPC + 4));

  // If this is not the last instruction of a block
  // and an unconditional branch, we will skip the rest process.
  // Because PPCAnalyst::Flatten() merged the blocks.
  if (!js.isLastInstruction && (inst.BO & BO_DONT_DECREMENT_FLAG) &&
      (inst.BO & BO_DONT_CHECK_CONDITION))
  {
    WriteBranchWatch<true>(js.compilerPC, js.op->branchTo, inst, CallerSavedRegistersInUse());
    if (inst.LK && !js.op->skipLRStack)
    {
      // We have to fake the stack as the RET instruction was not
      // found in the same block. This is a big overhead, but still
      // better than calling the dispatcher.
      FakeBLCall(js.compilerPC + 4);
    }
    return;
  }

  {
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();
    gpr.Flush();
    fpr.Flush();

    if (js.op->branchCounts)
      CountBranch(&js.op->branchCounts->taken);
    WriteBranchWatch<true>(js.compilerPC, js.op->branchTo, inst, {});
    if (js.op->branchIsIdleLoop)
    {
      WriteIdleExit(js.op->branchTo);
    }
    else
    {
      WriteExit(js.op->branchTo, inst.LK, js.compilerPC + 4);
    }
  }

  if ((inst.BO & BO_DONT_CHECK_CONDITION) == 0)
    SetJumpTarget(pConditionDontBranch);
  if ((inst.BO & BO_DONT_DECREMENT_FLAG) == 0)
    SetJumpTarget(pCTRDontBranch);
  if (js.op->branchCounts)
    CountBranch(&js.op->branchCounts->not_taken);

  if (!analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE))
  {
    gpr.Flush();
    fpr.Flush();
    WriteBranchWatch<false>(js.compilerPC, js.compilerPC + 4, inst, {});
    WriteExit(js.compilerPC + 4);
  }
  WriteBranchWatch<false>(js.compilerPC, js.compilerPC + 4, inst, CallerSavedRegistersInUse());
}

void Jit64::bcctrx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITBranchOff);

  // bcctrx doesn't decrement and/or test CTR
  DEBUG_ASSERT_MSG(POWERPC, inst.BO_2 & BO_DONT_DECREMENT_FLAG,
                   "bcctrx with decrement and test CTR option is invalid!");

  if (inst.BO_2 & BO_DONT_CHECK_CONDITION)
  {
    // BO_2 == 1z1zz -> b always

    // NPC = CTR & 0xfffffffc;
    gpr.Flush();
    fpr.Flush();

    MOV(32, R(RSCRATCH), PPCSTATE_CTR);
    if (inst.LK_3)
      MOV(32, PPCSTATE_LR, Imm32(js.compilerPC + 4));  // LR = PC + 4;
    AND(32, R(RSCRATCH), Imm32(0xFFFFFFFC));
    WriteBranchWatchDestInRSCRATCH(js.compilerPC, inst, BitSet32{RSCRATCH});
    WriteIndirectExit(inst.LK_3, js.compilerPC + 4);
  }
  else
  {
    // Rare condition seen in (just some versions of?) Nintendo's NES Emulator

    // BO_2 == 001zy -> b if false
    // BO_2 == 011zy -> b if true

    FixupBranch b =
        JumpIfCRFieldBit(inst.BI >> 2, 3 - (inst.BI & 3), !(inst.BO_2 & BO_BRANCH_IF_TRUE));
    MOV(32, R(RSCRATCH), PPCSTATE_CTR);
    AND(32, R(RSCRATCH), Imm32(0xFFFFFFFC));
    // MOV(32, PPCSTATE(pc), R(RSCRATCH)); => Already done in WriteExitDestInRSCRATCH()
    if (inst.LK_3)
      MOV(32, PPCSTATE_LR, Imm32(js.compilerPC + 4));  // LR = PC + 4;

    {
      RCForkGuard gpr_guard = gpr.Fork();
      RCForkGuard fpr_guard = fpr.Fork();
      gpr.Flush();
      fpr.Flush();
      WriteBranchWatchDestInRSCRATCH(js.compilerPC, inst, BitSet32{RSCRATCH});
      WriteExitDestInRSCRATCH(inst.LK_3, js.compilerPC + 4);
      // Would really like to continue the block here, but it ends. TODO.
    }
    SetJumpTarget(b);

    if (!analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE))
    {
      gpr.Flush();
      fpr.Flush();
      WriteBranchWatch<false>(js.compilerPC, js.compilerPC + 4, inst, {});
      WriteExit(js.compilerPC + 4);
    }
    WriteBranchWatch<false>(js.compilerPC, js.compilerPC + 4, inst, CallerSavedRegistersInUse());
  }
}

void Jit64::bclrx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITBranchOff);

  FixupBranch pCTRDontBranch;
  if ((inst.BO & BO_DONT_DECREMENT_FLAG) == 0)  // Decrement and test CTR
  {
    SUB(32, PPCSTATE_CTR, Imm8(1));
    if (inst.BO & BO_BRANCH_IF_CTR_0)
      pCTRDontBranch = J_CC(CC_NZ, Jump::Near);
    else
      pCTRDontBranch = J_CC(CC_Z, Jump::Near);
  }

  FixupBranch pConditionDontBranch;
  if ((inst.BO & BO_DONT_CHECK_CONDITION) == 0)  // Test a CR bit
  {
    pConditionDontBranch =
        JumpIfCRFieldBit(inst.BI >> 2, 3 - (inst.BI & 3), !(inst.BO_2 & BO_BRANCH_IF_TRUE));
  }

// This below line can be used to prove that blr "eats flags" in practice.
// This observation could let us do some useful optimizations.
#ifdef ACID_TEST
  AND(32, PPCSTATE(cr), Imm32(~(0xFF000000)));
#endif

  MOV(32, R(RSCRATCH), PPCSTATE_LR);
  // We don't have to do this because WriteBLRExit handles it for us. Specifically, since we only
  // ever push divisible-by-four instruction addresses onto the stack, if the return address
  // matches, we're already good. If it doesn't match, the mispredicted-BLR code handles the fixup.
  if (!m_enable_blr_optimization)
    AND(32, R(RSCRATCH), Imm32(0xFFFFFFFC));
  if (inst.LK)
    MOV(32, PPCSTATE_LR, Imm32(js.compilerPC + 4));

  {
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();
    gpr.Flush();
    fpr.Flush();

    if (js.op->branchIsIdleLoop)
    {
      WriteBranchWatch<true>(js.compilerPC, js.op->branchTo, inst, {});
      WriteIdleExit(js.op->branchTo);
    }
    else
    {
      WriteBranchWatchDestInRSCRATCH(js.compilerPC, inst, BitSet32{RSCRATCH});
      WriteBLRExit();
    }
  }

  if ((inst.BO & BO_DONT_CHECK_CONDITION) == 0)
    SetJumpTarget(pConditionDontBranch);
  if ((inst.BO & BO_DONT_DECREMENT_FLAG) == 0)
    SetJumpTarget(pCTRDontBranch);

  if (!analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE))
  {
    gpr.Flush();
    fpr.Flush();
    WriteBranchWatch<false>(js.compilerPC, js.compilerPC + 4, inst, {});
    WriteExit(js.compilerPC + 4);
  }
  else
  {
    WriteBranchWatch<false>(js.compilerPC, js.compilerPC + 4, inst, CallerSavedRegistersInUse());
  }
}

namespace
{
enum class DiamondOp
{
  AddImm,
  OrImm,
  XorImm,
  MovReg,
  NegReg,
  AddReg,
  SubReg,
  OrReg,
  AndReg,
  XorReg,
};

struct DiamondBody
{
  DiamondOp kind = DiamondOp::AddImm;
  int dst = -1;
  int src1 = -1;  // -1 means constant 0 (for li / lis / lis+addi / lis+ori)
  int src2 = -1;
  u32 imm = 0;
  u32 body_cycles = 0;
};

bool DecodeDiamondBody(const PPCAnalyst::CodeOp* ops, int n, DiamondBody* out)
{
  if (n == 2)
  {
    const UGeckoInstruction op1 = ops[0].inst;
    const UGeckoInstruction op2 = ops[1].inst;
    // addis/lis rD, rA, H; addi rD, rD, L (rD != 0 since addi with rA == 0 is li)
    if (op1.OPCD == 15 && op1.RD != 0 && op2.OPCD == 14 && op2.RD == op1.RD && op2.RA == op1.RD)
    {
      const u32 hi = static_cast<u32>(static_cast<u16>(op1.SIMM_16)) << 16;
      const u32 lo = static_cast<u32>(static_cast<s32>(static_cast<s16>(op2.SIMM_16)));
      *out = {DiamondOp::AddImm,
              static_cast<int>(op1.RD),
              op1.RA ? static_cast<int>(op1.RA) : -1,
              -1,
              hi + lo,
              static_cast<u32>(ops[0].opinfo->num_cycles + ops[1].opinfo->num_cycles)};
      return true;
    }
    // lis rD, H; ori rD, rD, L
    if (op1.OPCD == 15 && op1.RA == 0 && op2.OPCD == 24 && op2.RA == op1.RD && op2.RS == op1.RD)
    {
      const u32 hi = static_cast<u32>(static_cast<u16>(op1.SIMM_16)) << 16;
      const u32 lo = static_cast<u32>(op2.UIMM);
      *out = {DiamondOp::AddImm,
              static_cast<int>(op1.RD),
              -1,
              -1,
              hi | lo,
              static_cast<u32>(ops[0].opinfo->num_cycles + ops[1].opinfo->num_cycles)};
      return true;
    }
    return false;
  }

  if (n != 1)
    return false;

  const UGeckoInstruction inst = ops[0].inst;
  const u32 cycles = static_cast<u32>(ops[0].opinfo->num_cycles);
  switch (inst.OPCD)
  {
  case 14:  // addi / li
    *out = {DiamondOp::AddImm, static_cast<int>(inst.RD),
            inst.RA ? static_cast<int>(inst.RA) : -1, -1,
            static_cast<u32>(static_cast<s32>(static_cast<s16>(inst.SIMM_16))), cycles};
    return true;
  case 15:  // addis / lis
    *out = {DiamondOp::AddImm, static_cast<int>(inst.RD),
            inst.RA ? static_cast<int>(inst.RA) : -1, -1,
            static_cast<u32>(static_cast<u16>(inst.SIMM_16)) << 16, cycles};
    return true;
  case 24:  // ori
    *out = {DiamondOp::OrImm, static_cast<int>(inst.RA), static_cast<int>(inst.RS), -1,
            static_cast<u32>(inst.UIMM), cycles};
    return true;
  case 25:  // oris
    *out = {DiamondOp::OrImm, static_cast<int>(inst.RA), static_cast<int>(inst.RS), -1,
            static_cast<u32>(inst.UIMM) << 16, cycles};
    return true;
  case 26:  // xori
    *out = {DiamondOp::XorImm, static_cast<int>(inst.RA), static_cast<int>(inst.RS), -1,
            static_cast<u32>(inst.UIMM), cycles};
    return true;
  case 27:  // xoris
    *out = {DiamondOp::XorImm, static_cast<int>(inst.RA), static_cast<int>(inst.RS), -1,
            static_cast<u32>(inst.UIMM) << 16, cycles};
    return true;
  case 31:
    if (inst.Rc)
      return false;
    switch (inst.SUBOP10)
    {
    case 266:  // add
      if (inst.OE)
        return false;
      *out = {DiamondOp::AddReg, static_cast<int>(inst.RD), static_cast<int>(inst.RA),
              static_cast<int>(inst.RB), 0, cycles};
      return true;
    case 40:  // subf rD, rA, rB => rB - rA
      if (inst.OE)
        return false;
      *out = {DiamondOp::SubReg, static_cast<int>(inst.RD), static_cast<int>(inst.RB),
              static_cast<int>(inst.RA), 0, cycles};
      return true;
    case 104:  // neg
      if (inst.OE)
        return false;
      *out = {DiamondOp::NegReg, static_cast<int>(inst.RD), static_cast<int>(inst.RA), -1, 0,
              cycles};
      return true;
    case 444:  // or / mr
      if (inst.RS == inst.RB)
      {
        *out = {DiamondOp::MovReg, static_cast<int>(inst.RA), static_cast<int>(inst.RS), -1, 0,
                cycles};
      }
      else
      {
        *out = {DiamondOp::OrReg, static_cast<int>(inst.RA), static_cast<int>(inst.RS),
                static_cast<int>(inst.RB), 0, cycles};
      }
      return true;
    case 28:  // and
      *out = {DiamondOp::AndReg, static_cast<int>(inst.RA), static_cast<int>(inst.RS),
              static_cast<int>(inst.RB), 0, cycles};
      return true;
    case 316:  // xor
      *out = {DiamondOp::XorReg, static_cast<int>(inst.RA), static_cast<int>(inst.RS),
              static_cast<int>(inst.RB), 0, cycles};
      return true;
    default:
      return false;
    }
  default:
    return false;
  }
}
bool HasPriorFollowedBranch(const PPCAnalyst::CodeOp* code, u32 limit)
{
  for (u32 idx = 0; idx < limit; ++idx)
  {
    const UGeckoInstruction op = code[idx].inst;
    if (op.OPCD == 18 ||
        (op.OPCD == 16 && (op.BO & BO_DONT_DECREMENT_FLAG) &&
         (op.BO & BO_DONT_CHECK_CONDITION)) ||
        (op.OPCD == 19 && op.SUBOP10 == 16) || code[idx].followTaken)
    {
      return true;
    }
  }
  return false;
}
}  // namespace

bool Jit64::TryCompileBranchlessDiamond(int branch_offset)
{
  if (!analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE) ||
      IsBranchWatchEnabled() || bJITIntegerOff || bJITRegisterCacheOff || code_block.m_broken)
  {
    return false;
  }

  const u32 cur_idx = static_cast<u32>(js.op - m_code_buffer.data());
  if (HasPriorFollowedBranch(m_code_buffer.data(), cur_idx + static_cast<u32>(branch_offset)))
    return false;

  const PPCAnalyst::CodeOp& branch_op = js.op[branch_offset];
  const UGeckoInstruction& binst = branch_op.inst;
  if (binst.OPCD != 16 || binst.LK || !(binst.BO & BO_DONT_DECREMENT_FLAG) ||
      (binst.BO & BO_DONT_CHECK_CONDITION) || branch_op.followTaken ||
      branch_op.branchIsIdleLoop || branch_op.branchCounts != nullptr)
  {
    return false;
  }

  const u32 target = branch_op.branchTo;
  const u32 diff = target - branch_op.address;
  if (diff != 8 && diff != 12 && diff != 16)
    return false;
  const int n = static_cast<int>(diff / 4) - 1;  // 1, 2, or 3 skipped ops

  if (!CanMergeNextInstructions(branch_offset + n) ||
      js.instructionsLeft < branch_offset + n + 1 ||
      js.op[branch_offset + n + 1].address != target)
  {
    return false;
  }

  // Check if the final skipped op is a non-updating D-form integer store (stw/sth/stb).
  int store_access_size = 0;
  const PPCAnalyst::CodeOp& last_sop = js.op[branch_offset + n];
  const bool can_store_diamond = !jo.memcheck && jo.fastmem && !bJITLoadStoreOff &&
                                 !m_system.GetPowerPC().GetMemChecks().HasAny();
  if (can_store_diamond && last_sop.inst.RA != 0 && !gpr.IsImm(last_sop.inst.RA))
  {
    switch (last_sop.inst.OPCD)
    {
    case 36:  // stw
      store_access_size = 32;
      break;
    case 44:  // sth
      store_access_size = 16;
      break;
    case 38:  // stb
      store_access_size = 8;
      break;
    default:
      break;
    }
  }

  const int alu_count = store_access_size != 0 ? (n - 1) : n;
  if (alu_count > 2)
    return false;

  for (int k = 1; k <= n; ++k)
  {
    const PPCAnalyst::CodeOp& sop = js.op[branch_offset + k];
    const u32 sop_idx = cur_idx + static_cast<u32>(branch_offset + k);
    const bool is_final_store = store_access_size != 0 && k == n;
    if (sop.address != branch_op.address + static_cast<u32>(k * 4) || sop.skip || sop.canEndBlock ||
        (!is_final_store && sop.canCauseException) || sop.crOut || sop.outputCA || sop.outputFPRF ||
        HLE::TryReplaceFunction(m_ppc_symbol_db, sop.address, PowerPC::CoreMode::JIT) ||
        (sop_idx < m_dirty_mark_plan.size() &&
         m_dirty_mark_plan[sop_idx].kind == DirtyMarkPlan::Kind::Leader))
    {
      return false;
    }
  }

  DiamondBody body;
  const bool has_alu = alu_count > 0;
  if (has_alu && !DecodeDiamondBody(&js.op[branch_offset + 1], alu_count, &body))
    return false;

  const int crf = binst.BI >> 2;
  const int bit = 3 - (binst.BI & 3);
  const bool branch_if_true = !!(binst.BO_2 & BO_BRANCH_IF_TRUE);

  // Preserve exact downcount-expiry timing on the taken path: if downcount would expire at this
  // branch (<= js.downcountAmount, rare), bail to far code and exit to `target` via WriteExit when
  // taken, matching an unconverted bcx to the exact cycle for cross-build determinism.
  if (m_far_code.Enabled() && !m_far_code.IsInSpace(GetCodePtr()))
  {
    if (js.downcountAmount <= 127)
      CMP(32, PPCSTATE(downcount), Imm8(static_cast<s8>(js.downcountAmount)));
    else
      CMP(32, PPCSTATE(downcount), Imm32(js.downcountAmount));
    FixupBranch maybe_expire = J_CC(CC_LE, Jump::Near);
    SwitchToFarCode();
    SetJumpTarget(maybe_expire);
    FixupBranch not_taken_far = JumpIfCRFieldBit(crf, bit, !branch_if_true);
    {
      RCForkGuard gpr_guard = gpr.Fork();
      RCForkGuard fpr_guard = fpr.Fork();
      gpr.Flush();
      fpr.Flush();
      WriteExit(target);
    }
    SetJumpTarget(not_taken_far);
    FixupBranch resume = J(Jump::Near);
    SwitchToNearCode();
    SetJumpTarget(resume);
  }

  // Local forward store diamond: [optional 1..2 ALU ops] + non-updating stw/sth/stb.
  // Keep all registers in host registers across the diamond (zero block exit / flush / reload)
  // while never executing the store speculatively.
  if (store_access_size != 0)
  {
    const UGeckoInstruction st = last_sop.inst;
    const int st_base = static_cast<int>(st.RA);
    const int st_val = static_cast<int>(st.RS);
    const s32 st_disp = static_cast<s16>(st.SIMM_16);
    const u32 total_cycles =
        (has_alu ? body.body_cycles : 0u) + static_cast<u32>(last_sop.opinfo->num_cycles);

    // Pre-realize every register touched on the body path BEFORE the local branch so both paths
    // arrive at the join point with identical GPRRegCache state.
    RCX64Reg Rd;
    RCOpArg R1, R2, Rbase, Rstval;
    bool need_rd = false, need_r1 = false, need_r2 = false, need_base = false, need_stval = false;
    if (has_alu)
    {
      need_rd = true;
      Rd = gpr.Bind(body.dst, RCMode::ReadWrite);
      if (body.src1 >= 0 && body.src1 != body.dst)
      {
        need_r1 = true;
        R1 = gpr.Use(body.src1, RCMode::Read);
      }
      if (body.src2 >= 0 && body.src2 != body.dst && body.src2 != body.src1)
      {
        need_r2 = true;
        R2 = gpr.Use(body.src2, RCMode::Read);
      }
    }
    if (!need_rd || st_base != body.dst)
    {
      if ((!need_r1 || st_base != body.src1) && (!need_r2 || st_base != body.src2))
      {
        need_base = true;
        Rbase = gpr.Use(st_base, RCMode::Read);
      }
    }
    if ((!need_rd || st_val != body.dst) && (!need_r1 || st_val != body.src1) &&
        (!need_r2 || st_val != body.src2) && (!need_base || st_val != st_base))
    {
      need_stval = true;
      Rstval = gpr.Use(st_val, RCMode::Read);
    }

    // Realize all bound/used registers before the branch.
    auto realize_all = [&] {
      if (need_rd)
        Rd.Realize();
      if (need_r1)
        R1.Realize();
      if (need_r2)
        R2.Realize();
      if (need_base)
        Rbase.Realize();
      if (need_stval)
        Rstval.Realize();
    };
    realize_all();

    auto get_arg = [&](int reg) -> OpArg {
      if (need_rd && reg == body.dst)
        return OpArg(Rd);
      if (need_r1 && reg == body.src1)
        return OpArg(R1);
      if (need_r2 && reg == body.src2)
        return OpArg(R2);
      if (need_base && reg == st_base)
        return OpArg(Rbase);
      ASSERT(need_stval && reg == st_val);
      return OpArg(Rstval);
    };

    FixupBranch skip_diamond = JumpIfCRFieldBit(crf, bit, branch_if_true);

    if (has_alu)
    {
      const OpArg arg1 = body.src1 >= 0 ? get_arg(body.src1) : OpArg{};
      const OpArg arg2 = body.src2 >= 0 ? get_arg(body.src2) : OpArg{};
      switch (body.kind)
      {
      case DiamondOp::AddImm:
        if (body.src1 < 0)
          MOV(32, Rd, Imm32(body.imm));
        else if (arg1.IsImm())
          MOV(32, Rd, Imm32(arg1.Imm32() + body.imm));
        else if (arg1.IsSimpleReg())
          LEA(32, Rd, MDisp(arg1.GetSimpleReg(), static_cast<s32>(body.imm)));
        else
        {
          MOV(32, Rd, arg1);
          if (body.imm != 0)
            ADD(32, Rd, Imm32(body.imm));
        }
        break;
      case DiamondOp::OrImm:
        if (arg1.IsImm())
          MOV(32, Rd, Imm32(arg1.Imm32() | body.imm));
        else
        {
          if (!arg1.IsSimpleReg(Rd))
            MOV(32, Rd, arg1);
          if (body.imm != 0)
            OR(32, Rd, Imm32(body.imm));
        }
        break;
      case DiamondOp::XorImm:
        if (arg1.IsImm())
          MOV(32, Rd, Imm32(arg1.Imm32() ^ body.imm));
        else
        {
          if (!arg1.IsSimpleReg(Rd))
            MOV(32, Rd, arg1);
          if (body.imm != 0)
            XOR(32, Rd, Imm32(body.imm));
        }
        break;
      case DiamondOp::MovReg:
        if (!arg1.IsSimpleReg(Rd))
          MOV(32, Rd, arg1);
        break;
      case DiamondOp::NegReg:
        if (arg1.IsImm())
          MOV(32, Rd, Imm32(0u - arg1.Imm32()));
        else
        {
          if (!arg1.IsSimpleReg(Rd))
            MOV(32, Rd, arg1);
          NEG(32, Rd);
        }
        break;
      case DiamondOp::AddReg:
        if (arg1.IsSimpleReg() && arg2.IsSimpleReg())
          LEA(32, Rd, MRegSum(arg1.GetSimpleReg(), arg2.GetSimpleReg()));
        else
        {
          MOV(32, R(RSCRATCH), arg1);
          ADD(32, R(RSCRATCH), arg2);
          MOV(32, Rd, R(RSCRATCH));
        }
        break;
      case DiamondOp::SubReg:
        MOV(32, R(RSCRATCH), arg1);
        SUB(32, R(RSCRATCH), arg2);
        MOV(32, Rd, R(RSCRATCH));
        break;
      case DiamondOp::OrReg:
        MOV(32, R(RSCRATCH), arg1);
        OR(32, R(RSCRATCH), arg2);
        MOV(32, Rd, R(RSCRATCH));
        break;
      case DiamondOp::AndReg:
        MOV(32, R(RSCRATCH), arg1);
        AND(32, R(RSCRATCH), arg2);
        MOV(32, Rd, R(RSCRATCH));
        break;
      case DiamondOp::XorReg:
        MOV(32, R(RSCRATCH), arg1);
        XOR(32, R(RSCRATCH), arg2);
        MOV(32, Rd, R(RSCRATCH));
        break;
      }
    }

    MOV(32, R(RSCRATCH), get_arg(st_base));
    const OpArg val_arg = get_arg(st_val);
    OpArg store_val = val_arg;
    if (!val_arg.IsImm())
    {
      MOV(32, R(RSCRATCH2), val_arg);
      store_val = R(RSCRATCH2);
    }

    const u32 store_idx = cur_idx + static_cast<u32>(branch_offset + n);
    m_dirty_marks_covered = store_idx < m_dirty_mark_plan.size() &&
                            m_dirty_mark_plan[store_idx].kind == DirtyMarkPlan::Kind::Covered;
    const u32 saved_pc = js.compilerPC;
    js.compilerPC = last_sop.address;
    SafeWriteRegToReg(store_val, RSCRATCH, store_access_size, st_disp,
                      CallerSavedRegistersInUse());
    js.compilerPC = saved_pc;
    m_dirty_marks_covered = false;

    SUB(32, PPCSTATE(downcount), Imm8(static_cast<s8>(total_cycles)));
    SetJumpTarget(skip_diamond);

    if (has_alu)
      m_constant_propagation.ClearGPR(body.dst);
    js.skipInstructions = branch_offset + n;
    return true;
  }

  // Bind destination ReadWrite (old value survives when branch is taken) and distinct sources.
  RCX64Reg Rd = gpr.Bind(body.dst, RCMode::ReadWrite);
  const bool need_r1 = body.src1 >= 0 && body.src1 != body.dst;
  const bool need_r2 = body.src2 >= 0 && body.src2 != body.dst && body.src2 != body.src1;
  RCOpArg R1 = need_r1 ? gpr.Use(body.src1, RCMode::Read) : RCOpArg{};
  RCOpArg R2 = need_r2 ? gpr.Use(body.src2, RCMode::Read) : RCOpArg{};
  if (need_r1 && need_r2)
    RegCache::Realize(Rd, R1, R2);
  else if (need_r1)
    RegCache::Realize(Rd, R1);
  else if (need_r2)
    RegCache::Realize(Rd, R2);
  else
    RegCache::Realize(Rd);

  const OpArg arg1 =
      (body.src1 < 0) ? OpArg{} : ((body.src1 == body.dst) ? OpArg(Rd) : OpArg(R1));
  const OpArg arg2 = (body.src2 < 0)
                         ? OpArg{}
                         : ((body.src2 == body.dst) ? OpArg(Rd) :
                            (body.src2 == body.src1) ? arg1 :
                                                       OpArg(R2));

  // Compute candidate result into RSCRATCH without touching Rd.
  switch (body.kind)
  {
  case DiamondOp::AddImm:
    if (body.src1 < 0)
    {
      MOV(32, R(RSCRATCH), Imm32(body.imm));
    }
    else if (arg1.IsImm())
    {
      MOV(32, R(RSCRATCH), Imm32(arg1.Imm32() + body.imm));
    }
    else if (arg1.IsSimpleReg())
    {
      if (body.imm == 0)
        MOV(32, R(RSCRATCH), arg1);
      else
        LEA(32, RSCRATCH, MDisp(arg1.GetSimpleReg(), static_cast<s32>(body.imm)));
    }
    else
    {
      MOV(32, R(RSCRATCH), arg1);
      if (body.imm != 0)
        ADD(32, R(RSCRATCH), Imm32(body.imm));
    }
    break;
  case DiamondOp::OrImm:
    if (arg1.IsImm())
    {
      MOV(32, R(RSCRATCH), Imm32(arg1.Imm32() | body.imm));
    }
    else
    {
      MOV(32, R(RSCRATCH), arg1);
      if (body.imm != 0)
        OR(32, R(RSCRATCH), Imm32(body.imm));
    }
    break;
  case DiamondOp::XorImm:
    if (arg1.IsImm())
    {
      MOV(32, R(RSCRATCH), Imm32(arg1.Imm32() ^ body.imm));
    }
    else
    {
      MOV(32, R(RSCRATCH), arg1);
      if (body.imm != 0)
        XOR(32, R(RSCRATCH), Imm32(body.imm));
    }
    break;
  case DiamondOp::MovReg:
    MOV(32, R(RSCRATCH), arg1);
    break;
  case DiamondOp::NegReg:
    if (arg1.IsImm())
    {
      MOV(32, R(RSCRATCH), Imm32(0u - arg1.Imm32()));
    }
    else
    {
      MOV(32, R(RSCRATCH), arg1);
      NEG(32, R(RSCRATCH));
    }
    break;
  case DiamondOp::AddReg:
    if (arg1.IsSimpleReg() && arg2.IsSimpleReg())
    {
      LEA(32, RSCRATCH, MRegSum(arg1.GetSimpleReg(), arg2.GetSimpleReg()));
    }
    else
    {
      MOV(32, R(RSCRATCH), arg1);
      ADD(32, R(RSCRATCH), arg2);
    }
    break;
  case DiamondOp::SubReg:
    MOV(32, R(RSCRATCH), arg1);
    SUB(32, R(RSCRATCH), arg2);
    break;
  case DiamondOp::OrReg:
    MOV(32, R(RSCRATCH), arg1);
    OR(32, R(RSCRATCH), arg2);
    break;
  case DiamondOp::AndReg:
    MOV(32, R(RSCRATCH), arg1);
    AND(32, R(RSCRATCH), arg2);
    break;
  case DiamondOp::XorReg:
    MOV(32, R(RSCRATCH), arg1);
    XOR(32, R(RSCRATCH), arg2);
    break;
  }

  // Prepare RSCRATCH2 = 0 before testing CR so SETcc below needs no MOVZX and preserves RFLAGS.
  XOR(32, R(RSCRATCH2), R(RSCRATCH2));

  // Test the CR bit after candidate computation so RFLAGS is fresh for CMOVcc + SETcc.
  // cc_exec is true when the branch is NOT taken (i.e. the skipped body executes).
  // Fast path: when merged with an immediately preceding `cmpwi rD, 0` or `add./subf./neg. rD`
  // that just wrote CR(crf) from body.dst vs 0, test Rd directly to avoid a store-to-load stall
  // on PPCSTATE_CR(crf). Rd still holds its pre-diamond value here.
  bool tested_reg_directly = false;
  CCFlags cc_exec = CC_A;
  if (branch_offset == 1 && bit != PowerPC::CR_SO_BIT)
  {
    const UGeckoInstruction prev = js.op[0].inst;
    const bool prev_cmpwi_zero = prev.OPCD == 11 && prev.L == 0 && prev.SIMM_16 == 0 &&
                                 static_cast<int>(prev.CRFD) == crf &&
                                 static_cast<int>(prev.RA) == body.dst;
    const bool prev_rc_rd =
        crf == 0 && prev.OPCD == 31 && prev.Rc && !prev.OE &&
        (prev.SUBOP10 == 266 || prev.SUBOP10 == 40 || prev.SUBOP10 == 104) &&
        static_cast<int>(prev.RD) == body.dst;
    if (prev_cmpwi_zero || prev_rc_rd)
    {
      TEST(32, Rd, Rd);
      switch (bit)
      {
      case PowerPC::CR_EQ_BIT:
        cc_exec = branch_if_true ? CC_NZ : CC_Z;
        break;
      case PowerPC::CR_GT_BIT:
        cc_exec = branch_if_true ? CC_LE : CC_G;
        break;
      case PowerPC::CR_LT_BIT:
        cc_exec = branch_if_true ? CC_NS : CC_S;
        break;
      default:
        break;
      }
      tested_reg_directly = true;
    }
  }

  if (!tested_reg_directly)
  {
    switch (bit)
    {
    case PowerPC::CR_SO_BIT:
      BT(64, PPCSTATE_CR(crf), Imm8(PowerPC::CR_EMU_SO_BIT));
      cc_exec = branch_if_true ? CC_NC : CC_C;
      break;
    case PowerPC::CR_EQ_BIT:
      CMP(32, PPCSTATE_CR(crf), Imm8(0));
      cc_exec = branch_if_true ? CC_NZ : CC_Z;
      break;
    case PowerPC::CR_GT_BIT:
      CMP(64, PPCSTATE_CR(crf), Imm8(0));
      cc_exec = branch_if_true ? CC_LE : CC_G;
      break;
    case PowerPC::CR_LT_BIT:
      BT(64, PPCSTATE_CR(crf), Imm8(PowerPC::CR_EMU_LT_BIT));
      cc_exec = branch_if_true ? CC_NC : CC_C;
      break;
    default:
      ASSERT_MSG(DYNA_REC, false, "Invalid CR bit");
      return false;
    }
  }

  CMOVcc(32, Rd, R(RSCRATCH), cc_exec);

  // Cycle-exact downcount adjustment: subtract body_cycles iff the body executed.
  // RSCRATCH2 was zeroed before CR test, so SETcc sets it to 0 or 1.
  SETcc(cc_exec, R(RSCRATCH2));
  if (body.body_cycles == 2)
  {
    ADD(32, R(RSCRATCH2), R(RSCRATCH2));
  }
  else if (body.body_cycles == 3)
  {
    LEA(32, RSCRATCH2, MComplex(RSCRATCH2, RSCRATCH2, SCALE_2, 0));
  }
  else if (body.body_cycles > 3)
  {
    IMUL(32, RSCRATCH2, R(RSCRATCH2), Imm32(body.body_cycles));
  }
  SUB(32, PPCSTATE(downcount), R(RSCRATCH2));

  m_constant_propagation.ClearGPR(body.dst);
  js.skipInstructions = branch_offset + n;
  return true;
}

bool Jit64::TryCompileBranchlessStoreOverwrite(UGeckoInstruction inst, int access_size)
{
  if (!analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE) ||
      IsBranchWatchEnabled() || bJITIntegerOff || bJITLoadStoreOff || bJITRegisterCacheOff ||
      jo.memcheck || !jo.fastmem || code_block.m_broken ||
      m_system.GetPowerPC().GetMemChecks().HasAny())
  {
    return false;
  }

  const u32 cur_idx = static_cast<u32>(js.op - m_code_buffer.data());
  if (HasPriorFollowedBranch(m_code_buffer.data(), cur_idx + 1))
    return false;

  // Match:
  //   st{w,h,b} rS0, disp(rA)
  //   bcx +8/+12/+16          (conditional forward branch, no LK, no CTR decrement)
  //   [0..2 pure ALU ops producing rDst, where rDst != rA]
  //   st{w,h,b} rS1, disp(rA) (identical opcode, base rA, and disp; rS1 == rDst if ALU present)
  if (!CanMergeNextInstructions(1) || js.instructionsLeft < 3)
    return false;

  const PPCAnalyst::CodeOp& branch_op = js.op[1];
  const UGeckoInstruction& binst = branch_op.inst;
  if (branch_op.address != js.op[0].address + 4 || branch_op.skip || binst.OPCD != 16 ||
      binst.LK || !(binst.BO & BO_DONT_DECREMENT_FLAG) ||
      (binst.BO & BO_DONT_CHECK_CONDITION) || branch_op.followTaken ||
      branch_op.branchIsIdleLoop || branch_op.branchCounts != nullptr ||
      HLE::TryReplaceFunction(m_ppc_symbol_db, branch_op.address, PowerPC::CoreMode::JIT))
  {
    return false;
  }

  const u32 target = branch_op.branchTo;
  const u32 diff = target - branch_op.address;
  if (diff != 8 && diff != 12 && diff != 16)
    return false;
  const int n = static_cast<int>(diff / 4) - 1;  // 1, 2, or 3 skipped ops (last is st1)
  const int alu_count = n - 1;                   // 0, 1, or 2 ALU ops

  if (!CanMergeNextInstructions(1 + n) || js.instructionsLeft < 2 + n ||
      js.op[2 + n].address != target)
  {
    return false;
  }

  const PPCAnalyst::CodeOp& st1_op = js.op[1 + n];
  const UGeckoInstruction st1 = st1_op.inst;
  if (st1.OPCD != inst.OPCD || st1.RA != inst.RA || st1.SIMM_16 != inst.SIMM_16)
    return false;

  if (cur_idx + 1 < m_dirty_mark_plan.size() &&
      m_dirty_mark_plan[cur_idx + 1].kind == DirtyMarkPlan::Kind::Leader)
  {
    return false;
  }

  for (int k = 1; k <= n; ++k)
  {
    const PPCAnalyst::CodeOp& sop = js.op[1 + k];
    const u32 sop_idx = cur_idx + static_cast<u32>(1 + k);
    const bool is_st1 = (k == n);
    if (sop.address != branch_op.address + static_cast<u32>(k * 4) || sop.skip || sop.canEndBlock ||
        (!is_st1 && sop.canCauseException) || sop.crOut || sop.outputCA || sop.outputFPRF ||
        HLE::TryReplaceFunction(m_ppc_symbol_db, sop.address, PowerPC::CoreMode::JIT) ||
        (sop_idx < m_dirty_mark_plan.size() &&
         m_dirty_mark_plan[sop_idx].kind == DirtyMarkPlan::Kind::Leader))
    {
      return false;
    }
  }

  DiamondBody body;
  const bool has_alu = alu_count > 0;
  if (has_alu)
  {
    if (!DecodeDiamondBody(&js.op[2], alu_count, &body))
      return false;
    if (body.dst == static_cast<int>(inst.RA) || body.dst != static_cast<int>(st1.RS))
      return false;
  }

  const int st_base = static_cast<int>(inst.RA);
  const int st_val0 = static_cast<int>(inst.RS);
  const int st_val1 = static_cast<int>(st1.RS);
  const s32 st_disp = static_cast<s16>(inst.SIMM_16);
  const int crf = binst.BI >> 2;
  const int bit = 3 - (binst.BI & 3);
  const bool branch_if_true = !!(binst.BO_2 & BO_BRANCH_IF_TRUE);
  const u32 body_cycles =
      (has_alu ? body.body_cycles : 0u) + static_cast<u32>(st1_op.opinfo->num_cycles);

  // Account for the always-executed bcx instruction (st0 was already counted by DoJit).
  js.downcountAmount += branch_op.opinfo->num_cycles;

  FlushRegistersBeforeSlowAccess();

  {
    RCX64Reg Ra = gpr.Bind(st_base, RCMode::Read);
    RCX64Reg Rd;
    RCOpArg R1, R2, Rval0, Rval1;
    bool need_rd = false, need_r1 = false, need_r2 = false, need_val0 = false, need_val1 = false;

    if (has_alu)
    {
      need_rd = true;
      Rd = gpr.Bind(body.dst, RCMode::ReadWrite);
      if (body.src1 >= 0 && body.src1 != body.dst && body.src1 != st_base)
      {
        need_r1 = true;
        R1 = gpr.Use(body.src1, RCMode::Read);
      }
      if (body.src2 >= 0 && body.src2 != body.dst && body.src2 != st_base &&
          body.src2 != body.src1)
      {
        need_r2 = true;
        R2 = gpr.Use(body.src2, RCMode::Read);
      }
    }
    if (st_val0 != st_base && (!need_rd || st_val0 != body.dst) &&
        (!need_r1 || st_val0 != body.src1) && (!need_r2 || st_val0 != body.src2))
    {
      need_val0 = true;
      Rval0 = gpr.Use(st_val0, RCMode::Read);
    }
    if (!has_alu && st_val1 != st_base && st_val1 != st_val0)
    {
      need_val1 = true;
      Rval1 = gpr.Use(st_val1, RCMode::Read);
    }

    Ra.Realize();
    if (need_rd)
      Rd.Realize();
    if (need_r1)
      R1.Realize();
    if (need_r2)
      R2.Realize();
    if (need_val0)
      Rval0.Realize();
    if (need_val1)
      Rval1.Realize();

    auto get_arg = [&](int reg) -> OpArg {
      if (reg == st_base)
        return OpArg(Ra);
      if (need_rd && reg == body.dst)
        return OpArg(Rd);
      if (need_r1 && reg == body.src1)
        return OpArg(R1);
      if (need_r2 && reg == body.src2)
        return OpArg(R2);
      if (need_val0 && reg == st_val0)
        return OpArg(Rval0);
      ASSERT(need_val1 && reg == st_val1);
      return OpArg(Rval1);
    };

    // Compute candidate value (for body.dst and st1) into RSCRATCH without touching Rd.
    if (has_alu)
    {
      const OpArg arg1 = body.src1 >= 0 ? get_arg(body.src1) : OpArg{};
      const OpArg arg2 = body.src2 >= 0 ? get_arg(body.src2) : OpArg{};
      switch (body.kind)
      {
      case DiamondOp::AddImm:
        if (body.src1 < 0)
          MOV(32, R(RSCRATCH), Imm32(body.imm));
        else if (arg1.IsImm())
          MOV(32, R(RSCRATCH), Imm32(arg1.Imm32() + body.imm));
        else if (arg1.IsSimpleReg())
          LEA(32, RSCRATCH, MDisp(arg1.GetSimpleReg(), static_cast<s32>(body.imm)));
        else
        {
          MOV(32, R(RSCRATCH), arg1);
          if (body.imm != 0)
            ADD(32, R(RSCRATCH), Imm32(body.imm));
        }
        break;
      case DiamondOp::OrImm:
        if (arg1.IsImm())
          MOV(32, R(RSCRATCH), Imm32(arg1.Imm32() | body.imm));
        else
        {
          MOV(32, R(RSCRATCH), arg1);
          if (body.imm != 0)
            OR(32, R(RSCRATCH), Imm32(body.imm));
        }
        break;
      case DiamondOp::XorImm:
        if (arg1.IsImm())
          MOV(32, R(RSCRATCH), Imm32(arg1.Imm32() ^ body.imm));
        else
        {
          MOV(32, R(RSCRATCH), arg1);
          if (body.imm != 0)
            XOR(32, R(RSCRATCH), Imm32(body.imm));
        }
        break;
      case DiamondOp::MovReg:
        MOV(32, R(RSCRATCH), arg1);
        break;
      case DiamondOp::NegReg:
        if (arg1.IsImm())
          MOV(32, R(RSCRATCH), Imm32(0u - arg1.Imm32()));
        else
        {
          MOV(32, R(RSCRATCH), arg1);
          NEG(32, R(RSCRATCH));
        }
        break;
      case DiamondOp::AddReg:
        if (arg1.IsSimpleReg() && arg2.IsSimpleReg())
          LEA(32, RSCRATCH, MRegSum(arg1.GetSimpleReg(), arg2.GetSimpleReg()));
        else
        {
          MOV(32, R(RSCRATCH), arg1);
          ADD(32, R(RSCRATCH), arg2);
        }
        break;
      case DiamondOp::SubReg:
        MOV(32, R(RSCRATCH), arg1);
        SUB(32, R(RSCRATCH), arg2);
        break;
      case DiamondOp::OrReg:
        MOV(32, R(RSCRATCH), arg1);
        OR(32, R(RSCRATCH), arg2);
        break;
      case DiamondOp::AndReg:
        MOV(32, R(RSCRATCH), arg1);
        AND(32, R(RSCRATCH), arg2);
        break;
      case DiamondOp::XorReg:
        MOV(32, R(RSCRATCH), arg1);
        XOR(32, R(RSCRATCH), arg2);
        break;
      }
    }
    else
    {
      MOV(32, R(RSCRATCH), get_arg(st_val1));
    }

    // Prepare RSCRATCH2 with the taken-path store value (st_val0) if it differs from Rd.
    const bool store_is_rd = has_alu && st_val0 == body.dst;
    if (!store_is_rd)
      MOV(32, R(RSCRATCH2), get_arg(st_val0));
    else
      XOR(32, R(RSCRATCH2), R(RSCRATCH2));

    CCFlags cc_exec = CC_A;
    switch (bit)
    {
    case PowerPC::CR_SO_BIT:
      BT(64, PPCSTATE_CR(crf), Imm8(PowerPC::CR_EMU_SO_BIT));
      cc_exec = branch_if_true ? CC_NC : CC_C;
      break;
    case PowerPC::CR_EQ_BIT:
      CMP(32, PPCSTATE_CR(crf), Imm8(0));
      cc_exec = branch_if_true ? CC_NZ : CC_Z;
      break;
    case PowerPC::CR_GT_BIT:
      CMP(64, PPCSTATE_CR(crf), Imm8(0));
      cc_exec = branch_if_true ? CC_LE : CC_G;
      break;
    case PowerPC::CR_LT_BIT:
      BT(64, PPCSTATE_CR(crf), Imm8(PowerPC::CR_EMU_LT_BIT));
      cc_exec = branch_if_true ? CC_NC : CC_C;
      break;
    default:
      ASSERT_MSG(DYNA_REC, false, "Invalid CR bit");
      return false;
    }

    if (has_alu)
      CMOVcc(32, Rd, R(RSCRATCH), cc_exec);
    if (!store_is_rd)
      CMOVcc(32, RSCRATCH2, R(RSCRATCH), cc_exec);

    // Cycle-exact downcount adjustment (0 if taken, body_cycles if not taken).
    const X64Reg dc_reg = store_is_rd ? RSCRATCH2 : RSCRATCH;
    SETcc(cc_exec, R(dc_reg));
    if (!store_is_rd)
      MOVZX(32, 8, dc_reg, R(dc_reg));
    if (body_cycles == 2)
      ADD(32, R(dc_reg), R(dc_reg));
    else if (body_cycles == 3)
      LEA(32, dc_reg, MComplex(dc_reg, dc_reg, SCALE_2, 0));
    else if (body_cycles > 3)
      IMUL(32, dc_reg, R(dc_reg), Imm32(body_cycles));
    SUB(32, PPCSTATE(downcount), R(dc_reg));

    // Single branchless store of the final value (st_val0 if taken, cand/st_val1 if not taken).
    OpArg store_arg = store_is_rd ? OpArg(Rd) : R(RSCRATCH2);
    if (store_is_rd && WriteClobbersRegValue(access_size, /* swap */ true))
    {
      MOV(32, R(RSCRATCH2), Rd);
      store_arg = R(RSCRATCH2);
    }
    SafeWriteRegToReg(store_arg, Ra, access_size, st_disp, CallerSavedRegistersInUse(),
                      SAFE_LOADSTORE_CLOBBER_RSCRATCH_INSTEAD_OF_ADDR);
  }

  // Exact downcount-expiry guard on the taken path: all RCX64Reg/RCOpArg locks are now released.
  // If taken, Rd was untouched, st_val0 was written to disp(rA), and downcount was unchanged,
  // matching an unconverted st0 + bcx exit to the exact cycle.
  if (m_far_code.Enabled() && !m_far_code.IsInSpace(GetCodePtr()))
  {
    if (js.downcountAmount <= 127)
      CMP(32, PPCSTATE(downcount), Imm8(static_cast<s8>(js.downcountAmount)));
    else
      CMP(32, PPCSTATE(downcount), Imm32(js.downcountAmount));
    FixupBranch maybe_expire = J_CC(CC_LE, Jump::Near);
    SwitchToFarCode();
    SetJumpTarget(maybe_expire);
    FixupBranch not_taken_far = JumpIfCRFieldBit(crf, bit, !branch_if_true);
    {
      RCForkGuard gpr_guard = gpr.Fork();
      RCForkGuard fpr_guard = fpr.Fork();
      gpr.Flush();
      fpr.Flush();
      WriteExit(target);
    }
    SetJumpTarget(not_taken_far);
    FixupBranch resume = J(Jump::Near);
    SwitchToNearCode();
    SetJumpTarget(resume);
  }

  if (has_alu)
    m_constant_propagation.ClearGPR(body.dst);
  js.skipInstructions = 1 + n;
  return true;
}
