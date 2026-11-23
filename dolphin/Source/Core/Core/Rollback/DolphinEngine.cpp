// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/DolphinEngine.h"

#include <algorithm>
#include <iterator>

#include "BrawlOnline/Engine.h"
#include "Common/Version.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/RollbackManager.h"
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
  // Our Dolphin's version: git describe of the build ("v0.2.5" for a release, whose tag CI makes).
  BrawlOnline::EngineIdentity Identity() const override
  {
    return {"dolphin", Common::GetScmDescStr()};
  }

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
    out->fpscr = ppc.CurrentFPSCR();
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
    ppc.fprf_lazy = 0;
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

  u64 GetTicks() const override { return Timing().GetTicks(); }
  u32 GetTicksPerSecond() const override
  {
    return Core::System::GetInstance().GetSystemTimers().GetTicksPerSecond();
  }
  u32 GetTicksPerField() const override
  {
    return Core::System::GetInstance().GetVideoInterface().GetTicksPerField();
  }
  u64 GetFakeTimeBase() const override
  {
    return Core::System::GetInstance().GetSystemTimers().GetFakeTimeBase();
  }
  bool IsRollbackResimulating() const override { return Timing().IsRollbackResimulating(); }
  void SetRollbackResimulating(bool resimulating) override
  {
    Timing().SetRollbackResimulating(resimulating);
  }
  void SetRollbackLoadWait(bool waiting) override { Timing().SetRollbackLoadWait(waiting); }
  void SetRollbackAudioWaits(bool wait) override { Timing().SetRollbackAudioWaits(wait); }
  void AddRollbackAudioPause(s64 ticks) override { Timing().AddRollbackAudioPause(ticks); }
  void SetRollbackSpeedAdjustment(double factor) override
  {
    Timing().SetRollbackSpeedAdjustment(factor);
  }
  void AdvanceThrottle(DT amount) override { Timing().AdvanceThrottle(amount); }
  void SetPresentPacing(bool on) override { Timing().SetPresentPacing(on); }
  void SetPacedSleepLead(PacedSleepLead lead) override { Timing().SetPacedSleepLead(lead); }
  void ResetThrottleToNow() override { Timing().ResetThrottleToNow(); }
  void BeginRollbackBurst() override { Timing().BeginRollbackBurst(); }
  DT TakeMaxBehindSchedule() override { return Timing().TakeMaxBehindSchedule(); }
  std::optional<TimePoint> ScheduledHostTime() override { return Timing().ScheduledHostTime(); }
  void SleepUntil(TimePoint time_point) override { Timing().SleepUntil(time_point); }

  void BeginRegionMode(const Ranges& ranges, const Ranges& exclude) override
  {
    Rollback::RollbackManager::Get().BeginRegionMode(ranges, exclude);
  }
  void EndRegionMode() override { Rollback::RollbackManager::Get().EndRegionMode(); }
  void ToggleFrameSave() override { Rollback::RollbackManager::Get().ToggleFrameSave(); }
  void SaveFrame(s64 tag) override
  {
    Rollback::RollbackManager::Get().SaveFrame(Core::System::GetInstance(), tag);
  }
  bool LoadFrame(int frames_back) override
  {
    return Rollback::RollbackManager::Get().LoadFrame(Core::System::GetInstance(), frames_back);
  }
  int SlotsBackForTag(s64 tag) const override
  {
    return Rollback::RollbackManager::Get().SlotsBackForTag(tag);
  }
  int RingCount() const override { return Rollback::RollbackManager::Get().m_ring_count; }
  std::optional<PhysRange> MailboxRegion() const override
  {
    const auto& mb = Rollback::RollbackManager::Get().m_mailbox_region;
    if (!mb)
      return std::nullopt;
    return PhysRange{mb->phys_start, mb->phys_end};
  }
  void SetMailboxRegion(u32 virt_addr, u32 size_bytes) override
  {
    Rollback::RollbackManager::Get().SetMailboxRegion(virt_addr, size_bytes);
  }
  std::vector<std::pair<std::string, double>> SnapshotStats() const override
  {
    const auto& rbm = Rollback::RollbackManager::Get();
    const auto t = rbm.GetTimingStats();
    const auto d = [](auto v) { return static_cast<double>(v); };
    return {
        {"partial_granules", d(rbm.RegionPartialGranules())},
        {"save_count", d(t.save_count)},
        {"save_us_total", d(t.save_us_total)},
        {"save_us_max", d(t.save_us_max)},
        {"load_count", d(t.load_count)},
        {"load_us_total", d(t.load_us_total)},
        {"load_us_max", d(t.load_us_max)},
        {"load_evict_waits", d(t.load_evict_waits)},
        {"load_evict_wait_us_max", d(t.load_evict_wait_us_max)},
        {"save_granules_total", d(t.save_granules_total)},
        {"save_granules_max", d(t.save_granules_max)},
        {"load_granules_total", d(t.load_granules_total)},
        {"load_granules_max", d(t.load_granules_max)},
    };
  }
  std::vector<std::pair<u32, u64>> PartialGranuleSkips() const override
  {
    const auto m = Rollback::RollbackManager::Get().PartialSkippedStats();
    return {m.begin(), m.end()};
  }

private:
  static PowerPC::PowerPCState& Ppc() { return Core::System::GetInstance().GetPPCState(); }
  static CoreTiming::CoreTimingManager& Timing()
  {
    return Core::System::GetInstance().GetCoreTiming();
  }
};

DolphinEngine s_engine;
// Set when the program loads: the session's resets and the bridge run before any game boots (the
// harness's and the launcher's first commands). Core::Init's call keeps this file linked in.
[[maybe_unused]] const bool s_registered = (BrawlOnline::SetEngine(&s_engine), true);
}  // namespace

void RegisterDolphinEngine()
{
  BrawlOnline::SetEngine(&s_engine);
}
}  // namespace Rollback
