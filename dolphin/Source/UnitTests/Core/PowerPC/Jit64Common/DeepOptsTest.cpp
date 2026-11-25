// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for Jit64 deep optimization invariants:
// 1. WidenSingleToDouble (SNaN preservation, dst==src vs dst!=src, far-code & inline, SSE4.1 & SSE2,
//    RSCRATCH and XMM1 preservation).
// 2. Native single-precision FMA (VFMADD/VFMSUB SS/PS) + NaN precedence and FTZ parity with the
//    Interpreter's single-precision madd/msub semantics.
// 3. Branchless FloatCompare index & CR table parity across finite values, signed zeros, infinities,
//    and NaNs.
// 4. Static cross-block GPR (r1/r3/r4/r31) and FPR (f0-f3) pinning invariants, Store/Load round-trip,
//    deferred immediate materialization, and single-to-double normalization on Flush.

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "Common/BitSet.h"
#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/FloatUtils.h"
#include "Common/ScopeGuard.h"
#include "Common/x64ABI.h"
#include "Common/x64Emitter.h"
#include "Core/Core.h"
#include "Core/PowerPC/ConditionRegister.h"
#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64/RegCache/FPURegCache.h"
#include "Core/PowerPC/Jit64/RegCache/GPRRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64AsmCommon.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/System.h"

#include "../TestValues.h"

// Include gtest after emitter headers so gtest's TEST macro does not shadow XEmitter::TEST.
#include <gtest/gtest.h>

namespace
{
class TestEftRoutines : public CommonAsmRoutines
{
public:
  explicit TestEftRoutines(Jit64& jit) : CommonAsmRoutines(jit)
  {
    AllocCodeSpace(1 << 16);
    m_const_pool.Init(AllocChildCodeSpace(4096), 4096);
    fmadds_eft = AlignCode4();
    GenerateFmaddsEft();
    ps_madd_eft = AlignCode4();
    GeneratePsMaddEft();
  }
  ~TestEftRoutines() override { FreeCodeSpace(); }
};

class TestJit64 : public Jit64
{
public:
  explicit TestJit64(Core::System& system) : Jit64(system), eft_routines(*this)
  {
    const size_t farcode_size = 1 << 16;
    const size_t constpool_size = 4096;
    AllocCodeSpace((1 << 18) + farcode_size + constpool_size);
    AddChildCodeSpace(&m_far_code, farcode_size);
    m_const_pool.Init(AllocChildCodeSpace(constpool_size), constpool_size);
    ResetCodePtr();
    m_far_code.Init();
  }

  ~TestJit64() override { FreeCodeSpace(); }

  GPRRegCache test_gpr{*this, *this};
  FPURegCache test_fpr{*this, *this};
  TestEftRoutines eft_routines;
  using Jit64::m_far_code;
  using Jit64::m_ppc_state;
  using Jit64::WidenSingleToDouble;
};

struct WidenOut
{
  u64 ps0;
  u64 ps1;
  u64 rscratch_after;
  u64 xmm1_lo_after;
  u64 xmm1_hi_after;
};

using WidenFn = void (*)(const u32* in_lanes, WidenOut* out);

WidenFn EmitWidenHarness(TestJit64& jit, bool dst_eq_src, bool is_pair, bool may_be_snan,
                         bool in_far_code)
{
  using namespace Gen;
  if (in_far_code)
    jit.SwitchToFarCode();
  const auto fn = reinterpret_cast<WidenFn>(jit.AlignCode16());
  jit.ABI_PushRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
  jit.MOV(64, R(R12), R(ABI_PARAM1));
  jit.MOV(64, R(R13), R(ABI_PARAM2));

  // Load 4 float lanes into XMM4 (src), and fill XMM5 (dst if different), XMM1, and RSCRATCH with
  // sentinels so we verify neither XMM1 nor RSCRATCH is clobbered.
  jit.MOVUPS(XMM4, MatR(R12));
  if (!dst_eq_src)
    jit.PCMPEQD(XMM5, R(XMM5));
  jit.MOV(64, R(RSCRATCH), Imm64(0xDEADBEEF12345678ULL));
  jit.MOVQ_xmm(XMM1, R(RSCRATCH));
  jit.MOVDDUP(XMM1, R(XMM1));

  const X64Reg dst = dst_eq_src ? XMM4 : XMM5;
  jit.WidenSingleToDouble(dst, XMM4, is_pair, may_be_snan);

  jit.MOVUPD(MDisp(R13, 0), dst);
  jit.MOV(64, MDisp(R13, 16), R(RSCRATCH));
  jit.MOVUPD(MDisp(R13, 24), XMM1);

  jit.ABI_PopRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
  jit.RET();
  if (in_far_code)
    jit.SwitchToNearCode();
  return fn;
}

alignas(16) const u64 kFcmpCrTable[4] = {
    PowerPC::ConditionRegister::PPCToInternal(PowerPC::CR_EQ),
    PowerPC::ConditionRegister::PPCToInternal(PowerPC::CR_LT),
    PowerPC::ConditionRegister::PPCToInternal(PowerPC::CR_GT),
    PowerPC::ConditionRegister::PPCToInternal(PowerPC::CR_SO),
};

using FcmpPtrFn = u64 (*)(const u32* a_bits, const u32* b_bits, u32* out_idx);

FcmpPtrFn EmitBranchlessFcmpPtr(TestJit64& jit)
{
  using namespace Gen;
  const auto fn = reinterpret_cast<FcmpPtrFn>(jit.AlignCode16());
  jit.ABI_PushRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
  jit.MOV(64, R(R12), R(ABI_PARAM1));
  jit.MOV(64, R(R13), R(ABI_PARAM2));
  jit.MOV(64, R(R14), R(ABI_PARAM3));

  jit.MOVD_xmm(XMM4, MatR(R12));  // Ra
  jit.MOVD_xmm(XMM5, MatR(R13));  // Rb

  jit.XOR(32, R(RSCRATCH), R(RSCRATCH));
  jit.XOR(32, R(RSCRATCH2), R(RSCRATCH2));
  jit.UCOMISS(XMM5, R(XMM4));
  jit.SETcc(CC_A, R(RSCRATCH));
  jit.SETcc(CC_B, R(RSCRATCH2));
  jit.LEA(32, RSCRATCH, MComplex(RSCRATCH, RSCRATCH2, SCALE_2, 0));
  jit.SETcc(CC_P, R(RSCRATCH2));
  jit.ADD(32, R(RSCRATCH), R(RSCRATCH2));
  jit.MOV(32, MatR(R14), R(RSCRATCH));

  jit.MOV(64, R(RSCRATCH2), ImmPtr(kFcmpCrTable));
  jit.MOV(64, R(ABI_RETURN), MComplex(RSCRATCH2, RSCRATCH, SCALE_8, 0));

  jit.ABI_PopRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
  jit.RET();
  return fn;
}

using SingleFmaFn = void (*)(const u32* in_abc, u32 mxcsr, u32* out_res);

SingleFmaFn EmitSingleFma(TestJit64& jit, bool packed, bool subtract, bool negate)
{
  using namespace Gen;
  alignas(16) static const u32 sign_single[4] = {0x80000000u, 0, 0, 0};
  alignas(16) static const u32 sign_pair[4] = {0x80000000u, 0x80000000u, 0, 0};

  const auto fn = reinterpret_cast<SingleFmaFn>(jit.AlignCode16());
  jit.ABI_PushRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
  jit.MOV(64, R(R12), R(ABI_PARAM1));
  jit.MOV(32, R(R13), R(ABI_PARAM2));
  jit.MOV(64, R(R14), R(ABI_PARAM3));

  jit.SUB(64, R(RSP), Imm8(16));
  jit.STMXCSR(MDisp(RSP, 0));
  jit.MOV(32, MDisp(RSP, 4), R(R13));
  jit.LDMXCSR(MDisp(RSP, 4));

  jit.MOVUPS(XMM4, MDisp(R12, 0));   // Ra (4 lanes)
  jit.MOVUPS(XMM5, MDisp(R12, 16));  // Rb (4 lanes)
  jit.MOVUPS(XMM6, MDisp(R12, 32));  // Rc (4 lanes)
  jit.MOVAPS(XMM1, R(XMM6));
  if (subtract)
  {
    if (packed)
      jit.VFMSUB132PS(XMM1, XMM5, R(XMM4));
    else
      jit.VFMSUB132SS(XMM1, XMM5, R(XMM4));
  }
  else
  {
    if (packed)
      jit.VFMADD132PS(XMM1, XMM5, R(XMM4));
    else
      jit.VFMADD132SS(XMM1, XMM5, R(XMM4));
  }
  if (negate)
  {
    jit.MOV(64, R(RSCRATCH), ImmPtr(packed ? sign_pair : sign_single));
    jit.XORPS(XMM1, MatR(RSCRATCH));
  }

  jit.MOVUPS(MatR(R14), XMM1);
  jit.LDMXCSR(MDisp(RSP, 0));
  jit.ADD(64, R(RSP), Imm8(16));
  jit.ABI_PopRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
  jit.RET();
  return fn;
}

// Emits the pre-change Jit64 double-precision FMA + EFT (fmadds_eft / ps_madd_eft) + round-to-float
// path for single-precision inputs, so SinglePrecisionFMAExactness verifies exact parity against
// both the old Jit64 path (all MXCSR modes including FTZ) and the Interpreter.
SingleFmaFn EmitOldDoubleEftFma(TestJit64& jit, bool packed, bool subtract, bool negate)
{
  using namespace Gen;
  alignas(16) static const u64 sign_single[2] = {0x8000000000000000ULL, 0ULL};
  alignas(16) static const u64 sign_pair[2] = {0x8000000000000000ULL, 0x8000000000000000ULL};

  const auto fn = reinterpret_cast<SingleFmaFn>(jit.AlignCode16());
  jit.ABI_PushRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
  jit.MOV(64, R(R12), R(ABI_PARAM1));
  jit.MOV(32, R(R13), R(ABI_PARAM2));
  jit.MOV(64, R(R14), R(ABI_PARAM3));

  jit.SUB(64, R(RSP), Imm8(16));
  jit.STMXCSR(MDisp(RSP, 0));
  jit.MOV(32, MDisp(RSP, 4), R(R13));
  jit.LDMXCSR(MDisp(RSP, 4));

  jit.MOVUPS(XMM4, MDisp(R12, 0));
  jit.MOVUPS(XMM5, MDisp(R12, 16));
  jit.MOVUPS(XMM6, MDisp(R12, 32));
  jit.CVTPS2PD(XMM4, R(XMM4));  // Ra as double pair
  jit.CVTPS2PD(XMM5, R(XMM5));  // Rb as double pair
  jit.CVTPS2PD(XMM6, R(XMM6));  // Rc as double pair

  jit.MOVAPD(XMM1, R(XMM6));
  if (subtract)
  {
    if (packed)
      jit.VFMSUB132PD(XMM1, XMM5, R(XMM4));
    else
      jit.VFMSUB132SD(XMM1, XMM5, R(XMM4));
  }
  else
  {
    if (packed)
      jit.VFMADD132PD(XMM1, XMM5, R(XMM4));
    else
      jit.VFMADD132SD(XMM1, XMM5, R(XMM4));
  }

  if (packed)
  {
    if (subtract)
      jit.VADDPD(XMM0, XMM1, R(XMM5));
    else
      jit.VSUBPD(XMM0, XMM1, R(XMM5));
    jit.VSUBPD(XMM2, XMM1, R(XMM0));
    jit.VFMSUB231PD(XMM0, XMM6, R(XMM4));
    if (subtract)
      jit.VADDPD(XMM2, XMM2, R(XMM5));
    else
      jit.VSUBPD(XMM2, XMM2, R(XMM5));
    jit.CALL(jit.eft_routines.ps_madd_eft);
  }
  else
  {
    if (subtract)
      jit.VADDSD(XMM0, XMM1, R(XMM5));
    else
      jit.VSUBSD(XMM0, XMM1, R(XMM5));
    jit.VSUBSD(XMM2, XMM1, R(XMM0));
    jit.VFMSUB231SD(XMM0, XMM6, R(XMM4));
    if (subtract)
      jit.ADDSD(XMM2, R(XMM5));
    else
      jit.SUBSD(XMM2, R(XMM5));
    jit.CALL(jit.eft_routines.fmadds_eft);
  }

  if (negate)
  {
    jit.MOV(64, R(RSCRATCH), ImmPtr(packed ? sign_pair : sign_single));
    jit.XORPD(XMM1, MatR(RSCRATCH));
  }

  if (packed)
    jit.CVTPD2PS(XMM1, R(XMM1));
  else
    jit.CVTSD2SS(XMM1, R(XMM1));

  jit.MOVUPS(MatR(R14), XMM1);
  jit.LDMXCSR(MDisp(RSP, 0));
  jit.ADD(64, R(RSP), Imm8(16));
  jit.ABI_PopRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
  jit.RET();
  return fn;
}

}  // namespace

TEST(Jit64, WidenSingleToDoubleMatchesConvertToDouble)
{
  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_thread_guard([] { Core::UndeclareAsCPUThread(); });

  const CPUInfo saved_cpu = cpu_info;
  Common::ScopeGuard cpu_guard([&] { cpu_info = saved_cpu; });

  std::vector<u32> samples = {
      0x00000000u, 0x80000000u,  // +/- 0
      0x00000001u, 0x80000001u,  // min subnormal
      0x007FFFFFu, 0x807FFFFFu,  // max subnormal
      0x00800000u, 0x80800000u,  // min normal
      0x3F800000u, 0xBF800000u,  // +/- 1.0
      0x7F7FFFFFu, 0xFF7FFFFFu,  // max normal
      0x7F800000u, 0xFF800000u,  // +/- Inf
      0x7FC00000u, 0xFFC00000u,  // canonical +/- QNaN
      0x7FC12345u, 0xFFD54321u,  // payload +/- QNaN
      0x7F800001u, 0xFF800001u,  // +/- SNaN (low bit)
      0x7FBFFFFFu, 0xFFBFFFFFu,  // +/- SNaN (all payload bits)
  };

  for (const bool sse41 : {saved_cpu.bSSE4_1, false})
  {
    cpu_info.bSSE4_1 = sse41;
    TestJit64 jit(Core::System::GetInstance());

    for (const bool dst_eq_src : {false, true})
    {
      for (const bool in_far_code : {false, true})
      {
        const WidenFn fn_snan =
            EmitWidenHarness(jit, dst_eq_src, /*is_pair=*/false, /*may_be_snan=*/true, in_far_code);
        const WidenFn fn_dup = EmitWidenHarness(jit, dst_eq_src, /*is_pair=*/false,
                                                /*may_be_snan=*/false, in_far_code);
        const WidenFn fn_pair = EmitWidenHarness(jit, dst_eq_src, /*is_pair=*/true,
                                                 /*may_be_snan=*/false, in_far_code);

        for (size_t i = 0; i < samples.size(); ++i)
        {
          const u32 lo = samples[i];
          const u32 hi = samples[(i * 7 + 3) % samples.size()];
          alignas(16) const u32 in_lanes[4] = {lo, hi, 0x7F800001u, 0xFF800001u};

          // 1. may_be_snan=true: must preserve SNaN bit and duplicate into both double lanes.
          WidenOut out{};
          fn_snan(in_lanes, &out);
          const u64 expected_snan = ConvertToDouble(lo);
          EXPECT_EQ(out.ps0, expected_snan)
              << "lo=" << std::hex << lo << " sse41=" << sse41 << " dst_eq_src=" << dst_eq_src;
          EXPECT_EQ(out.ps1, expected_snan);
          EXPECT_EQ(out.rscratch_after, 0xDEADBEEF12345678ULL);
          EXPECT_EQ(out.xmm1_lo_after, 0xDEADBEEF12345678ULL);
          EXPECT_EQ(out.xmm1_hi_after, 0xDEADBEEF12345678ULL);

          // 2. may_be_snan=false, duplicated: skip SNaN inputs (guaranteed quieted).
          const bool lo_is_snan = (lo & 0x7FC00000u) == 0x7F800000u && (lo & 0x003FFFFFu) != 0;
          const bool hi_is_snan = (hi & 0x7FC00000u) == 0x7F800000u && (hi & 0x003FFFFFu) != 0;
          if (!lo_is_snan)
          {
            fn_dup(in_lanes, &out);
            EXPECT_EQ(out.ps0, ConvertToDouble(lo));
            EXPECT_EQ(out.ps1, ConvertToDouble(lo));
            EXPECT_EQ(out.rscratch_after, 0xDEADBEEF12345678ULL);
            EXPECT_EQ(out.xmm1_lo_after, 0xDEADBEEF12345678ULL);
          }

          // 3. may_be_snan=false, pair: independent lanes, upper lanes 2-3 ignored.
          if (!lo_is_snan && !hi_is_snan)
          {
            fn_pair(in_lanes, &out);
            EXPECT_EQ(out.ps0, ConvertToDouble(lo));
            EXPECT_EQ(out.ps1, ConvertToDouble(hi));
            EXPECT_EQ(out.rscratch_after, 0xDEADBEEF12345678ULL);
            EXPECT_EQ(out.xmm1_lo_after, 0xDEADBEEF12345678ULL);
          }
        }
      }
    }
  }
}

TEST(Jit64, SinglePrecisionFMAExactness)
{
  if (!cpu_info.bFMA)
    GTEST_SKIP() << "Host CPU does not support FMA";

  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_thread_guard([] { Core::UndeclareAsCPUThread(); });

  TestJit64 jit(Core::System::GetInstance());

  constexpr u32 kMxcsrDefault = 0x1F80u;      // Round-to-nearest, all exceptions masked, FTZ=0, DAZ=0
  constexpr u32 kMxcsrFTZ = 0x1F80u | 0x8000u;  // FTZ=1, DAZ=0 (matches Dolphin s_fpscr_to_mxcsr)

  // Curated single-precision edge values + random values + double-rounding tie generators.
  std::vector<u32> pool = {
      0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu,
      0x00800000u, 0x80800000u, 0x3F800000u, 0xBF800000u, 0x3F000000u, 0xBF000000u,
      0x34000000u, 0xB4000000u, 0x3F800001u, 0x3F800003u, 0x33800001u, 0x2F800001u,
      0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u,
  };
  std::mt19937 rng(0xC0FFEEu);
  for (int i = 0; i < 64; ++i)
  {
    const u32 bits = rng();
    if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0)
      continue;  // finite / inf / zero tested here; NaNs checked separately
    pool.push_back(bits);
  }

  // Known single-precision inputs where double-precision FMA + round-to-float double-rounds
  // differently from true single-rounding FMA (a*c + b lands on an even tie in float32).
  const std::array<std::array<u32, 3>, 4> tie_triples = {{
      {0x3F800001u, 0x33800000u, 0x3F800001u},  // (1 + 2^-23)^2 + 2^-24
      {0x3F800001u, 0xB3800000u, 0x3F800001u},  // (1 + 2^-23)^2 - 2^-24
      {0x3F800003u, 0x33800000u, 0x3F800001u},  // (1 + 3*2^-23)*(1 + 2^-23) + 2^-24
      {0x3F800001u, 0x34000001u, 0x3F800001u},  // near-halfway tie with non-zero residual
  }};

  auto& ppc = jit.m_ppc_state;
  const u32 saved_fpscr = ppc.fpscr.Hex;
  Common::ScopeGuard fpscr_guard([&] { ppc.fpscr.Hex = saved_fpscr; });

  for (const bool packed : {false, true})
  {
    for (const bool subtract : {false, true})
    {
      for (const bool negate : {false, true})
      {
        const SingleFmaFn fn = EmitSingleFma(jit, packed, subtract, negate);
        const SingleFmaFn old_fn = EmitOldDoubleEftFma(jit, packed, subtract, negate);
        for (const u32 mxcsr : {kMxcsrDefault, kMxcsrFTZ})
        {
          const bool ftz = (mxcsr & 0x8000u) != 0;
          ppc.fpscr.Hex = 0;
          ppc.fpscr.NI = ftz ? 1 : 0;

          const int total_iters = 500 + static_cast<int>(tie_triples.size());
          for (int iter = 0; iter < total_iters; ++iter)
          {
            alignas(16) u32 in_abc[12]{};
            for (int lane = 0; lane < 2; ++lane)
            {
              if (iter < static_cast<int>(tie_triples.size()))
              {
                in_abc[lane] = tie_triples[iter][0];
                in_abc[4 + lane] = tie_triples[iter][1];
                in_abc[8 + lane] = tie_triples[iter][2];
              }
              else
              {
                in_abc[lane] = pool[rng() % pool.size()];
                in_abc[4 + lane] = pool[rng() % pool.size()];
                in_abc[8 + lane] = pool[rng() % pool.size()];
              }
            }
            // Put SNaNs in unused upper lanes 2-3 to prove they don't pollute lanes 0-1.
            in_abc[2] = in_abc[3] = in_abc[6] = in_abc[7] = in_abc[10] = in_abc[11] = 0x7F800001u;

            alignas(16) u32 out[4]{};
            alignas(16) u32 old_out[4]{};
            fn(in_abc, mxcsr, out);
            old_fn(in_abc, mxcsr, old_out);

            const int lanes = packed ? 2 : 1;
            for (int lane = 0; lane < lanes; ++lane)
            {
              if (std::isnan(std::bit_cast<float>(old_out[lane])))
              {
                // Generated NaN (0*inf or inf-inf) is handled by HandleNaNsSingle in Jit64.
                EXPECT_TRUE(std::isnan(std::bit_cast<float>(out[lane])));
                continue;
              }

              // 1. Exact bit parity with the pre-change Jit64 double+EFT path (both FTZ=0 and 1).
              EXPECT_EQ(out[lane], old_out[lane])
                  << "vs old double+EFT: packed=" << packed << " sub=" << subtract
                  << " neg=" << negate << " ftz=" << ftz << " a=" << std::hex << in_abc[lane]
                  << " b=" << in_abc[4 + lane] << " c=" << in_abc[8 + lane];

              // 2. Exact bit parity with Dolphin's Interpreter (NI_madd/NI_msub + ForceSingle) for
              //    non-FTZ (FTZ before-rounding tininess is a pre-existing Jit64/x86 difference
              //    shared identically by both old_fn and fn).
              if (!ftz)
              {
                const double da = std::bit_cast<double>(ConvertToDouble(in_abc[lane]));
                const double db = std::bit_cast<double>(ConvertToDouble(in_abc[4 + lane]));
                const double dc = std::bit_cast<double>(ConvertToDouble(in_abc[8 + lane]));
                const FPResult interp_res =
                    subtract ? NI_msub<true>(ppc, da, dc, db) : NI_madd<true>(ppc, da, dc, db);
                float expected_f = ForceSingle(ppc.fpscr, interp_res.value);
                if (negate)
                  expected_f = -expected_f;
                EXPECT_EQ(out[lane], std::bit_cast<u32>(expected_f))
                    << "vs Interpreter: packed=" << packed << " sub=" << subtract
                    << " neg=" << negate << " a=" << std::hex << in_abc[lane]
                    << " b=" << in_abc[4 + lane] << " c=" << in_abc[8 + lane];
              }
            }
          }
        }
      }
    }
  }
}

TEST(Jit64, BranchlessFloatCompareMatchesSemantics)
{
  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_thread_guard([] { Core::UndeclareAsCPUThread(); });

  TestJit64 jit(Core::System::GetInstance());
  const FcmpPtrFn fn = EmitBranchlessFcmpPtr(jit);

  const std::array<u32, 10> vals = {
      0xFF800000u,  // -Inf
      0xBF800000u,  // -1.0
      0x80000000u,  // -0.0
      0x00000000u,  // +0.0
      0x00000001u,  // +min subnormal
      0x3F800000u,  // +1.0
      0x7F800000u,  // +Inf
      0x7FC00000u,  // +QNaN
      0xFFC00000u,  // -QNaN
      0x7F800001u,  // +SNaN
  };

  for (const u32 a_bits : vals)
  {
    for (const u32 b_bits : vals)
    {
      u32 idx = 99;
      const u64 cr_internal = fn(&a_bits, &b_bits, &idx);

      const float a = std::bit_cast<float>(a_bits);
      const float b = std::bit_cast<float>(b_bits);
      u32 expected_idx;
      u32 expected_cr_flag;
      if (std::isnan(a) || std::isnan(b))
      {
        expected_idx = 3;
        expected_cr_flag = PowerPC::CR_SO;
      }
      else if (a < b)
      {
        expected_idx = 1;
        expected_cr_flag = PowerPC::CR_LT;
      }
      else if (a > b)
      {
        expected_idx = 2;
        expected_cr_flag = PowerPC::CR_GT;
      }
      else
      {
        expected_idx = 0;
        expected_cr_flag = PowerPC::CR_EQ;
      }

      EXPECT_EQ(idx, expected_idx) << "a=" << std::hex << a_bits << " b=" << b_bits;
      EXPECT_EQ(cr_internal, PowerPC::ConditionRegister::PPCToInternal(expected_cr_flag))
          << "a=" << std::hex << a_bits << " b=" << b_bits;
    }
  }
}

TEST(Jit64, FcmpCrorBranchMaskExactness)
{
  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_thread_guard([] { Core::UndeclareAsCPUThread(); });

  TestJit64 jit(Core::System::GetInstance());
  using namespace Gen;
  using MaskFn = u32 (*)(u32 idx);

  // Exhaustively verify every 4-bit match_mask (0..15) x condition (0..1) x idx (0..3)
  // through the exact x86-64 test sequences emitted by FloatCompare (including fused cror).
  for (u8 match_mask = 0; match_mask < 16; ++match_mask)
  {
    for (const bool condition : {false, true})
    {
      const auto fn = reinterpret_cast<MaskFn>(jit.AlignCode16());
      jit.MOV(32, R(RSCRATCH), R(ABI_PARAM1));
      FixupBranch pDontBranch;
      if (match_mask == 0x1)
      {
        (jit.TEST)(32, R(RSCRATCH), R(RSCRATCH));
        pDontBranch = jit.J_CC(condition ? CC_NE : CC_E, XEmitter::Jump::Near);
      }
      else if (match_mask == 0x2 || match_mask == 0x4 || match_mask == 0x8)
      {
        jit.CMP(32, R(RSCRATCH), Imm8(static_cast<u8>(std::countr_zero(match_mask))));
        pDontBranch = jit.J_CC(condition ? CC_NE : CC_E, XEmitter::Jump::Near);
      }
      else if (match_mask == 0x3)
      {
        jit.CMP(32, R(RSCRATCH), Imm8(1));
        pDontBranch = jit.J_CC(condition ? CC_A : CC_BE, XEmitter::Jump::Near);
      }
      else if (match_mask == 0x5)
      {
        (jit.TEST)(8, R(RSCRATCH), Imm8(1));
        pDontBranch = jit.J_CC(condition ? CC_NZ : CC_Z, XEmitter::Jump::Near);
      }
      else
      {
        jit.MOV(32, R(RSCRATCH2), Imm32(match_mask));
        jit.BT(32, R(RSCRATCH2), R(RSCRATCH));
        pDontBranch = jit.J_CC(condition ? CC_NC : CC_C, XEmitter::Jump::Near);
      }
      jit.MOV(32, R(ABI_RETURN), Imm32(1));
      jit.RET();
      jit.SetJumpTarget(pDontBranch);
      jit.XOR(32, R(ABI_RETURN), R(ABI_RETURN));
      jit.RET();

      for (u32 idx = 0; idx < 4; ++idx)
      {
        const bool bit_set = ((match_mask >> idx) & 1u) != 0;
        const u32 expected = (bit_set == condition) ? 1u : 0u;
        EXPECT_EQ(fn(idx), expected)
            << "mask=" << static_cast<int>(match_mask) << " cond=" << condition << " idx=" << idx;
      }
    }
  }
}

TEST(Jit64, PinnedRegCacheInvariantsAndRoundTrip)
{
  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_thread_guard([] { Core::UndeclareAsCPUThread(); });

  TestJit64 jit(Core::System::GetInstance());

  // 1. Verify pinned GPRs and FPRs are active on Start() and disjoint from allocatable pools.
  jit.test_gpr.Start();
  jit.test_fpr.Start();

  EXPECT_EQ(jit.test_gpr.GetPinnedRegisters().size(), 4u);
  EXPECT_EQ(jit.test_fpr.GetPinnedRegisters().size(), 4u);

  for (const auto& [guest, host] : jit.test_gpr.GetPinnedRegisters())
  {
    EXPECT_TRUE(jit.test_gpr.IsPinned(guest));
    EXPECT_EQ(jit.test_gpr.GetPinnedHostRegister(guest), host);
    EXPECT_TRUE(jit.test_gpr.HostRegistersInUse()[host]);
  }
  for (const auto& [guest, host] : jit.test_fpr.GetPinnedRegisters())
  {
    EXPECT_TRUE(jit.test_fpr.IsPinned(guest));
    EXPECT_EQ(jit.test_fpr.GetPinnedHostRegister(guest), host);
    EXPECT_TRUE(jit.test_fpr.HostRegistersInUse()[host]);
  }

  // 2. Emit code that loads pinned GPRs & FPRs from PPCState, exercises:
  //    - deferred immediate on pinned GPR r3 -> Flush(Full) materializes into R13
  //    - single-precision SNaN on pinned FPR f1 -> StorePinnedToPPCStateNoMutate preserves f1 as
  //      single while writing canonical double to PPCState, then Flush(Full) normalizes f1 in-place
  //    - StorePinnedToPPCState writes all pinned GPRs & FPRs back to PPCState
  using namespace Gen;
  using RoundTripFn = void (*)(void* ppc_state_plus_80);
  const auto fn = reinterpret_cast<RoundTripFn>(jit.AlignCode16());
  jit.ABI_PushRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
  jit.MOV(64, R(RPPCSTATE), R(ABI_PARAM1));

  GPRRegCache::LoadPinnedFromPPCState(jit);
  FPURegCache::LoadPinnedFromPPCState(jit);

  jit.test_gpr.Start();
  jit.test_fpr.Start();

  // Put a deferred immediate in pinned r3.
  jit.test_gpr.SetImmediate32(3, 0xCAFEBABEu);
  EXPECT_TRUE(jit.test_gpr.IsImm(3));

  // Load a single-precision SNaN into pinned f1 and mark it SingleDuplicated + may_be_snan + raw.
  {
    RCX64Reg f1 = jit.test_fpr.BindSingle(1, RCMode::Write, /*want_pair=*/false);
    RegCache::Realize(f1);
    jit.MOV(32, R(RSCRATCH), Imm32(0x7F800042u));
    jit.MOVD_xmm(f1, R(RSCRATCH));
    jit.test_fpr.SetSingle(1, /*is_pair=*/false, /*may_be_snan=*/true, /*raw_load=*/true);
  }
  EXPECT_TRUE(jit.test_fpr.IsSingle(1));
  EXPECT_TRUE(jit.test_fpr.MayBeSNaN(1));
  EXPECT_TRUE(jit.test_fpr.IsRawSingleLoad(1));

  // Verify psq_l-style SinglePair raw load sets IsRawSingleLoad without setting MayBeSNaN,
  // while arithmetic single output clears IsRawSingleLoad.
  {
    RCX64Reg f6 = jit.test_fpr.BindSingle(6, RCMode::Write, /*want_pair=*/true);
    RegCache::Realize(f6);
    jit.test_fpr.SetSingle(6, /*is_pair=*/true, /*may_be_snan=*/false, /*raw_load=*/true);
    EXPECT_TRUE(jit.test_fpr.IsRawSingleLoad(6));
    EXPECT_FALSE(jit.test_fpr.MayBeSNaN(6));
    jit.test_fpr.SetSingle(6, /*is_pair=*/true, /*may_be_snan=*/false, /*raw_load=*/false);
    EXPECT_FALSE(jit.test_fpr.IsRawSingleLoad(6));
    jit.test_fpr.ConvertToDouble(6);
    EXPECT_FALSE(jit.test_fpr.IsSingle(6));
    EXPECT_FALSE(jit.test_fpr.IsRawSingleLoad(6));
  }

  // StorePinnedToPPCStateNoMutate must widen f1 into PPCState without mutating f1's single state.
  jit.test_fpr.StorePinnedToPPCStateNoMutate(BitSet32{1});
  EXPECT_TRUE(jit.test_fpr.IsSingle(1));
  EXPECT_TRUE(jit.test_fpr.MayBeSNaN(1));
  EXPECT_TRUE(jit.test_fpr.IsRawSingleLoad(1));

  // Full Flush must materialize r3 into R13 and widen f1 in-place to canonical double.
  jit.test_gpr.Flush();
  jit.test_fpr.Flush();
  EXPECT_FALSE(jit.test_fpr.IsSingle(1));
  EXPECT_FALSE(jit.test_fpr.IsRawSingleLoad(1));

  GPRRegCache::StorePinnedToPPCState(jit);
  FPURegCache::StorePinnedToPPCState(jit);

  jit.ABI_PopRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
  jit.RET();

  // Seed PPCState with known patterns.
  auto& ppc = jit.m_ppc_state;
  ppc.gpr[1] = 0x81234560u;
  ppc.gpr[3] = 0x11111111u;
  ppc.gpr[4] = 0x22222222u;
  ppc.gpr[31] = 0x80555550u;
  ppc.ps[0].SetBoth(u64{0x3FF0000000000000ULL}, u64{0xBFF0000000000000ULL});
  ppc.ps[1].SetBoth(u64{0}, u64{0});
  ppc.ps[2].SetBoth(u64{0x7FF0000000000001ULL}, u64{0xFFF0000000000002ULL});  // SNaNs
  ppc.ps[3].SetBoth(u64{0x8000000000000000ULL}, u64{0x400921FB54442D18ULL});

  fn(reinterpret_cast<u8*>(&ppc) + 0x80);

  EXPECT_EQ(ppc.gpr[1], 0x81234560u);
  EXPECT_EQ(ppc.gpr[3], 0xCAFEBABEu);
  EXPECT_EQ(ppc.gpr[4], 0x22222222u);
  EXPECT_EQ(ppc.gpr[31], 0x80555550u);

  EXPECT_EQ(ppc.ps[0].PS0AsU64(), 0x3FF0000000000000ULL);
  EXPECT_EQ(ppc.ps[0].PS1AsU64(), 0xBFF0000000000000ULL);
  const u64 expected_f1 = ConvertToDouble(0x7F800042u);
  EXPECT_EQ(ppc.ps[1].PS0AsU64(), expected_f1);
  EXPECT_EQ(ppc.ps[1].PS1AsU64(), expected_f1);
  EXPECT_EQ(ppc.ps[2].PS0AsU64(), 0x7FF0000000000001ULL);
  EXPECT_EQ(ppc.ps[2].PS1AsU64(), 0xFFF0000000000002ULL);
  EXPECT_EQ(ppc.ps[3].PS0AsU64(), 0x8000000000000000ULL);
  EXPECT_EQ(ppc.ps[3].PS1AsU64(), 0x400921FB54442D18ULL);
}

TEST(Jit64, BranchlessDiamondAndStoreOverwriteExactness)
{
  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_thread_guard([] { Core::UndeclareAsCPUThread(); });

  TestJit64 jit(Core::System::GetInstance());
  using namespace Gen;

  // 1. Verify the zero-stall TEST(32, Rd, Rd) + CMOVcc + SETcc downcount sequence matches
  //    PPCSTATE_CR(crf) semantics across all CR bits (LT, GT, EQ) x branch_if_true x body_cycles
  //    (1, 2, 3) and edge 32-bit signed values (0, 1, -1, INT32_MIN, INT32_MAX, etc.).
  struct DiamondOut
  {
    u32 rd;
    u32 stored;
    s32 downcount;
  };
  using DiamondFn = void (*)(u32 init_rd, u32 init_val0, u32 cand, s32 init_dc, DiamondOut* out);

  const std::array<u32, 9> test_vals = {
      0x00000000u, 0x00000001u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
      0x80000001u, 0x12345678u, 0xFEDCBA98u, 0x00008000u,
  };

  for (const int bit : {PowerPC::CR_LT_BIT, PowerPC::CR_GT_BIT, PowerPC::CR_EQ_BIT})
  {
    for (const bool branch_if_true : {false, true})
    {
      for (const u32 body_cycles : {1u, 2u, 3u})
      {
        const auto fn = reinterpret_cast<DiamondFn>(jit.AlignCode16());
        jit.ABI_PushRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
        // Save ABI args: param1=init_rd, param2=init_val0, param3=cand, param4=init_dc, param5=out
        jit.MOV(32, R(R12), R(ABI_PARAM1));  // Rd
        jit.MOV(32, R(R13), R(ABI_PARAM2));  // val0
        jit.MOV(32, R(RSCRATCH), R(ABI_PARAM3));  // cand
        jit.MOV(32, R(R14), R(ABI_PARAM4));  // downcount
        jit.MOV(64, R(R15), R(ABI_PARAM5));  // out

        jit.MOV(32, R(RSCRATCH2), R(R13));  // RSCRATCH2 = val0
        (jit.TEST)(32, R(R12), R(R12));

        CCFlags cc_exec = CC_A;
        if (bit == PowerPC::CR_EQ_BIT)
          cc_exec = branch_if_true ? CC_NZ : CC_Z;
        else if (bit == PowerPC::CR_GT_BIT)
          cc_exec = branch_if_true ? CC_LE : CC_G;
        else
          cc_exec = branch_if_true ? CC_NS : CC_S;

        jit.CMOVcc(32, R12, R(RSCRATCH), cc_exec);
        jit.CMOVcc(32, RSCRATCH2, R(RSCRATCH), cc_exec);

        jit.SETcc(cc_exec, R(RSCRATCH));
        jit.MOVZX(32, 8, RSCRATCH, R(RSCRATCH));
        if (body_cycles == 2)
          jit.ADD(32, R(RSCRATCH), R(RSCRATCH));
        else if (body_cycles == 3)
          jit.LEA(32, RSCRATCH, MComplex(RSCRATCH, RSCRATCH, SCALE_2, 0));
        jit.SUB(32, R(R14), R(RSCRATCH));

        jit.MOV(32, MDisp(R15, 0), R(R12));
        jit.MOV(32, MDisp(R15, 4), R(RSCRATCH2));
        jit.MOV(32, MDisp(R15, 8), R(R14));
        jit.ABI_PopRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
        jit.RET();

        for (const u32 v : test_vals)
        {
          const s32 sv = static_cast<s32>(v);
          const u64 cr_internal = PowerPC::ConditionRegister::PPCToInternal(
              sv < 0 ? PowerPC::CR_LT : (sv > 0 ? PowerPC::CR_GT : PowerPC::CR_EQ));
          bool cr_bit_set = false;
          if (bit == PowerPC::CR_LT_BIT)
            cr_bit_set = ((cr_internal >> PowerPC::CR_EMU_LT_BIT) & 1u) != 0;
          else if (bit == PowerPC::CR_EQ_BIT)
            cr_bit_set = static_cast<u32>(cr_internal) == 0;
          else if (bit == PowerPC::CR_GT_BIT)
            cr_bit_set = static_cast<s64>(cr_internal) > 0;

          const bool branch_taken = (cr_bit_set == branch_if_true);
          const u32 val0 = v ^ 0x55AA33CCu;
          const u32 cand = v + 0x7FE17AD7u;
          const s32 init_dc = 100;

          DiamondOut out{};
          fn(v, val0, cand, init_dc, &out);

          EXPECT_EQ(out.rd, branch_taken ? v : cand)
              << "bit=" << bit << " bif=" << branch_if_true << " v=" << std::hex << v;
          EXPECT_EQ(out.stored, branch_taken ? val0 : cand)
              << "bit=" << bit << " bif=" << branch_if_true << " v=" << std::hex << v;
          EXPECT_EQ(out.downcount, branch_taken ? init_dc : (init_dc - static_cast<s32>(body_cycles)))
              << "bit=" << bit << " bif=" << branch_if_true << " cyc=" << body_cycles;
        }
      }
    }
  }

  // 2. Verify addis+addi immediate recombination matches SC2's PRNG modulo-wrap constants:
  //    - subis rD, rA, 32768; subi rD, rD, 1  => +0x7FFFFFFF (2^31 - 1)
  //    - addis rD, rA, 32737; addi rD, rD, 31447 => +0x7FE17AD7 (2145483479)
  auto combine_addis_addi = [](s16 hi_simm, s16 lo_simm) -> u32 {
    const u32 hi = static_cast<u32>(static_cast<u16>(hi_simm)) << 16;
    const u32 lo = static_cast<u32>(static_cast<s32>(lo_simm));
    return hi + lo;
  };
  EXPECT_EQ(combine_addis_addi(static_cast<s16>(-32768), static_cast<s16>(-1)), 0x7FFFFFFFu);
  EXPECT_EQ(combine_addis_addi(static_cast<s16>(32737), static_cast<s16>(31447)), 0x7FE17AD7u);
  EXPECT_EQ(combine_addis_addi(static_cast<s16>(1), static_cast<s16>(-1)), 0x0000FFFFu);
}
