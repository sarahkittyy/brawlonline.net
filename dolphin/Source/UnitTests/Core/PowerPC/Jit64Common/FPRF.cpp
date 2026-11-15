// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// GitHub issue #49 (lazy FPRF): Jit64's eager classification (EmuCodeBlock::SetFPRF, both its
// SSE4.1 and its plain path) against Common::ClassifyFloat/ClassifyDouble, which the lazy flush
// uses; every single, many doubles. And the lazy state (PowerPCState::fprf_lazy) against the eager
// order of updates for random sequences of results and compares.

#include <bit>
#include <random>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/FloatUtils.h"
#include "Common/ScopeGuard.h"
#include "Common/x64ABI.h"
#include "Core/Core.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64Common/Jit64AsmCommon.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "../TestValues.h"

#include <gtest/gtest.h>

namespace
{
class TestFPRFRoutines : public CommonAsmRoutines
{
public:
  explicit TestFPRFRoutines(Core::System& system) : CommonAsmRoutines(jit), jit(system)
  {
    using namespace Gen;
    AllocCodeSpace(16384);
    m_const_pool.Init(AllocChildCodeSpace(4096), 4096);

    const bool had_sse41 = cpu_info.bSSE4_1;
    for (int sse41 = 0; sse41 < 2; ++sse41)
    {
      cpu_info.bSSE4_1 = sse41 != 0 && had_sse41;
      for (int single = 0; single < 2; ++single)
      {
        auto& f = fn[sse41][single];
        f = reinterpret_cast<u32 (*)(u64, UReg_FPSCR&)>(AlignCode16());
        ABI_PushRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
        // SetFPRF reads and writes fpscr alone: point PPCSTATE at the caller's.
        LEA(64, RPPCSTATE, MDisp(ABI_PARAM2, -PPCSTATE_OFF(fpscr)));
        MOVQ_xmm(XMM0, R(ABI_PARAM1));
        // Junk in the upper half, as a packed single result leaves it.
        if (single)
        {
          MOV(64, R(RSCRATCH), Imm64(0xDEADBEEFCAFEF00DULL));
          MOVQ_xmm(XMM1, R(RSCRATCH));
          PUNPCKLQDQ(XMM0, R(XMM1));
        }
        SetFPRF(XMM0, single != 0);
        MOV(32, R(ABI_RETURN), PPCSTATE(fpscr));
        ABI_PopRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
        RET();
      }
    }
    cpu_info.bSSE4_1 = had_sse41;
    has_sse41 = had_sse41;
  }

  u32 Run(bool sse41, bool single, u64 bits, u32 fpscr_before) const
  {
    UReg_FPSCR fpscr{fpscr_before};
    return fn[sse41][single](bits, fpscr);
  }

  u32 (*fn[2][2])(u64, UReg_FPSCR&) = {};
  bool has_sse41 = false;
  Jit64 jit;
};

u32 Expected(bool single, u64 bits, u32 fpscr_before)
{
  const u32 c = single ? Common::ClassifyFloat(std::bit_cast<float>(static_cast<u32>(bits))) :
                         Common::ClassifyDouble(std::bit_cast<double>(bits));
  // UReg_FPSCR drops the reserved bit, as Run's does.
  return (UReg_FPSCR{fpscr_before}.Hex & ~FPRF_MASK) | (c << FPRF_SHIFT);
}
}  // namespace

TEST(Jit64, FPRFEveryFloat)
{
  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_thread_guard([] { Core::UndeclareAsCPUThread(); });
  const TestFPRFRoutines r(Core::System::GetInstance());
  if (!r.has_sse41)
    fmt::print("no SSE4.1 here: the SSE4.1 path is not tested\n");

  u64 bad = 0;
  for (int sse41 = r.has_sse41 ? 1 : 0; sse41 >= 0; --sse41)
  {
    u32 i = 0;
    do
    {
      // Every other FPSCR bit set and cleared in turn, FPRF holding something else.
      const u32 before = (i & 1) ? 0xFFFFFFFFu : (0x0000F000u ^ (i & 0x1F000u));
      const u32 actual = r.Run(sse41 != 0, true, i, before);
      const u32 expected = Expected(true, i, before);
      if (actual != expected && bad++ < 20)
        fmt::print("sse41 {} single {:08x}: {:08x} != {:08x}\n", sse41, i, actual, expected);
    } while (++i != 0);
  }
  EXPECT_EQ(bad, 0u);
}

TEST(Jit64, FPRFDoubles)
{
  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_thread_guard([] { Core::UndeclareAsCPUThread(); });
  const TestFPRFRoutines r(Core::System::GetInstance());

  std::vector<u64> values(double_test_values.begin(), double_test_values.end());
  const u64 exps[] = {0, 1, 2, 0x3FE, 0x3FF, 0x400, 0x380, 0x37F, 0x47F, 0x7FD, 0x7FE, 0x7FF};
  const u64 mants[] = {0, 1, 2, 0x0008000000000ULL, 0x0007FFFFFFFFFULL, 0xFFFFFFFFFFFFFULL,
                       0x8000000000000ULL, 0x7FFFFFFFFFFFFULL, 0x0000000100000ULL,
                       0x00000FFFFFFFFULL, 0x0000020000000ULL};
  for (u64 s = 0; s < 2; ++s)
    for (const u64 e : exps)
      for (const u64 m : mants)
        values.push_back((s << 63) | (e << 52) | m);
  std::mt19937_64 rng(49);
  for (int k = 0; k < 50'000'000; ++k)
  {
    u64 v = rng();
    // Bias towards the edge exponents.
    switch (k & 3)
    {
    case 1:
      v &= ~(0x7FFULL << 52);
      break;
    case 2:
      v |= 0x7FFULL << 52;
      break;
    default:
      break;
    }
    values.push_back(v);
  }

  u64 bad = 0;
  for (int sse41 = r.has_sse41 ? 1 : 0; sse41 >= 0; --sse41)
  {
    for (size_t k = 0; k < values.size(); ++k)
    {
      const u32 before = (k & 1) ? 0xFFFFFFFFu : static_cast<u32>(k * 0x9E3779B9u);
      const u32 actual = r.Run(sse41 != 0, false, values[k], before);
      const u32 expected = Expected(false, values[k], before);
      if (actual != expected && bad++ < 20)
        fmt::print("sse41 {} double {:016x}: {:08x} != {:08x}\n", sse41, values[k], actual,
                   expected);
    }
  }
  EXPECT_EQ(bad, 0u);
}

// The lazy state, written as Jit64 writes it, read through CurrentFPSCR/FlushLazyFPRF, against
// FPSCR updated eagerly in the same order (results classify all five bits; a compare sets FPCC
// and leaves C; an interpreter result goes through UpdateFPRF*, which drops what is pending).
TEST(Jit64, FPRFLazyState)
{
  std::mt19937_64 rng(4949);
  auto& system = Core::System::GetInstance();
  PowerPC::PowerPCState& lazy = system.GetPPCState();
  const u32 compares[] = {PowerPC::CR_LT, PowerPC::CR_GT, PowerPC::CR_EQ, PowerPC::CR_SO};
  auto value = [&](bool single) -> u64 {
    if (rng() % 3 == 0)
    {
      const u64 picks[] = {0, 1ull << 63, 1, 0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
                           0x7FF8000000000000ULL, 0x7FF0000000000001ULL, 0x000FFFFFFFFFFFFFULL,
                           0x800FFFFFFFFFFFFFULL, 0x3FF0000000000000ULL};
      const u32 fpicks[] = {0, 0x80000000u, 1, 0x80000001u, 0x7F800000u, 0xFF800000u,
                            0x7FC00000u, 0x7F800001u, 0x007FFFFFu, 0x3F800000u};
      return single ? fpicks[rng() % 10] : picks[rng() % 10];
    }
    return single ? (rng() & 0xFFFFFFFFu) : rng();
  };
  u64 checks = 0, bad = 0;
  for (int seq = 0; seq < 200'000; ++seq)
  {
    u32 eager = UReg_FPSCR{static_cast<u32>(rng())}.Hex;
    lazy.fpscr.Hex = eager;
    lazy.fprf_lazy = 0;
    const int len = 1 + static_cast<int>(rng() % 12);
    for (int step = 0; step < len; ++step)
    {
      switch (rng() % 6)
      {
      case 0:
      case 1:  // a compiled result
      {
        const bool single = rng() & 1;
        const u64 v = value(single);
        eager = Expected(single, v, eager);
        lazy.fprf_lazy_value = single ? (v | (rng() << 32)) : v;
        lazy.fprf_lazy = single ? PowerPC::PowerPCState::LAZY_FPRF_SINGLE :
                                  PowerPC::PowerPCState::LAZY_FPRF_DOUBLE;
        break;
      }
      case 2:
      case 3:  // a compiled compare
      {
        const u32 c = compares[rng() % 4];
        eager = (eager & ~FPCC_MASK) | (c << FPRF_SHIFT);
        reinterpret_cast<u8*>(&lazy.fprf_lazy)[1] = static_cast<u8>(c);
        break;
      }
      case 4:  // an interpreter result
      {
        const bool single = rng() & 1;
        const u64 v = value(single);
        eager = Expected(single, v, eager);
        if (single)
          lazy.UpdateFPRFSingle(std::bit_cast<float>(static_cast<u32>(v)));
        else
          lazy.UpdateFPRFDouble(std::bit_cast<double>(v));
        break;
      }
      case 5:  // another FPSCR bit changes (an exception bit, the rounding mode)
      {
        const u32 bit = 1u << (rng() % 32);
        if ((bit & FPRF_MASK) || UReg_FPSCR{bit}.Hex != bit)
          break;
        eager ^= bit;
        lazy.fpscr.Hex ^= bit;
        break;
      }
      }
      ++checks;
      if (lazy.CurrentFPSCR() != eager && bad++ < 20)
        fmt::print("seq {} step {}: {:08x} != {:08x}\n", seq, step, lazy.CurrentFPSCR(), eager);
      if (rng() % 4 == 0)
      {
        lazy.FlushLazyFPRF();
        if ((lazy.fpscr.Hex != eager || lazy.fprf_lazy != 0) && bad++ < 20)
          fmt::print("seq {} step {} flushed: {:08x} != {:08x}\n", seq, step, lazy.fpscr.Hex,
                     eager);
      }
    }
  }
  lazy.fpscr.Hex = 0;
  lazy.fprf_lazy = 0;
  EXPECT_GT(checks, 0u);
  EXPECT_EQ(bad, 0u);
}
