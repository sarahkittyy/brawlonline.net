// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>

#include "Common/CommonTypes.h"

namespace VideoCommon
{
enum class OutputResamplingPassType : int
{
  // These values are shared with default_pre_post_process.glsl.
  None = 0,
  // One axis, point taps weighted by a table row per output pixel.
  Table = 1,
  // One axis, the seven Sharp 2021 taps.
  Sharp = 2,
  // Magic Kernel enlargement of one or both axes: the three nearest texels of each.
  Enlarge = 3,
};

enum class OutputResamplingKernel : int
{
  // Magic Kernel reduction.
  Reduce,
  // Magic Kernel reduction followed by Sharp 2021 on the reduced grid, as one set of weights.
  ReduceSharp,
};

struct OutputResamplingPass
{
  OutputResamplingPassType type = OutputResamplingPassType::None;
  // Size of this pass's target.
  u32 width = 0;
  u32 height = 0;
  // Table and Sharp: 0 for horizontal, 1 for vertical.
  u32 axis = 0;
  // Enlarge: bit 0 for horizontal, bit 1 for vertical.
  u32 enlarge_axes = 0;
  // Table: what its weights do, and the source and target lengths along its axis.
  OutputResamplingKernel kernel = OutputResamplingKernel::Reduce;
  u32 source_length = 0;
  u32 target_length = 0;
  // Table: weights per table row. Zero until the table has been made.
  u32 table_taps = 0;
  bool decode_input = false;
  bool final_pass = false;
};

struct OutputResamplingPlan
{
  std::array<OutputResamplingPass, 3> passes{};
  u32 count = 0;
};

// Each axis that shrinks is reduced and then sharpened on the smaller grid; each axis that
// grows is sharpened and then enlarged. Every case takes at most three passes:
//
//   both shrink:   Table(Reduce, a)   Table(ReduceSharp, b)   Sharp(a)
//   one shrinks:   Table(Reduce, a)   Sharp(a)
//   both grow:     Sharp(x)           Sharp(y)                Enlarge(x, y)
//   one grows:     Sharp(a)           Enlarge(a)
//   a shrinks,
//   b grows:       Sharp(b)           Table(ReduceSharp, a)   Enlarge(b)
//
// Only the first pass reads the gamma-encoded source, so it is the one with the fewest taps.
// When both axes shrink, the first pass takes the axis that leaves the smaller intermediate.
constexpr OutputResamplingPlan CreateOutputResamplingPlan(u32 source_width, u32 source_height,
                                                          u32 target_width, u32 target_height)
{
  OutputResamplingPlan plan;
  if (source_width == 0 || source_height == 0 || target_width == 0 || target_height == 0)
    return plan;

  const std::array<u32, 2> source{source_width, source_height};
  const std::array<u32, 2> target{target_width, target_height};
  std::array<u32, 2> size = source;
  const auto add_table = [&](u32 axis, OutputResamplingKernel kernel) {
    size[axis] = target[axis];
    OutputResamplingPass& pass = plan.passes[plan.count++];
    pass.type = OutputResamplingPassType::Table;
    pass.width = size[0];
    pass.height = size[1];
    pass.axis = axis;
    pass.kernel = kernel;
    pass.source_length = source[axis];
    pass.target_length = target[axis];
  };
  const auto add_sharp = [&](u32 axis) {
    OutputResamplingPass& pass = plan.passes[plan.count++];
    pass.type = OutputResamplingPassType::Sharp;
    pass.width = size[0];
    pass.height = size[1];
    pass.axis = axis;
  };
  const auto add_enlarge = [&](u32 axes) {
    OutputResamplingPass& pass = plan.passes[plan.count++];
    pass.type = OutputResamplingPassType::Enlarge;
    pass.width = target[0];
    pass.height = target[1];
    pass.enlarge_axes = axes;
  };

  const std::array<bool, 2> shrinks{target[0] < source[0], target[1] < source[1]};
  const std::array<bool, 2> grows{target[0] > source[0], target[1] > source[1]};
  const u32 grow_axes = (grows[0] ? 1u : 0u) | (grows[1] ? 2u : 0u);

  if (shrinks[0] && shrinks[1])
  {
    // Reducing the rows first leaves source_width x target_height texels, the columns first
    // target_width x source_height.
    const u32 first = static_cast<u64>(source[0]) * target[1] <=
                              static_cast<u64>(target[0]) * source[1] ?
                          1u :
                          0u;
    add_table(first, OutputResamplingKernel::Reduce);
    add_table(first ^ 1u, OutputResamplingKernel::ReduceSharp);
    add_sharp(first);
  }
  else if (shrinks[0] || shrinks[1])
  {
    const u32 axis = shrinks[0] ? 0u : 1u;
    if (grow_axes != 0)
    {
      add_sharp(axis ^ 1u);
      add_table(axis, OutputResamplingKernel::ReduceSharp);
      add_enlarge(grow_axes);
    }
    else
    {
      add_table(axis, OutputResamplingKernel::Reduce);
      add_sharp(axis);
    }
  }
  else if (grow_axes != 0)
  {
    if (grows[0])
      add_sharp(0);
    if (grows[1])
      add_sharp(1);
    add_enlarge(grow_axes);
  }

  if (plan.count != 0)
  {
    plan.passes[0].decode_input = true;
    plan.passes[plan.count - 1].final_pass = true;
  }
  return plan;
}

// John Costella's Sharp 2021 filter, [-1, 6, -35, 204, -35, 6, -1] / 144, blended with the
// identity by `sharpness` (0 to 1). Element i is the weight of the taps i texels from the center.
std::array<float, 4> GetOutputResamplingSharpWeights(float sharpness);

// Weights of a Table pass. Row i (of `rows`) is for output pixel i along the pass's axis and
// holds `taps + 1` values: the index of its first source texel, then one weight per texel from
// there on. Texels past the image edges are folded into the edge texels, every row sums to one,
// and first + taps never passes the end of the source.
struct OutputResamplingTable
{
  u32 taps = 0;
  u32 rows = 0;
  std::vector<float> data;
};

OutputResamplingTable CreateOutputResamplingTable(OutputResamplingKernel kernel, u32 source_length,
                                                  u32 target_length, float sharpness);
}  // namespace VideoCommon
