// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Jit64::fmaddXX on a CPU without FMA instructions, with SESSION_USE_FMA set (whole-machine
// sessions set it on every machine): when both factors are known single-precision values
// (fprIsSingle), it multiplies and adds instead of calling std::fma, as their product is exact in
// double precision (bug report #313). This checks that claim on the host's instructions: for single
// factors (any float bits, denormals, infinities and NaNs included) and any addend, MULPD/MULSD and
// then ADDPD/SUBPD give the bits VFMADD132/VFMSUB132 give, in every rounding mode, with
// flush-to-zero and denormals-are-zero on or off; and the bits std::fma gives in the default mode.
// A NaN result only has to be a NaN: fmaddXX's HandleNaNs picks the NaN the game sees.

#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <random>
#include <vector>

#include <fmt/format.h>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/x64ABI.h"
#include "Common/x64Emitter.h"

// gtest after the emitter: its TEST macro would take the emitter's TEST instruction.

namespace
{
// in: a[2], c[2], b[2], mxcsr; out: multiply-and-add[2], fused[2], scratch.
using Wrapped = void (*)(const u64* in, u64* out);

class Code : public Gen::X64CodeBlock
{
public:
  Code() { AllocCodeSpace(4096); }
  ~Code() { FreeCodeSpace(); }

  Wrapped Emit(bool packed, bool subtract, bool fused_too)
  {
    using namespace Gen;
    const auto wrapped = reinterpret_cast<Wrapped>(AlignCode16());
    STMXCSR(MDisp(ABI_PARAM2, 32));
    LDMXCSR(MDisp(ABI_PARAM1, 48));
    MOVUPD(XMM0, MDisp(ABI_PARAM1, 0));
    MOVUPD(XMM1, MDisp(ABI_PARAM1, 16));
    MOVUPD(XMM2, MDisp(ABI_PARAM1, 32));

    // fmaddXX without host FMA: result_xmm = Rc, times Ra, plus or minus Rb.
    MOVAPD(XMM3, R(XMM1));
    if (packed)
    {
      MULPD(XMM3, R(XMM0));
      if (subtract)
        SUBPD(XMM3, R(XMM2));
      else
        ADDPD(XMM3, R(XMM2));
    }
    else
    {
      MULSD(XMM3, R(XMM0));
      if (subtract)
        SUBSD(XMM3, R(XMM2));
      else
        ADDSD(XMM3, R(XMM2));
    }
    MOVUPD(MDisp(ABI_PARAM2, 0), XMM3);

    // fmaddXX with host FMA.
    if (fused_too)
    {
      MOVAPD(XMM3, R(XMM1));
      if (packed)
      {
        if (subtract)
          VFMSUB132PD(XMM3, XMM2, R(XMM0));
        else
          VFMADD132PD(XMM3, XMM2, R(XMM0));
      }
      else
      {
        if (subtract)
          VFMSUB132SD(XMM3, XMM2, R(XMM0));
        else
          VFMADD132SD(XMM3, XMM2, R(XMM0));
      }
      MOVUPD(MDisp(ABI_PARAM2, 16), XMM3);
    }

    LDMXCSR(MDisp(ABI_PARAM2, 32));
    RET();
    return wrapped;
  }
};

double D(u64 bits)
{
  return std::bit_cast<double>(bits);
}
u64 B(double d)
{
  return std::bit_cast<u64>(d);
}

class Generator
{
public:
  explicit Generator(u64 seed) : rng(seed) {}

  // A factor as fprIsSingle promises it: a single's value, as the JIT holds it (in double).
  double Factor()
  {
    switch (rng() % 4)
    {
    case 0:  // any float bits: denormals, infinities, NaNs, zeros
      return static_cast<double>(std::bit_cast<float>(static_cast<u32>(rng())));
    case 1:  // a modest single
    {
      const u32 mant = static_cast<u32>(rng()) & 0x7FFFFF & (~0u << (rng() % 24));
      const u32 exp = 127 + static_cast<u32>(rng() % 40) - 20;
      const u32 sign = rng() % 2 ? 0x80000000u : 0;
      return static_cast<double>(std::bit_cast<float>(sign | (exp << 23) | mant));
    }
    case 2:  // the edges of a single's range
    {
      static constexpr std::array<float, 10> edges{
          FLT_MAX, FLT_MIN, FLT_TRUE_MIN, 0.0f, 1.0f, 0x1.fffffep0f, 0x1p-140f, 0x1p100f, 0x1p-126f,
          0x1.000002p-126f};
      const float f = edges[rng() % edges.size()];
      return rng() % 2 ? -static_cast<double>(f) : static_cast<double>(f);
    }
    default:  // a single denormal
      return static_cast<double>(std::bit_cast<float>(static_cast<u32>(rng() & 0x807FFFFF)));
    }
  }

  // Any addend: double bits, singles, and values that cancel the product (exactly or nearly).
  double Addend(double p)
  {
    switch (rng() % 5)
    {
    case 0:
      return D(rng());
    case 1:
      return static_cast<double>(std::bit_cast<float>(static_cast<u32>(rng())));
    case 2:  // a double denormal
      return D(rng() & 0x800FFFFFFFFFFFFFULL);
    default:  // near -p (or p for a subtract): the sum's low bits and its sign of zero
    {
      double b = rng() % 2 ? -p : p;
      const int nudge = static_cast<int>(rng() % 9) - 4;
      for (int i = 0; i < std::abs(nudge); ++i)
        b = std::nextafter(b, nudge > 0 ? INFINITY : -INFINITY);
      if (rng() % 4 == 0)
        b = static_cast<float>(b);
      return b;
    }
    }
  }

private:
  std::mt19937_64 rng;
};

bool Same(u64 x, u64 y)
{
  return x == y || (std::isnan(D(x)) && std::isnan(D(y)));
}
}  // namespace

#include <gtest/gtest.h>

TEST(Jit64, MaddExactProductSameAsFma)
{
  const bool host_fma = cpu_info.bFMA;
  Code code;
  // MXCSR: exceptions masked, then RC (bits 13-14), FTZ (bit 15), DAZ (bit 6).
  std::vector<u32> modes;
  for (u32 rc = 0; rc < 4; ++rc)
  {
    for (u32 ftz = 0; ftz < 2; ++ftz)
    {
      for (u32 daz = 0; daz < 2; ++daz)
        modes.push_back(0x1F80 | (rc << 13) | (ftz << 15) | (daz << 6));
    }
  }

  constexpr int cases = 100000;
  for (int vi = 0; vi < 4; ++vi)
  {
    const bool packed = (vi & 1) != 0;
    const bool subtract = (vi & 2) != 0;
    const Wrapped f = code.Emit(packed, subtract, host_fma);
    Generator gen(313 + vi);
    int fused_mismatches = 0, std_mismatches = 0, std_checked = 0;
    for (int i = 0; i < cases; ++i)
    {
      const double a0 = gen.Factor(), a1 = gen.Factor(), c0 = gen.Factor(), c1 = gen.Factor();
      const double b0 = gen.Addend(a0 * c0), b1 = gen.Addend(a1 * c1);
      const u32 mode = modes[i % modes.size()];
      const std::array<u64, 7> in{B(a0), B(a1), B(c0), B(c1), B(b0), B(b1), mode};
      std::array<u64, 5> out{};
      f(in.data(), out.data());

      for (int lane = 0; lane < (packed ? 2 : 1); ++lane)
      {
        const u64 mul_add = out[lane];
        if (host_fma && !Same(mul_add, out[2 + lane]) && ++fused_mismatches <= 10)
        {
          ADD_FAILURE() << fmt::format(
              "packed={} sub={} mxcsr {:04x}: a {:016x} c {:016x} b {:016x}: multiply and add "
              "{:016x}, FMA {:016x}",
              packed, subtract, mode, in[lane], in[2 + lane], in[4 + lane], mul_add, out[2 + lane]);
        }
        if (mode == 0x1F80)
        {
          const double a = D(in[lane]), c = D(in[2 + lane]), b = D(in[4 + lane]);
          const u64 ref = B(std::fma(a, c, subtract ? -b : b));
          ++std_checked;
          if (!Same(mul_add, ref) && ++std_mismatches <= 10)
          {
            ADD_FAILURE() << fmt::format(
                "packed={} sub={}: a {:016x} c {:016x} b {:016x}: multiply and add {:016x}, "
                "std::fma {:016x}",
                packed, subtract, in[lane], in[2 + lane], in[4 + lane], mul_add, ref);
          }
        }
      }
    }
    EXPECT_EQ(fused_mismatches, 0);
    EXPECT_EQ(std_mismatches, 0);
    EXPECT_GT(std_checked, 1000);
    fmt::print("packed={} sub={}: {} cases over {} MXCSR modes{}, {} against std::fma\n", packed,
               subtract, cases, modes.size(), host_fma ? " against host FMA" : "", std_checked);
  }
}

// The test's own check: a factor that is not a single (a full double) makes them differ.
TEST(Jit64, MaddExactProductNeedsSingles)
{
  Code code;
  const Wrapped f = code.Emit(false, false, false);
  // (1 + 2^-30)^2 - 1 = 2^-29 + 2^-60: the multiply alone rounds the 2^-60 away.
  const double a = 1.0 + 0x1p-30, c = 1.0 + 0x1p-30, b = -1.0;
  const std::array<u64, 7> in{B(a), 0, B(c), 0, B(b), 0, 0x1F80};
  std::array<u64, 5> out{};
  f(in.data(), out.data());
  EXPECT_NE(out[0], B(std::fma(a, c, b)));
}
