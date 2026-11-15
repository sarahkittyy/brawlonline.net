// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// GitHub issue #40: Jit64's accurate single-precision madd tests for an even tie before it computes
// the error terms and calls fmadds_eft / ps_madd_eft, and computes them out of line only for a
// tie. This test emits the tail of Jit64::fmaddXX's error-free transformation both ways (as it
// was, and with the tie test first) around the real routines, and checks that the results are
// the same bits for every operand: random, constructed ties (with and without an error) and edge
// values, scalar and paired, madd and msub, with host FMA, without it (MULPD + SUBPD error terms),
// without AVX and without SSE4.1. The instruction sequences are copied from fmaddXX; keep them in
// step.

#include <array>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>
#include <vector>

#include <fmt/format.h>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/ScopeGuard.h"
#include "Common/x64ABI.h"
#include "Core/Core.h"
#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64Common/Jit64AsmCommon.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"
#include "Core/System.h"

// gtest after the emitter code below: its TEST macro would take the emitter's TEST instruction.

namespace
{
// in: Ra[2], Rb[2], Rc_rounded[2]; out: XMM1[2] (the result), Ra[2], Rb[2] as left.
using Wrapped = void (*)(const u64* in, u64* out);

alignas(16) const u64 madd_tie_bits[2] = {0x8000000000000000ULL, 0x8000000000000000ULL};

struct Variant
{
  bool packed;
  bool subtract;
  bool use_fma;
  bool rc_in_xmm3;  // Rc_rounded_duplicated is XMM3 (want_rc_rounded) or a bound register
};

class TestRoutines : public CommonAsmRoutines
{
public:
  explicit TestRoutines(Core::System& system) : CommonAsmRoutines(jit), jit(system)
  {
    AllocCodeSpace(1 << 20);
    m_const_pool.Init(AllocChildCodeSpace(4096), 4096);
    AddChildCodeSpace(&m_far_code, 1 << 18);
    m_far_code.Init();

    fmadds_eft = AlignCode4();
    GenerateFmaddsEft();
    ps_madd_eft = AlignCode4();
    GeneratePsMaddEft();
  }

  // Emits the s := Ra * Rc_rounded +/- Rb of fmaddXX, then its error-free transformation tail.
  // loop: in[0] is a count, then count operand sets of 6 u64 (in + 2 onwards); out gets each
  // XMM1 (2 u64), for timing.
  Wrapped Emit(const Variant& v, bool fast_path, bool loop = false)
  {
    using namespace Gen;
    const X64Reg Ra_reg = XMM4;
    const X64Reg Rb_reg = XMM5;
    const X64Reg Rc_rounded_duplicated = v.rc_in_xmm3 ? XMM3 : XMM6;
    const OpArg Ra = R(Ra_reg);
    const OpArg Rb = R(Rb_reg);
    const bool packed = v.packed;
    const bool subtract = v.subtract;
    const bool use_fma = v.use_fma;

    const auto wrapped = reinterpret_cast<Wrapped>(AlignCode16());
    ABI_PushRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
    // The pointers in callee-saved registers: ABI_PARAM2 is RDX = RSCRATCH2 on Windows.
    MOV(64, R(RBX), R(ABI_PARAM2));
    MOV(64, R(R12), R(ABI_PARAM1));
    const u8* loop_top = nullptr;
    if (loop)
    {
      MOV(64, R(R13), MatR(R12));
      ADD(64, R(R12), Imm8(16));
      loop_top = GetCodePtr();
    }
    MOVUPD(Ra_reg, MDisp(R12, 0));
    MOVUPD(Rb_reg, MDisp(R12, 16));
    MOVUPD(Rc_rounded_duplicated, MDisp(R12, 32));
    // Junk in the scratch registers, as the JIT leaves them.
    PCMPEQD(XMM0, R(XMM0));
    PCMPEQD(XMM2, R(XMM2));
    if (!v.rc_in_xmm3)
      PCMPEQD(XMM3, R(XMM3));

    // s, as fmaddXX computes it into result_xmm (XMM1 here).
    MOVAPD(XMM1, R(Rc_rounded_duplicated));
    if (use_fma)
    {
      if (subtract)
      {
        if (packed)
          VFMSUB132PD(XMM1, Rb_reg, Ra);
        else
          VFMSUB132SD(XMM1, Rb_reg, Ra);
      }
      else
      {
        if (packed)
          VFMADD132PD(XMM1, Rb_reg, Ra);
        else
          VFMADD132SD(XMM1, Rb_reg, Ra);
      }
    }
    else
    {
      if (packed)
      {
        MULPD(XMM1, Ra);
        if (subtract)
          SUBPD(XMM1, Rb);
        else
          ADDPD(XMM1, Rb);
      }
      else
      {
        MULSD(XMM1, Ra);
        if (subtract)
          SUBSD(XMM1, Rb);
        else
          ADDSD(XMM1, Rb);
      }
    }

    // ---- From fmaddXX (software_fma is false) ----
    std::optional<FixupBranch> tie;
    if (fast_path && (!packed || cpu_info.bSSE4_1) && m_far_code.Enabled() &&
        !m_far_code.IsInSpace(GetCodePtr()))
    {
      if (packed)
      {
        avx_op(&XEmitter::VPSLLQ, &XEmitter::PSLLQ, XMM0, XMM1, 35);
        PCMPEQQ(XMM0, MConst(madd_tie_bits));
        PTEST(XMM0, R(XMM0));
        tie = J_CC(CC_NZ, Jump::Near);
      }
      else
      {
        MOVQ_xmm(R(RSCRATCH), XMM1);
        LEA(32, RSCRATCH2, MScaled(RSCRATCH, SCALE_8, static_cast<s32>(0x80000000)));
        TEST(32, R(RSCRATCH2), R(RSCRATCH2));
        tie = J_CC(CC_Z, Jump::Near);
      }
      SwitchToFarCode();
      SetJumpTarget(*tie);
    }

    if (packed)
    {
      if (subtract)
        avx_op(&XEmitter::VADDPD, &XEmitter::ADDPD, XMM0, R(XMM1), Rb);
      else
        avx_op(&XEmitter::VSUBPD, &XEmitter::SUBPD, XMM0, R(XMM1), Rb);
      avx_op(&XEmitter::VSUBPD, &XEmitter::SUBPD, XMM2, R(XMM1), R(XMM0));
      if (use_fma)
      {
        VFMSUB231PD(XMM0, Rc_rounded_duplicated, Ra);
      }
      else
      {
        avx_op(&XEmitter::VMULPD, &XEmitter::MULPD, XMM3, R(Rc_rounded_duplicated), Ra);
        avx_op(&XEmitter::VSUBPD, &XEmitter::SUBPD, XMM0, R(XMM3), R(XMM0), true, false, XMM3);
      }
      if (subtract)
        avx_op(&XEmitter::VADDPD, &XEmitter::ADDPD, XMM2, R(XMM2), Rb);
      else
        avx_op(&XEmitter::VSUBPD, &XEmitter::SUBPD, XMM2, R(XMM2), Rb);
      CALL(ps_madd_eft);
    }
    else
    {
      if (subtract)
        avx_op(&XEmitter::VADDSD, &XEmitter::ADDSD, XMM0, R(XMM1), Rb, false);
      else
        avx_op(&XEmitter::VSUBSD, &XEmitter::SUBSD, XMM0, R(XMM1), Rb, false);
      avx_op(&XEmitter::VSUBSD, &XEmitter::SUBSD, XMM2, R(XMM1), R(XMM0), false);
      if (use_fma)
      {
        VFMSUB231SD(XMM0, Rc_rounded_duplicated, Ra);
      }
      else
      {
        avx_op(&XEmitter::VMULSD, &XEmitter::MULSD, XMM3, R(Rc_rounded_duplicated), Ra, false);
        avx_op(&XEmitter::VSUBSD, &XEmitter::SUBSD, XMM0, R(XMM3), R(XMM0), false, false, XMM3);
      }
      if (subtract)
        ADDSD(XMM2, Rb);
      else
        SUBSD(XMM2, Rb);
      CALL(fmadds_eft);
    }

    if (tie)
    {
      FixupBranch join = J(Jump::Near);
      SwitchToNearCode();
      SetJumpTarget(join);
    }
    // ---- End of fmaddXX's part ----

    MOVUPD(MDisp(RBX, 0), XMM1);
    if (loop)
    {
      ADD(64, R(R12), Imm8(48));
      ADD(64, R(RBX), Imm8(16));
      SUB(64, R(R13), Imm8(1));
      J_CC(CC_NZ, loop_top);
    }
    else
    {
      MOVUPD(MDisp(RBX, 16), Ra_reg);
      MOVUPD(MDisp(RBX, 32), Rb_reg);
    }
    ABI_PopRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
    RET();
    return wrapped;
  }

  ~TestRoutines() { FreeCodeSpace(); }  // before the far code (a child) is destroyed

  const u8* fmadds_eft;
  const u8* ps_madd_eft;
  Jit64 jit;
};

double D(u64 bits)
{
  return std::bit_cast<double>(bits);
}
u64 B(double d)
{
  return std::bit_cast<u64>(d);
}
bool IsTie(u64 s)
{
  return (s & 0x1fffffff) == 0x10000000;
}

struct Operands
{
  u64 a, b, c;  // c already rounded to 25 bits (Force25Bit), as fmaddXX's Rc_rounded
};

class Generator
{
public:
  explicit Generator(u64 seed) : rng(seed) {}

  Operands Next(bool subtract)
  {
    Operands o{};
    switch (rng() % 9)
    {
    case 8:  // a tie at an extreme exponent: near FLT_MAX, FLT_MIN, single denormals, huge doubles
    {
      static constexpr std::array<int, 12> exps{128, 127, 126, -125, -126, -127, -140, -149,
                                                -150, 1000, -1000, 1023};
      const int k = exps[rng() % exps.size()];
      const double a = 1.0 + static_cast<double>(rng() & 0xFFFFFF) * 0x1p-24;
      const double c = Force25Bit(std::ldexp(1.0 + static_cast<double>(rng() & 0xFFFFFF) * 0x1p-24, k));
      const double p = a * c;
      int e;
      std::frexp(p, &e);
      const int shift = static_cast<int>(rng() % 3) - 1;
      u64 t = (static_cast<u64>(std::clamp(e + shift + 1022, 1, 2046)) << 52) |
              (rng() & 0x000FFFFFE0000000ULL) | 0x10000000;
      if (rng() % 2)
        t |= 0x8000000000000000ULL;
      double b = subtract ? p - D(t) : D(t) - p;
      const int nudge = static_cast<int>(rng() % 5) - 2;
      for (int i = 0; i < std::abs(nudge); ++i)
        b = std::nextafter(b, nudge > 0 ? INFINITY : -INFINITY);
      o.a = B(rng() % 2 ? a : -a);
      o.c = B(c);
      o.b = B(b);
      break;
    }
    case 0:  // random singles
      o.a = B(RandomFloat());
      o.c = B(RandomFloat());
      o.b = B(RandomFloat());
      break;
    case 1:  // random doubles of any bits
      o.a = rng();
      o.c = rng();
      o.b = rng();
      break;
    case 2:  // edge values, mixed with singles
      o.a = rng() % 2 ? Edge() : B(RandomFloat());
      o.c = rng() % 2 ? Edge() : B(RandomFloat());
      o.b = rng() % 2 ? Edge() : B(RandomFloat());
      break;
    default:  // a tie: s lands on a double whose low 29 bits are 0x10000000
    {
      const double a = RandomFloatNear();
      const double c = Force25Bit(RandomFloatNear());
      const double p = a * c;  // exact for singles; close otherwise
      int e;
      std::frexp(p, &e);
      // A target T with the tie pattern within a few binades of p, either sign.
      const int shift = static_cast<int>(rng() % 9) - 4;
      u64 t = (static_cast<u64>(std::clamp(e + shift + 1022, 1, 2046)) << 52) |
              (rng() & 0x000FFFFFE0000000ULL) | 0x10000000;
      if (rng() % 2)
        t |= 0x8000000000000000ULL;
      double b = subtract ? p - D(t) : D(t) - p;
      // Nudge b by a few ulps so the exact sum has bits below s (an error to correct).
      const int nudge = static_cast<int>(rng() % 7) - 3;
      for (int i = 0; i < std::abs(nudge); ++i)
        b = std::nextafter(b, nudge > 0 ? INFINITY : -INFINITY);
      // Or make b a single, as most of Brawl's are.
      if (rng() % 4 == 0)
        b = static_cast<float>(b);
      o.a = B(a);
      o.c = B(c);
      o.b = B(b);
      break;
    }
    }
    o.c = B(Force25Bit(D(o.c)));
    return o;
  }

private:
  float RandomFloat()
  {
    const u32 bits = static_cast<u32>(rng());
    return std::bit_cast<float>(bits);
  }
  double RandomFloatNear()
  {
    // A single with a modest exponent and a random mantissa (often short).
    const u32 mant = static_cast<u32>(rng()) & 0x7FFFFF & (~0u << (rng() % 24));
    const u32 exp = 127 + static_cast<u32>(rng() % 40) - 20;
    const u32 sign = rng() % 2 ? 0x80000000u : 0;
    return std::bit_cast<float>(sign | (exp << 23) | mant);
  }
  u64 Edge()
  {
    static const std::vector<u64> edges = [] {
      std::vector<u64> v = {
          0,
          1,                      // smallest double denormal
          0x000FFFFFFFFFFFFFULL,  // largest double denormal
          0x0010000000000000ULL,  // DBL_MIN
          0x7FEFFFFFFFFFFFFFULL,  // DBL_MAX
          0x7FF0000000000000ULL,  // Inf
          0x7FF8000000000000ULL,  // QNaN
          0x7FF0000000000001ULL,  // SNaN
          0x7FF4000010000000ULL,  // SNaN with the tie bits
          B(FLT_MIN),
          B(FLT_MAX),
          B(FLT_TRUE_MIN),
          B(static_cast<double>(FLT_MAX) * 2),  // past FLT_MAX
          B(std::nextafter(static_cast<double>(FLT_MAX), 0.0)),
          B(std::nextafter(static_cast<double>(FLT_MIN), 0.0)),  // under FLT_MIN
          B(1.0),
          0x3FF0000010000000ULL,  // 1.0 + a tie
          0x3FF000000FFFFFFFULL,
          0x3FF0000010000001ULL,
          0x3FF0000030000000ULL,  // odd tie
          B(0x1p127),
          B(0x1p-126),
          B(0x1p-149),
          B(0x1p-150),
          B(0x1p1000),
          B(0x1p-1000),
          B(3.0),
          B(0x1.fffffep0),
      };
      const size_t n = v.size();
      for (size_t i = 0; i < n; ++i)
        v.push_back(v[i] | 0x8000000000000000ULL);
      return v;
    }();
    return edges[rng() % edges.size()];
  }

  std::mt19937_64 rng;
};

}  // namespace

template <bool sub>
double InterpreterMadd(PowerPC::PowerPCState& ppc_state, double a, double c, double b)
{
  return NI_madd_msub<sub, true>(ppc_state, a, c, b).value;
}

#include <gtest/gtest.h>

namespace
{
struct HostConfig
{
  const char* name;
  bool fma;
  bool avx;
  bool sse4_1;
};
}  // namespace

TEST(Jit64, MaddTieFastPathSameBits)
{
  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_thread_guard([] { Core::UndeclareAsCPUThread(); });

  const CPUInfo saved = cpu_info;
  Common::ScopeGuard restore([&] { cpu_info = saved; });

  std::vector<HostConfig> configs;
  if (saved.bFMA && saved.bAVX)
    configs.push_back({"FMA host", true, true, true});
  if (saved.bAVX)
    configs.push_back({"no FMA (MULPD error terms), AVX", false, true, saved.bSSE4_1});
  configs.push_back({"no FMA, no AVX", false, false, saved.bSSE4_1});
  configs.push_back({"no FMA, no AVX, no SSE4.1", false, false, false});

  auto& ppc_state = Core::System::GetInstance().GetPPCState();
  // MADD_TIE_CASES=N for a longer run.
  const char* const cases_env = std::getenv("MADD_TIE_CASES");
  const int cases = cases_env ? std::atoi(cases_env) : 200000;

  for (const HostConfig& config : configs)
  {
    cpu_info.bFMA = config.fma;
    cpu_info.bAVX = config.avx;
    cpu_info.bSSE4_1 = config.sse4_1;
    cpu_info.bSSE4_2 = config.sse4_1 && saved.bSSE4_2;

    TestRoutines routines(Core::System::GetInstance());

    for (int vi = 0; vi < 16; ++vi)
    {
      const Variant v{(vi & 1) != 0, (vi & 2) != 0, config.fma, (vi & 4) != 0};
      if ((vi & 8) != 0 && !v.packed)
        continue;  // packed only: one lane tie and the other not, below
      const Wrapped before = routines.Emit(v, false);
      const Wrapped after = routines.Emit(v, true);

      Generator gen(40 + vi * 7 + (config.fma ? 1000 : 0) + (config.avx ? 100 : 0) +
                    (config.sse4_1 ? 10 : 0));
      int ties = 0, corrected = 0, mismatches = 0, ref_checked = 0, ref_mismatches = 0;
      for (int i = 0; i < cases; ++i)
      {
        const Operands l0 = gen.Next(v.subtract);
        const Operands l1 = (vi & 8) != 0 ? Operands{l0.a, B(D(l0.b) * 3.0), l0.c} :
                                            gen.Next(v.subtract);
        const std::array<u64, 6> in{l0.a, l1.a, l0.b, l1.b, l0.c, l1.c};
        std::array<u64, 6> out_before{};
        std::array<u64, 6> out_after{};
        before(in.data(), out_before.data());
        after(in.data(), out_after.data());

        if (out_before != out_after)
        {
          if (++mismatches <= 10)
          {
            ADD_FAILURE() << fmt::format(
                "{} packed={} sub={} rc_xmm3={}: a {:016x} {:016x} b {:016x} {:016x} c {:016x} "
                "{:016x} -> before {:016x} {:016x}, after {:016x} {:016x}",
                config.name, v.packed, v.subtract, v.rc_in_xmm3, in[0], in[1], in[2], in[3], in[4],
                in[5], out_before[0], out_before[1], out_after[0], out_after[1]);
          }
        }
        EXPECT_EQ(out_before[2], in[0]);
        EXPECT_EQ(out_before[4], in[2]);

        for (int lane = 0; lane < (v.packed ? 2 : 1); ++lane)
        {
          const Operands& o = lane == 0 ? l0 : l1;
          // s as computed before the correction: with FMA, exactly std::fma.
          const double s = v.use_fma ? std::fma(D(o.a), D(o.c), v.subtract ? -D(o.b) : D(o.b)) :
                                       (v.subtract ? D(o.a) * D(o.c) - D(o.b) :
                                                     D(o.a) * D(o.c) + D(o.b));
          if (IsTie(B(s)))
            ++ties;
          if (out_before[lane] != B(s))
            ++corrected;

          // The interpreter's result, where it is defined the same way (finite, no NaN handling).
          if (v.use_fma && std::isfinite(D(o.a)) && std::isfinite(D(o.b)) &&
              std::isfinite(D(o.c)) && std::isfinite(s))
          {
            const double r =
                v.subtract ?
                    InterpreterMadd<true>(ppc_state, D(o.a), D(o.c), D(o.b)) :
                    InterpreterMadd<false>(ppc_state, D(o.a), D(o.c), D(o.b));
            ++ref_checked;
            if (B(r) != out_after[lane] && ++ref_mismatches <= 5)
            {
              ADD_FAILURE() << fmt::format(
                  "interpreter differs: {} packed={} sub={} a {:016x} b {:016x} c {:016x}: jit "
                  "{:016x}, interpreter {:016x}",
                  config.name, v.packed, v.subtract, o.a, o.b, o.c, out_after[lane], B(r));
            }
          }
        }
      }
      EXPECT_EQ(mismatches, 0);
      EXPECT_EQ(ref_mismatches, 0);
      // The constructed operands must exercise the out-of-line path, and the rounding in it.
      EXPECT_GT(ties, cases / 20);
      EXPECT_GT(corrected, 100);
      fmt::print("{:32} packed={} sub={} rc_xmm3={} mixed={}: {} cases, {} ties, {} corrected, "
                 "{} interpreter checks, {} mismatches\n",
                 config.name, v.packed, v.subtract, v.rc_in_xmm3, (vi & 8) != 0, cases, ties,
                 corrected, ref_checked, mismatches);
    }
  }
}

// MADD_TIE_BENCH=1: times the madd with the error-free transformation as it was and with the tie
// test first, in a generated loop over operand sets of which a given share are ties, old or new
// picked at random per batch. Not a correctness test; skipped unless asked for.
TEST(Jit64, MaddTieFastPathBench)
{
  if (!std::getenv("MADD_TIE_BENCH"))
    GTEST_SKIP() << "MADD_TIE_BENCH not set";

  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_thread_guard([] { Core::UndeclareAsCPUThread(); });
  const CPUInfo saved = cpu_info;
  Common::ScopeGuard restore([&] { cpu_info = saved; });

  std::vector<HostConfig> configs;
  if (saved.bFMA && saved.bAVX)
    configs.push_back({"FMA host", true, true, true});
  configs.push_back({"no FMA, AVX", false, true, saved.bSSE4_1});
  configs.push_back({"no FMA, no AVX", false, false, saved.bSSE4_1});
  configs.push_back({"no FMA, no AVX, no SSE4.1", false, false, false});

  constexpr int pool = 4096;
  const int batches = 400;
  for (const HostConfig& config : configs)
  {
    cpu_info.bFMA = config.fma;
    cpu_info.bAVX = config.avx;
    cpu_info.bSSE4_1 = config.sse4_1;
    cpu_info.bSSE4_2 = config.sse4_1 && saved.bSSE4_2;
    TestRoutines routines(Core::System::GetInstance());
    for (const bool packed : {false, true})
    {
      const Variant v{packed, false, config.fma, false};
      const Wrapped before = routines.Emit(v, false, true);
      const Wrapped after = routines.Emit(v, true, true);
      for (const double tie_share : {0.0, 0.025, 0.10, 0.5})
      {
        // Operand sets whose s is a tie (per lane) in the given share, in random order.
        Generator gen(7);
        std::mt19937_64 rng(11);
        std::vector<Operands> ties, others;
        while (ties.size() < pool || others.size() < pool)
        {
          const Operands o = gen.Next(false);
          if (!std::isfinite(D(o.a)) || !std::isfinite(D(o.b)) || !std::isfinite(D(o.c)))
            continue;
          const double s = config.fma ? std::fma(D(o.a), D(o.c), D(o.b)) : D(o.a) * D(o.c) + D(o.b);
          if (!std::isfinite(s))
            continue;
          (IsTie(B(s)) ? ties : others).push_back(o);
        }
        std::vector<u64> in(2 + pool * 6);
        in[0] = pool;
        for (int i = 0; i < pool; ++i)
        {
          for (int lane = 0; lane < 2; ++lane)
          {
            const bool tie = std::uniform_real_distribution<double>(0, 1)(rng) < tie_share;
            const Operands& o = (tie ? ties : others)[rng() % pool];
            in[2 + i * 6 + lane] = o.a;
            in[2 + i * 6 + 2 + lane] = o.b;
            in[2 + i * 6 + 4 + lane] = o.c;
          }
        }
        std::vector<u64> out_b(pool * 2), out_a(pool * 2);
        before(in.data(), out_b.data());
        after(in.data(), out_a.data());
        EXPECT_EQ(out_b, out_a);

        std::vector<double> t_before, t_after;
        std::mt19937_64 pick(40);
        for (int b = 0; b < batches; ++b)
        {
          const bool new_arm = pick() % 2;
          const auto t0 = std::chrono::steady_clock::now();
          for (int k = 0; k < 8; ++k)
            (new_arm ? after : before)(in.data(), (new_arm ? out_a : out_b).data());
          const auto t1 = std::chrono::steady_clock::now();
          (new_arm ? t_after : t_before)
              .push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / (8.0 * pool));
        }
        const auto stats = [](std::vector<double> t) {
          std::sort(t.begin(), t.end());
          return std::array<double, 3>{t[t.size() / 4], t[t.size() / 2], t[t.size() * 3 / 4]};
        };
        const auto sb = stats(t_before), sa = stats(t_after);
        fmt::print("BENCH {:28} {:6} ties {:5.1f}%: before {:.3f} ns [{:.3f}-{:.3f}] x{}, after {:.3f} ns "
                   "[{:.3f}-{:.3f}] x{}, after/before {:.3f}\n",
                   config.name, packed ? "paired" : "scalar", tie_share * 100, sb[1], sb[0], sb[2],
                   t_before.size(), sa[1], sa[0], sa[2], t_after.size(), sa[1] / sb[1]);
      }
    }
  }
}
