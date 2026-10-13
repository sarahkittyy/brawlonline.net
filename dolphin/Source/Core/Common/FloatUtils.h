// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bit>
#include <limits>

#include "Common/CommonTypes.h"

namespace Common
{
template <typename T>
constexpr T SNANConstant()
{
  return std::numeric_limits<T>::signaling_NaN();
}

// The most significant bit of the fraction is an is-quiet bit on all architectures we care about.
static constexpr u64 DOUBLE_QBIT = 0x0008000000000000ULL;
static constexpr u64 DOUBLE_SIGN = 0x8000000000000000ULL;
static constexpr u64 DOUBLE_EXP = 0x7FF0000000000000ULL;
static constexpr u64 DOUBLE_FRAC = 0x000FFFFFFFFFFFFFULL;
static constexpr u64 DOUBLE_ZERO = 0x0000000000000000ULL;
static constexpr int DOUBLE_EXP_WIDTH = 11;
static constexpr int DOUBLE_FRAC_WIDTH = 52;

static constexpr u32 FLOAT_SIGN = 0x80000000;
static constexpr u32 FLOAT_EXP = 0x7F800000;
static constexpr u32 FLOAT_FRAC = 0x007FFFFF;
static constexpr u32 FLOAT_ZERO = 0x00000000;
static constexpr int FLOAT_EXP_WIDTH = 8;
static constexpr int FLOAT_FRAC_WIDTH = 23;

// Sign bit changes that are right under any host FPU mode. On arm64 hosts with FEAT_AFP,
// Common::FPU::SetSIMDMode sets FPCR.AH while the guest is in non-IEEE mode, and then FABS and FNEG
// leave a NaN as it is. The compiler both turns integer sign masking into FABS/FNEG and builds the
// mask 0x7FF...F (for std::copysign, or |x| of two lanes) as FNEG of all ones, a NaN, which then
// keeps its sign: Interpreter::ps_abs cleared no sign at all. Code that runs under the guest's FPU
// mode changes signs with these, whose mask the compiler cannot see through.
inline u64 OpaqueBits(u64 value)
{
#if defined(__GNUC__) || defined(__clang__)
  asm("" : "+r"(value));
#endif
  return value;
}

inline u64 ClearSignBit(u64 bits)
{
  return bits & OpaqueBits(~DOUBLE_SIGN);
}

inline u64 SetSignBit(u64 bits)
{
  return bits | OpaqueBits(DOUBLE_SIGN);
}

inline u64 FlipSignBit(u64 bits)
{
  return bits ^ OpaqueBits(DOUBLE_SIGN);
}

// std::copysign(magnitude, sign), on the integer side.
inline double CopySign(double magnitude, double sign)
{
  return std::bit_cast<double>(ClearSignBit(std::bit_cast<u64>(magnitude)) |
                               (std::bit_cast<u64>(sign) & OpaqueBits(DOUBLE_SIGN)));
}

inline bool IsQNAN(double d)
{
  const u64 i = std::bit_cast<u64>(d);
  return ((i & DOUBLE_EXP) == DOUBLE_EXP) && ((i & DOUBLE_QBIT) == DOUBLE_QBIT);
}

inline bool IsSNAN(double d)
{
  const u64 i = std::bit_cast<u64>(d);
  return ((i & DOUBLE_EXP) == DOUBLE_EXP) && ((i & DOUBLE_FRAC) != DOUBLE_ZERO) &&
         ((i & DOUBLE_QBIT) == DOUBLE_ZERO);
}

inline float FlushToZero(float f)
{
  u32 i = std::bit_cast<u32>(f);
  if ((i & FLOAT_EXP) == 0)
  {
    // Turn into signed zero
    i &= FLOAT_SIGN;
  }
  return std::bit_cast<float>(i);
}

inline double FlushToZero(double d)
{
  u64 i = std::bit_cast<u64>(d);
  if ((i & DOUBLE_EXP) == 0)
  {
    // Turn into signed zero
    i &= DOUBLE_SIGN;
  }
  return std::bit_cast<double>(i);
}

inline double MakeQuiet(double d)
{
  const u64 integral = std::bit_cast<u64>(d) | Common::DOUBLE_QBIT;

  return std::bit_cast<double>(integral);
}

enum PPCFpClass
{
  PPC_FPCLASS_QNAN = 0x11,
  PPC_FPCLASS_NINF = 0x9,
  PPC_FPCLASS_NN = 0x8,
  PPC_FPCLASS_ND = 0x18,
  PPC_FPCLASS_NZ = 0x12,
  PPC_FPCLASS_PZ = 0x2,
  PPC_FPCLASS_PD = 0x14,
  PPC_FPCLASS_PN = 0x4,
  PPC_FPCLASS_PINF = 0x5,
};

// Uses PowerPC conventions for the return value, so it can be easily
// used directly in CPU emulation.
u32 ClassifyDouble(double dvalue);
u32 ClassifyFloat(float fvalue);

struct BaseAndDec
{
  int m_base;
  int m_dec;
};
extern const std::array<BaseAndDec, 32> frsqrte_expected;
extern const std::array<BaseAndDec, 32> fres_expected;

// PowerPC approximation algorithms
double ApproximateReciprocalSquareRoot(double val);
double ApproximateReciprocal(double val);

}  // namespace Common
