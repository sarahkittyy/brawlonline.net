// Copyright 2026 Brawl Online
// SPDX-License-Identifier: GPL-2.0-or-later

// Brawl's stage reverb in C++.
//
// Every AX frame (96 samples, 3 ms) Brawl's AUX callback runs the Revolution SDK's high-quality
// reverb ("HI", with bus in/out) on the CPU: main.dol 0x80202A58, once per AUX bus, about 11 times
// per video frame. Per sample and channel (left, right, surround) it runs a pre-delay, three
// early-reflection taps, three comb filters and two allpass filters, a low-pass and one more
// allpass, then mixes the channels (crosstalk) and converts back to int. It is the costliest guest
// function of a presented frame. ReverbHiHook does the same arithmetic in the same order:
//
// - Dolphin's JIT computes fmuls/fadds/fsubs in double and rounds to single. For single inputs
//   that is the single-precision operation itself (double has 53 bits >= 2 * 24 + 2, so the double
//   rounding is innocuous, and a product of two singles is exact in double): plain float
//   arithmetic gives the same bits (the build has -ffp-contract=off).
// - fctiwz truncates toward zero and saturates; NaN gives 0x80000000.
//
// so it writes the same bytes as the guest code. Writes mark their granules dirty as Dolphin's
// own writes into guest memory do. It runs only when the function's code is the one it reproduces,
// every range it touches is in MEM1/MEM2, no two of them overlap, and every position is inside its
// line; otherwise the guest's own code runs. Whole-machine sessions (Other Games rooms) don't get
// the hooks at all (HLE.cpp): there every machine must run exactly what builds without them run.
//
// PPR_REVERB_HLE: unset/1 this, 0 the guest code, check the guest code, compared on every call with
// this run on copies of the memory (WARN log lines for differences, a summary every 2,000 calls).

#include "Core/HLE/HLE_BrawlReverb.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/MachineSession.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/System.h"

namespace HLE_BrawlReverb
{
namespace
{
constexpr u32 FUNCTION = 0x80202A58;
constexpr u32 FUNCTION_RETURN = 0x80202F6C;  // its blr
constexpr u32 FRAME_SIZE = 0x70;             // its stwu r1, -0x70(r1)

// The reverb struct (the function's r4).
constexpr u32 EARLY_LINE = 0x00;   // float* [3]
constexpr u32 EARLY_POS = 0x0C;    // u32 [3] (shared by the channels)
constexpr u32 EARLY_LEN = 0x18;    // u32
constexpr u32 EARLY_COEF = 0x20;   // float [3]
constexpr u32 PRE_LINE = 0x2C;     // float* [3]
constexpr u32 PRE_POS = 0x38;      // u32
constexpr u32 PRE_LEN = 0x3C;      // u32, 0: no pre-delay
constexpr u32 COMB_LINE = 0x44;    // float* [3][3]
constexpr u32 COMB_POS = 0x68;     // u32 [3]
constexpr u32 COMB_LEN = 0x74;     // u32 [3]
constexpr u32 COMB_COEF = 0x8C;    // float [3]
constexpr u32 AP_LINE = 0x98;      // float* [3][2]
constexpr u32 AP_POS = 0xB0;       // u32 [2]
constexpr u32 AP_LEN = 0xB8;       // u32 [2]
constexpr u32 LAST_LINE = 0xC8;    // float* [3]
constexpr u32 LAST_POS = 0xD4;     // u32 [3]
constexpr u32 LAST_LEN = 0xE0;     // u32 [3]
constexpr u32 AP_COEF = 0xF8;      // float
constexpr u32 LPF_STATE = 0xFC;    // float [3]
constexpr u32 LPF_COEF = 0x108;    // float
constexpr u32 SKIP_FLAGS = 0x10C;  // u32: nonzero, the function clears bit 1 and returns
constexpr u32 CROSSTALK = 0x12C;   // float
constexpr u32 OUT_SCALE = 0x134;   // float
constexpr u32 BUS_IN = 0x138;      // s32** [3] or 0
constexpr u32 BUS_OUT = 0x13C;     // s32** [3] or 0
constexpr u32 OUT_GAIN = 0x140;    // float
constexpr u32 SEND_GAIN = 0x144;   // float
constexpr u32 STRUCT_SIZE = 0x148;
constexpr u32 SAMPLES = 96;

float Mul(float a, float b)
{
  return a * b;
}
float Add(float a, float b)
{
  return a + b;
}
float Sub(float a, float b)
{
  return a - b;
}
u32 Fctiwz(float f)
{
  const double d = f;
  if (std::isnan(d))
    return 0x80000000u;
  if (d >= 2147483648.0)
    return 0x7FFFFFFFu;
  if (d <= -2147483649.0)
    return 0x80000000u;
  return static_cast<u32>(static_cast<s32>(d));
}

// A guest array of big-endian words in host memory.
struct Line
{
  u8* p = nullptr;
  u32 len = 0;  // words
  u32 guest = 0;
  u32 Get(u32 i) const
  {
    u32 v;
    std::memcpy(&v, p + 4 * static_cast<size_t>(i), 4);
    return Common::swap32(v);
  }
};

inline float GetF(const u8* line, u32 i)
{
  u32 v;
  std::memcpy(&v, line + 4 * static_cast<size_t>(i), 4);
  return std::bit_cast<float>(Common::swap32(v));
}
inline void SetF(u8* line, u32 i, float f)
{
  const u32 v = Common::swap32(std::bit_cast<u32>(f));
  std::memcpy(line + 4 * static_cast<size_t>(i), &v, 4);
}
inline u32 GetW(const u8* line, u32 i)
{
  u32 v;
  std::memcpy(&v, line + 4 * static_cast<size_t>(i), 4);
  return Common::swap32(v);
}
inline void SetW(u8* line, u32 i, u32 w)
{
  const u32 v = Common::swap32(w);
  std::memcpy(line + 4 * static_cast<size_t>(i), &v, 4);
}

struct Consts
{
  float one, k_out, k_cross, zero;  // the function's constants (r2-relative)
};

struct Plan
{
  u32 rv = 0;
  Line early[3], pre[3], comb[3][3], ap[3][2], last[3], io[3], bus_in[3], bus_out[3];
  bool has_pre = false, has_bus_in = false, has_bus_out = false;
  u32 e_pos[3]{}, e_len = 0, pre_pos = 0, pre_len = 0, comb_pos[3]{}, comb_len[3]{}, ap_pos[2]{},
      ap_len[2]{}, last_pos[3]{}, last_len[3]{};
  float e_coef[3]{}, comb_coef[3]{}, ap_coef = 0, lpf = 0, lpf_state[3]{}, crosstalk = 0,
        out_scale = 0, out_gain = 0, send_gain = 0;
  Consts k{};
};

// `words` words at `addr` in host memory, or nullptr if they are not all in MEM1 or MEM2.
u8* HostRange(Memory::MemoryManager& memory, u32 addr, u32 words)
{
  const u64 bytes = 4ull * words;
  const u32 seg = addr >> 28;
  const u64 off = addr & 0x0FFFFFFF;
  if ((seg == 0x8 || seg == 0xC) && off + bytes <= memory.GetRamSizeReal())
    return memory.GetRAM() + off;
  if ((seg == 0x9 || seg == 0xD) && memory.GetEXRAM() && off + bytes <= memory.GetExRamSizeReal())
    return memory.GetEXRAM() + off;
  return nullptr;
}

// Instructions of the function this reproduces, read before every call: a mod or a Gecko code
// that changes the function gets its own code run.
constexpr std::array<std::pair<u32, u32>, 8> FUNCTION_WORDS{{
    {0x80202A58, 0x9421FF90},  // stwu r1, -0x70(r1)
    {0x80202A6C, 0x8004010C},  // lwz r0, 0x10C(r4)
    {0x80202B14, 0x7C892378},  // mr r9, r4 (the sample loop)
    {0x80202BA4, 0x7D19E42E},  // lfsx f8, r25, r28 (the first early tap)
    {0x80202D34, 0x4200FDFC},  // bdnz (the channel loop)
    {0x80202F50, 0x281F0060},  // cmplwi r31, 96
    {0x80202F54, 0x4180FBC0},  // blt (the sample loop)
    {0x80202F6C, 0x4E800020},  // blr
}};

bool IsTheFunction(Memory::MemoryManager& memory)
{
  for (const auto& [addr, word] : FUNCTION_WORDS)
  {
    if (memory.Read_U32(addr) != word)
      return false;
  }
  return true;
}

// Reads the call's state (r2, r3, r4). False if the guest code must run instead.
bool MakePlan(Memory::MemoryManager& memory, u32 rtoc, u32 bufs, u32 rv, Plan& p)
{
  const u8* const rvp = HostRange(memory, rv, STRUCT_SIZE / 4);
  if (!rvp || !IsTheFunction(memory))
    return false;
  const auto r32 = [&](u32 a) {
    if (a - rv < STRUCT_SIZE)
    {
      u32 v;
      std::memcpy(&v, rvp + (a - rv), 4);
      return Common::swap32(v);
    }
    return memory.Read_U32(a);
  };
  const auto rf = [&](u32 a) { return std::bit_cast<float>(r32(a)); };
  if (r32(rv + SKIP_FLAGS) != 0)
    return false;
  p = {};
  p.rv = rv;
  p.k = {rf(rtoc - 0x5A00), rf(rtoc - 0x59FC), rf(rtoc - 0x59F8), rf(rtoc - 0x5A04)};
  std::vector<std::pair<u64, u64>> ranges;  // every range touched, by physical address
  ranges.emplace_back(rv & 0x1FFFFFFF, (rv & 0x1FFFFFFF) + STRUCT_SIZE);
  bool ok = true;
  const auto line = [&](Line& l, u32 addr, u32 words) {
    l.guest = addr;
    l.len = words;
    l.p = words ? HostRange(memory, addr, words) : nullptr;
    if (!l.p)
      ok = false;
    else
      ranges.emplace_back(addr & 0x1FFFFFFF, (addr & 0x1FFFFFFF) + 4ull * words);
  };
  p.e_len = r32(rv + EARLY_LEN);
  p.pre_len = r32(rv + PRE_LEN);
  p.has_pre = p.pre_len != 0;
  p.pre_pos = r32(rv + PRE_POS);
  if (p.has_pre && p.pre_pos >= p.pre_len)
    return false;
  const u32 bus_in = r32(rv + BUS_IN), bus_out = r32(rv + BUS_OUT);
  p.has_bus_in = bus_in != 0;
  p.has_bus_out = bus_out != 0;
  for (u32 j = 0; j < 3; ++j)
  {
    p.e_pos[j] = r32(rv + EARLY_POS + 4 * j);
    p.e_coef[j] = rf(rv + EARLY_COEF + 4 * j);
    p.comb_pos[j] = r32(rv + COMB_POS + 4 * j);
    p.comb_len[j] = r32(rv + COMB_LEN + 4 * j);
    p.comb_coef[j] = rf(rv + COMB_COEF + 4 * j);
    if (p.e_pos[j] >= p.e_len || p.comb_pos[j] >= p.comb_len[j])
      return false;
  }
  for (u32 j = 0; j < 2; ++j)
  {
    p.ap_pos[j] = r32(rv + AP_POS + 4 * j);
    p.ap_len[j] = r32(rv + AP_LEN + 4 * j);
    if (p.ap_pos[j] >= p.ap_len[j])
      return false;
  }
  for (u32 ch = 0; ch < 3; ++ch)
  {
    line(p.early[ch], r32(rv + EARLY_LINE + 4 * ch), p.e_len);
    if (p.has_pre)
      line(p.pre[ch], r32(rv + PRE_LINE + 4 * ch), p.pre_len);
    for (u32 j = 0; j < 3; ++j)
      line(p.comb[ch][j], r32(rv + COMB_LINE + 12 * ch + 4 * j), p.comb_len[j]);
    for (u32 j = 0; j < 2; ++j)
      line(p.ap[ch][j], r32(rv + AP_LINE + 8 * ch + 4 * j), p.ap_len[j]);
    p.last_pos[ch] = r32(rv + LAST_POS + 4 * ch);
    p.last_len[ch] = r32(rv + LAST_LEN + 4 * ch);
    if (p.last_pos[ch] >= p.last_len[ch])
      return false;
    line(p.last[ch], r32(rv + LAST_LINE + 4 * ch), p.last_len[ch]);
    p.lpf_state[ch] = rf(rv + LPF_STATE + 4 * ch);
    line(p.io[ch], r32(bufs + 4 * ch), SAMPLES);
    if (p.has_bus_in)
      line(p.bus_in[ch], r32(bus_in + 4 * ch), SAMPLES);
    if (p.has_bus_out)
      line(p.bus_out[ch], r32(bus_out + 4 * ch), SAMPLES);
  }
  if (!ok)
    return false;
  std::sort(ranges.begin(), ranges.end());
  for (size_t n = 1; n < ranges.size(); ++n)
  {
    if (ranges[n].first < ranges[n - 1].second)
      return false;
  }
  p.ap_coef = rf(rv + AP_COEF);
  p.lpf = rf(rv + LPF_COEF);
  p.crosstalk = rf(rv + CROSSTALK);
  p.out_scale = rf(rv + OUT_SCALE);
  p.out_gain = rf(rv + OUT_GAIN);
  p.send_gain = rf(rv + SEND_GAIN);
  return true;
}

// The 96 samples (the function's loop at 0x80202B14), on the plan's lines.
void RunPlan(Plan& p)
{
  const Consts& k = p.k;
  const float lpf = p.lpf;
  const float lpf_rest = Sub(k.one, lpf);
  const float out_scale = Mul(k.k_out, p.out_scale);
  const float ap = p.ap_coef;
  const float cross = Mul(k.k_cross, p.crosstalk);
  const float out_gain = p.out_gain, send_gain = p.send_gain;
  // In locals: the lines are written through byte pointers, which could alias the plan.
  u8 *early[3], *pre[3], *comb[3][3], *ap0[3], *ap1[3], *last[3], *io[3], *bin[3], *bout[3];
  float lpf_state[3];
  u32 last_pos[3], last_len[3];
  for (u32 ch = 0; ch < 3; ++ch)
  {
    early[ch] = p.early[ch].p;
    pre[ch] = p.pre[ch].p;
    for (u32 j = 0; j < 3; ++j)
      comb[ch][j] = p.comb[ch][j].p;
    ap0[ch] = p.ap[ch][0].p;
    ap1[ch] = p.ap[ch][1].p;
    last[ch] = p.last[ch].p;
    io[ch] = p.io[ch].p;
    bin[ch] = p.bus_in[ch].p;
    bout[ch] = p.bus_out[ch].p;
    lpf_state[ch] = p.lpf_state[ch];
    last_pos[ch] = p.last_pos[ch];
    last_len[ch] = p.last_len[ch];
  }
  u32 e_pos[3] = {p.e_pos[0], p.e_pos[1], p.e_pos[2]};
  const u32 e_len = p.e_len;
  const float e_coef[3] = {p.e_coef[0], p.e_coef[1], p.e_coef[2]};
  u32 pre_pos = p.pre_pos;
  const u32 pre_len = p.pre_len;
  u32 comb_pos[3] = {p.comb_pos[0], p.comb_pos[1], p.comb_pos[2]};
  const u32 comb_len[3] = {p.comb_len[0], p.comb_len[1], p.comb_len[2]};
  const float comb_coef[3] = {p.comb_coef[0], p.comb_coef[1], p.comb_coef[2]};
  u32 ap_pos[2] = {p.ap_pos[0], p.ap_pos[1]};
  const u32 ap_len[2] = {p.ap_len[0], p.ap_len[1]};
  const bool has_pre = p.has_pre, has_bus_in = p.has_bus_in, has_bus_out = p.has_bus_out;

  // The channels are independent until the crosstalk: each stage runs for all three, so their
  // dependency chains overlap. Every operation's operands and grouping are the guest's.
  for (u32 i = 0; i < SAMPLES; ++i)
  {
    float early_sum[3], delayed[3], comb_sum[3], t[3];
    for (u32 ch = 0; ch < 3; ++ch)
    {
      u32 sample = GetW(io[ch], i);
      if (has_bus_in)
        sample += GetW(bin[ch], i);
      const float in = static_cast<float>(static_cast<double>(static_cast<s32>(sample)));
      const float e0 = Mul(e_coef[0], GetF(early[ch], e_pos[0]));
      const float e1 = Mul(e_coef[1], GetF(early[ch], e_pos[1]));
      const float e2 = Mul(e_coef[2], GetF(early[ch], e_pos[2]));
      early_sum[ch] = Add(e2, Add(e0, e1));
      delayed[ch] = in;
      if (has_pre)
      {
        delayed[ch] = GetF(pre[ch], pre_pos);
        SetF(pre[ch], pre_pos, in);
      }
      comb_sum[ch] = k.zero;
    }
    for (u32 j = 0; j < 3; ++j)
    {
      for (u32 ch = 0; ch < 3; ++ch)
      {
        const float old = GetF(comb[ch][j], comb_pos[j]);
        comb_sum[ch] = Add(comb_sum[ch], old);
        SetF(comb[ch][j], comb_pos[j], Add(delayed[ch], Mul(old, comb_coef[j])));
      }
    }
    for (u32 ch = 0; ch < 3; ++ch)
    {
      const float a0 = GetF(ap0[ch], ap_pos[0]);
      float v = Add(comb_sum[ch], Mul(a0, ap));
      SetF(ap0[ch], ap_pos[0], v);
      const float x0 = Sub(a0, Mul(v, ap));
      const float a1 = GetF(ap1[ch], ap_pos[1]);
      v = Add(x0, Mul(a1, ap));
      SetF(ap1[ch], ap_pos[1], v);
      const float x1 = Sub(a1, Mul(v, ap));
      const float filtered = Add(Mul(lpf_rest, x1), Mul(lpf, lpf_state[ch]));
      lpf_state[ch] = filtered;
      const float l = GetF(last[ch], last_pos[ch]);
      v = Add(filtered, Mul(l, ap));
      SetF(last[ch], last_pos[ch], v);
      const float out = Sub(l, Mul(v, ap));
      if (++last_pos[ch] >= last_len[ch])
        last_pos[ch] = 0;
      t[ch] = Add(Mul(out, out_scale), early_sum[ch]);
    }
    const float left = Add(t[0], Mul(Add(t[1], t[2]), cross));
    const float right = Add(t[1], Mul(Add(t[0], t[2]), cross));
    const float surround = Add(t[2], Mul(Add(t[0], t[1]), cross));
    SetW(io[0], i, Fctiwz(Mul(left, out_gain)));
    SetW(io[1], i, Fctiwz(Mul(right, out_gain)));
    SetW(io[2], i, Fctiwz(Mul(surround, out_gain)));
    if (has_bus_out)
    {
      SetW(bout[0], i, Fctiwz(Mul(left, send_gain)));
      SetW(bout[1], i, Fctiwz(Mul(right, send_gain)));
      SetW(bout[2], i, Fctiwz(Mul(surround, send_gain)));
    }
    for (u32 j = 0; j < 3; ++j)
    {
      if (++e_pos[j] >= e_len)
        e_pos[j] = 0;
    }
    if (has_pre && ++pre_pos >= pre_len)
      pre_pos = 0;
    for (u32 j = 0; j < 3; ++j)
    {
      if (++comb_pos[j] >= comb_len[j])
        comb_pos[j] = 0;
    }
    for (u32 j = 0; j < 2; ++j)
    {
      if (++ap_pos[j] >= ap_len[j])
        ap_pos[j] = 0;
    }
  }
  for (u32 ch = 0; ch < 3; ++ch)
  {
    p.lpf_state[ch] = lpf_state[ch];
    p.last_pos[ch] = last_pos[ch];
    p.e_pos[ch] = e_pos[ch];
    p.comb_pos[ch] = comb_pos[ch];
  }
  p.ap_pos[0] = ap_pos[0];
  p.ap_pos[1] = ap_pos[1];
  p.pre_pos = pre_pos;
}

// The struct's fields the run changed, back into guest memory (Write_U32 marks them dirty).
void StorePlan(Memory::MemoryManager& memory, const Plan& p)
{
  for (u32 j = 0; j < 3; ++j)
  {
    memory.Write_U32(p.e_pos[j], p.rv + EARLY_POS + 4 * j);
    memory.Write_U32(p.comb_pos[j], p.rv + COMB_POS + 4 * j);
    memory.Write_U32(p.last_pos[j], p.rv + LAST_POS + 4 * j);
    memory.Write_U32(std::bit_cast<u32>(p.lpf_state[j]), p.rv + LPF_STATE + 4 * j);
  }
  for (u32 j = 0; j < 2; ++j)
    memory.Write_U32(p.ap_pos[j], p.rv + AP_POS + 4 * j);
  if (p.has_pre)
    memory.Write_U32(p.pre_pos, p.rv + PRE_POS);
}

// Marks [addr, addr + bytes) in the rollback dirty bitmap, as MemoryManager::MarkRangeDirty does
// (ReverbHiHook runs the guest code when the machine session logs or watches writes).
void MarkDirty(u32 addr, u32 bytes)
{
  u8* const entries = Rollback::JITDirtyBitmap::Get().entries;
  const u32 phys = addr & 0x1FFFFFFFu;
  const u32 last = (phys + bytes - 1) >> ROLLBACK_PAGE_SHIFT;
  for (u32 g = phys >> ROLLBACK_PAGE_SHIFT; g <= last; ++g)
    entries[g] = 1;
}

// What RunPlan wrote of a line: the SAMPLES positions from `start` on, wrapping.
void MarkWindow(const Line& l, u32 start)
{
  if (!l.len)
    return;
  if (l.len <= SAMPLES)
  {
    MarkDirty(l.guest, 4 * l.len);
    return;
  }
  const u32 first = std::min(SAMPLES, l.len - start);
  MarkDirty(l.guest + 4 * start, 4 * first);
  if (first < SAMPLES)
    MarkDirty(l.guest, 4 * (SAMPLES - first));
}

void MarkPlan(const Plan& start)
{
  for (u32 ch = 0; ch < 3; ++ch)
  {
    if (start.has_pre)
      MarkWindow(start.pre[ch], start.pre_pos);
    for (u32 j = 0; j < 3; ++j)
      MarkWindow(start.comb[ch][j], start.comb_pos[j]);
    for (u32 j = 0; j < 2; ++j)
      MarkWindow(start.ap[ch][j], start.ap_pos[j]);
    MarkWindow(start.last[ch], start.last_pos[ch]);
    MarkDirty(start.io[ch].guest, 4 * SAMPLES);
    if (start.has_bus_out)
      MarkDirty(start.bus_out[ch].guest, 4 * SAMPLES);
  }
}

enum class Mode
{
  Guest,
  Native,
  Check,
};

Mode GetMode()
{
  static const Mode mode = [] {
    const char* v = std::getenv("PPR_REVERB_HLE");
    if (!v)
      return Mode::Native;
    if (std::strcmp(v, "check") == 0)
      return Mode::Check;
    return std::atoi(v) != 0 ? Mode::Native : Mode::Guest;
  }();
  return mode;
}

// The check mode: at the entry, RunPlan on copies of every line; at the return, the guest's result
// compared with them.
struct Check
{
  bool armed = false;
  u32 sp = 0;
  Plan plan;
  std::vector<std::vector<u8>> copies, entry;
  std::vector<std::pair<const Line*, const u8*>> lines;  // (the plan's line, its guest bytes)
  std::unordered_set<u32> shared;  // words someone else wrote while the guest code ran
  u64 calls = 0, compared = 0, differed = 0, interrupted = 0;
};
Check s_check;

void ArmCheck(Memory::MemoryManager& memory, u32 rtoc, u32 bufs, u32 rv, u32 sp)
{
  s_check.armed = false;
  if (!MakePlan(memory, rtoc, bufs, rv, s_check.plan))
    return;
  s_check.copies.clear();
  s_check.entry.clear();
  s_check.lines.clear();
  s_check.copies.reserve(64);
  s_check.entry.reserve(64);
  Plan& p = s_check.plan;
  const auto copy = [&](Line& l) {
    if (!l.p)
      return;
    s_check.copies.emplace_back(l.p, l.p + 4 * static_cast<size_t>(l.len));
    s_check.entry.emplace_back(l.p, l.p + 4 * static_cast<size_t>(l.len));
    s_check.lines.emplace_back(&l, l.p);
    l.p = s_check.copies.back().data();
  };
  for (u32 ch = 0; ch < 3; ++ch)
  {
    copy(p.early[ch]);
    copy(p.pre[ch]);
    for (auto& c : p.comb[ch])
      copy(c);
    for (auto& a : p.ap[ch])
      copy(a);
    copy(p.last[ch]);
    copy(p.io[ch]);
    copy(p.bus_in[ch]);
    copy(p.bus_out[ch]);
  }
  RunPlan(p);
  s_check.sp = sp;
  s_check.armed = true;
}

void FinishCheck(Memory::MemoryManager& memory)
{
  s_check.armed = false;
  ++s_check.calls;
  // A word this run left as it was but the guest's run changed was written by someone else while
  // the guest code ran (the JIT runs CoreTiming events between blocks; on Temple of Time a delay
  // line's tail overlaps memory an event writes). That call cannot be compared, and the word is
  // left out of later comparisons.
  bool external = false;
  for (size_t n = 0; n < s_check.lines.size(); ++n)
  {
    const auto& [l, guest] = s_check.lines[n];
    const u8* const at_entry = s_check.entry[n].data();
    for (u32 w = 0; w < l->len; ++w)
    {
      u32 g, e;
      std::memcpy(&g, guest + 4 * static_cast<size_t>(w), 4);
      std::memcpy(&e, at_entry + 4 * static_cast<size_t>(w), 4);
      if (g != e && Common::swap32(e) == l->Get(w))
      {
        external = true;
        s_check.shared.insert(l->guest + 4 * w);
      }
    }
  }
  if (external)
  {
    ++s_check.interrupted;
    return;
  }
  ++s_check.compared;
  u64 bad = 0;
  std::string detail;
  const auto note = [&](u32 addr, u32 got, u32 want) {
    if (got == want)
      return;
    if (bad < 4)
      detail += fmt::format(" [{:08x}: guest {:08x} hle {:08x}]", addr, got, want);
    ++bad;
  };
  for (const auto& [l, guest] : s_check.lines)
  {
    for (u32 w = 0; w < l->len; ++w)
    {
      if (s_check.shared.contains(l->guest + 4 * w))
        continue;
      u32 g;
      std::memcpy(&g, guest + 4 * static_cast<size_t>(w), 4);
      note(l->guest + 4 * w, Common::swap32(g), l->Get(w));
    }
  }
  const Plan& p = s_check.plan;
  for (u32 j = 0; j < 3; ++j)
  {
    note(p.rv + EARLY_POS + 4 * j, memory.Read_U32(p.rv + EARLY_POS + 4 * j), p.e_pos[j]);
    note(p.rv + COMB_POS + 4 * j, memory.Read_U32(p.rv + COMB_POS + 4 * j), p.comb_pos[j]);
    note(p.rv + LAST_POS + 4 * j, memory.Read_U32(p.rv + LAST_POS + 4 * j), p.last_pos[j]);
    note(p.rv + LPF_STATE + 4 * j, memory.Read_U32(p.rv + LPF_STATE + 4 * j),
         std::bit_cast<u32>(p.lpf_state[j]));
  }
  for (u32 j = 0; j < 2; ++j)
    note(p.rv + AP_POS + 4 * j, memory.Read_U32(p.rv + AP_POS + 4 * j), p.ap_pos[j]);
  if (p.has_pre)
    note(p.rv + PRE_POS, memory.Read_U32(p.rv + PRE_POS), p.pre_pos);
  if (bad)
  {
    ++s_check.differed;
    WARN_LOG_FMT(BRAWLBACK, "reverb hle check: call {}: {} words differ:{}", s_check.calls, bad,
                 detail);
  }
  if (s_check.calls % 2000 == 0)
  {
    INFO_LOG_FMT(BRAWLBACK,
                 "reverb hle check: {} calls, {} compared, {} differed, {} written meanwhile by "
                 "someone else ({} such words)",
                 s_check.calls, s_check.compared, s_check.differed, s_check.interrupted,
                 s_check.shared.size());
  }
}
}  // namespace

void ReverbHiHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  auto& memory = system.GetMemory();
  const Mode mode = GetMode();
  // Whole-machine sessions (Other Games) compare all of RAM between machines and don't install
  // this hook (HLE.cpp); should one be armed later, the guest code runs, interleaved with
  // CoreTiming events as on every other machine. So does it while the session logs or watches
  // guest writes.
  if (mode == Mode::Native && !Machine::IsArmed() && !Machine::g_io_log && !Machine::g_watch_hi)
  {
    static Plan plan;
    if (MakePlan(memory, ppc.gpr[2], ppc.gpr[3], ppc.gpr[4], plan))
    {
      const Plan start = plan;
      RunPlan(plan);
      StorePlan(memory, plan);
      MarkPlan(start);
      ppc.npc = LR(ppc);
      return;
    }
  }
  if (mode == Mode::Check)
    ArmCheck(memory, ppc.gpr[2], ppc.gpr[3], ppc.gpr[4], ppc.gpr[1]);
  // The guest code: run the replaced instruction, stwu r1, -0x70(r1), and go on.
  PowerPC::MMU::HostWrite<u32>(guard, ppc.gpr[1], ppc.gpr[1] - FRAME_SIZE);
  ppc.gpr[1] -= FRAME_SIZE;
  ppc.npc = FUNCTION + 4;
}

void ReverbHiReturnHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  if (s_check.armed && system.GetPPCState().gpr[1] == s_check.sp)
    FinishCheck(system.GetMemory());
}

u32 FunctionAddress()
{
  return FUNCTION;
}

u32 ReturnAddress()
{
  return FUNCTION_RETURN;
}
}  // namespace HLE_BrawlReverb
