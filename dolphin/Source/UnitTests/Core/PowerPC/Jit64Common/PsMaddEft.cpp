// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <bit>
#include <random>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/ScopeGuard.h"
#include "Common/x64ABI.h"
#include "Core/Core.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64Common/Jit64AsmCommon.h"
#include "Core/System.h"

#include <gtest/gtest.h>

namespace
{
// in: XMM0, XMM1, XMM2 (two lanes each); out: XMM1.
using WrappedEft = void (*)(const u64* in, u64* out);

// The routine twice: with SSE4.1 (PCMPEQQ, PTEST) and with the SSE2 fallback, as on a CPU without
// it (cpu_info.bSSE4_1 is read while the code is generated).
class TestCommonAsmRoutines : public CommonAsmRoutines
{
public:
  explicit TestCommonAsmRoutines(Core::System& system) : CommonAsmRoutines(jit), jit(system)
  {
    AllocCodeSpace(4096);
    m_const_pool.Init(AllocChildCodeSpace(1024), 1024);

    const bool had_sse4_1 = cpu_info.bSSE4_1;
    Common::ScopeGuard restore([&] { cpu_info.bSSE4_1 = had_sse4_1; });
    cpu_info.bSSE4_1 = true;
    sse4_1 = Wrap();
    cpu_info.bSSE4_1 = false;
    sse2 = Wrap();
  }

  WrappedEft sse4_1;
  WrappedEft sse2;
  Jit64 jit;

private:
  WrappedEft Wrap()
  {
    using namespace Gen;

    const u8* raw = AlignCode4();
    GeneratePsMaddEft();

    const auto wrapped = reinterpret_cast<WrappedEft>(AlignCode4());
    ABI_PushRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
    MOVUPD(XMM0, MDisp(ABI_PARAM1, 0));
    MOVUPD(XMM1, MDisp(ABI_PARAM1, 16));
    MOVUPD(XMM2, MDisp(ABI_PARAM1, 32));
    ABI_CallFunction(reinterpret_cast<void (*)()>(raw));
    MOVUPD(MDisp(ABI_PARAM2, 0), XMM1);
    ABI_PopRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
    RET();
    return wrapped;
  }
};

u64 Bits(double d)
{
  return std::bit_cast<u64>(d);
}
}  // namespace

// Without SSE4.1 the even-tie test compared 16-bit words: a result whose low 13 bits were 0 but
// was no even tie got half a mask, and was rounded anyway (bug #189: a PC without SSE4.1 desynced
// against every other).
TEST(Jit64, PsMaddEftSameWithoutSSE41)
{
  if (!cpu_info.bSSE4_1)
    GTEST_SKIP() << "the host has no SSE4.1";

  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_thread_guard([] { Core::UndeclareAsCPUThread(); });

  const TestCommonAsmRoutines routines(Core::System::GetInstance());

  const auto check = [&](const std::array<u64, 6>& in) {
    std::array<u64, 2> a{};
    std::array<u64, 2> b{};
    routines.sse4_1(in.data(), a.data());
    routines.sse2(in.data(), b.data());
    EXPECT_EQ(a, b) << fmt::format("XMM0 {:016x} {:016x}, XMM1 {:016x} {:016x}, XMM2 {:016x} {:016x}",
                                   in[0], in[1], in[2], in[3], in[4], in[5]);
    return a;
  };

  // Low 13 bits 0, bit 13 set, an error: no even tie, so the result stays as it is.
  const u64 not_a_tie = 0x3FF0'0000'0000'2000;
  const auto kept =
      check({Bits(0x1p-60), Bits(0x1p-60), not_a_tie, not_a_tie, Bits(0.0), Bits(0.0)});
  EXPECT_EQ(kept[0], not_a_tie);
  EXPECT_EQ(kept[1], not_a_tie);

  // Its upper word matching instead (bit 28 set), a low bit too: no even tie either.
  const u64 upper_only = 0x3FF0'0000'1000'0001;
  const auto kept2 =
      check({Bits(0x1p-60), Bits(-0x1p-60), upper_only, upper_only, Bits(0.0), Bits(0.0)});
  EXPECT_EQ(kept2[0], upper_only);
  EXPECT_EQ(kept2[1], upper_only);

  // An even tie with an error is rounded by one (down for a positive error on a positive result).
  const u64 tie = 0x3FF0'0000'1000'0000;
  const auto rounded = check({Bits(0x1p-60), Bits(-0x1p-60), tie, tie, Bits(0.0), Bits(0.0)});
  EXPECT_NE(rounded[0], tie);
  EXPECT_NE(rounded[1], tie);

  // Results with their low bits cleared to every length, ties among them, and errors of both
  // signs, or none.
  std::mt19937_64 rng(189);
  const std::array<double, 5> errors{0.0, 0x1p-60, -0x1p-60, 0x1p-30, -0x1p-90};
  for (int i = 0; i < 20000; ++i)
  {
    std::array<u64, 6> in{};
    for (int lane = 0; lane < 2; ++lane)
    {
      u64 result = rng();
      const unsigned cleared = rng() % 34;
      if (cleared != 0)
        result &= ~0ULL << cleared;
      if (rng() % 4 == 0)
        result = (result & ~0x1FFF'FFFFULL) | 0x1000'0000;
      in[2 + lane] = result;
      in[lane] = Bits(errors[rng() % errors.size()]);
      in[4 + lane] = Bits(errors[rng() % errors.size()]);
    }
    check(in);
  }
}
