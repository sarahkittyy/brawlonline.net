// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/DolphinEngine.h"

#include <algorithm>
#include <iterator>

#include "BrawlOnline/Engine.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace Rollback
{
namespace
{
// The session's hooks run on the CPU thread, in the middle of the emulated loop: the PowerPC
// state is the guest's at the hook (HLE_Misc.cpp's Brawlback hooks).
class DolphinEngine final : public BrawlOnline::Engine
{
public:
  void ContinueFrame() override
  {
    auto& ppc = Ppc();
    ppc.gpr[25] = 1;  // the replaced li r25,1
    ppc.npc = BrawlOnline::LOOP_TOP + 4;
  }

  void SkipFrame() override
  {
    auto& ppc = Ppc();
    ppc.gpr[25] = 1;
    ppc.npc = BrawlOnline::LOOP_END;
  }

  void BackToLoopTop() override { Ppc().npc = BrawlOnline::LOOP_TOP; }

  void FinishFrameEnd() override
  {
    auto& system = Core::System::GetInstance();
    auto& ppc = system.GetPPCState();
    const Core::CPUThreadGuard guard(system);
    // The replaced instruction: stw r0,0x100(r23).
    PowerPC::MMU::HostWrite<u32>(guard, ppc.gpr[0], ppc.gpr[23] + 0x100);
    ppc.npc = BrawlOnline::FRAME_END + 4;
  }

  void CallFromLoopTop(u32 function, std::initializer_list<u32> args) override
  {
    auto& ppc = Ppc();
    int r = 3;
    for (const u32 a : args)
      ppc.gpr[r++] = a;
    ppc.spr[SPR_LR] = BrawlOnline::LOOP_TOP;
    ppc.npc = function;
  }

  u32 Gpr(int index) const override { return Ppc().gpr[index]; }

  void SaveGuestRegs(BrawlOnline::GuestRegs* out) const override
  {
    const auto& ppc = Ppc();
    std::copy(std::begin(ppc.gpr), std::end(ppc.gpr), out->gpr.begin());
    for (size_t i = 0; i < out->ps.size(); ++i)
      out->ps[i] = {ppc.ps[i].PS0AsU64(), ppc.ps[i].PS1AsU64()};
    std::copy(std::begin(ppc.cr.fields), std::end(ppc.cr.fields), out->cr.begin());
    out->fpscr = ppc.fpscr.Hex;
    out->xer_ca = ppc.xer_ca;
    out->xer_so_ov = ppc.xer_so_ov;
    out->xer_stringctrl = ppc.xer_stringctrl;
    out->lr = ppc.spr[SPR_LR];
    out->ctr = ppc.spr[SPR_CTR];
  }

  void RestoreGuestRegs(const BrawlOnline::GuestRegs& in) override
  {
    auto& ppc = Ppc();
    std::copy(in.gpr.begin(), in.gpr.end(), std::begin(ppc.gpr));
    for (size_t i = 0; i < in.ps.size(); ++i)
      ppc.ps[i].SetBoth(in.ps[i][0], in.ps[i][1]);
    std::copy(in.cr.begin(), in.cr.end(), std::begin(ppc.cr.fields));
    const bool rounding_changed = ppc.fpscr.Hex != in.fpscr;
    ppc.fpscr.Hex = in.fpscr;
    if (rounding_changed)
      PowerPC::RoundingModeUpdated(ppc);
    ppc.xer_ca = in.xer_ca;
    ppc.xer_so_ov = in.xer_so_ov;
    ppc.xer_stringctrl = in.xer_stringctrl;
    ppc.spr[SPR_LR] = in.lr;
    ppc.spr[SPR_CTR] = in.ctr;
  }

  bool CpuRunning() const override
  {
    return Core::System::GetInstance().GetCPU().GetState() == CPU::State::Running;
  }

private:
  static PowerPC::PowerPCState& Ppc() { return Core::System::GetInstance().GetPPCState(); }
};

DolphinEngine s_engine;
}  // namespace

void RegisterDolphinEngine()
{
  BrawlOnline::SetEngine(&s_engine);
}
}  // namespace Rollback
