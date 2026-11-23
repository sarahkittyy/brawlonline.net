// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/OutputResampling.h"

#include <algorithm>
#include <cmath>

namespace VideoCommon
{
namespace
{
constexpr std::array<double, 4> SHARP_2021{204.0 / 144.0, -35.0 / 144.0, 6.0 / 144.0,
                                           -1.0 / 144.0};

std::array<double, 4> SharpWeights(double sharpness)
{
  sharpness = std::clamp(sharpness, 0.0, 1.0);
  std::array<double, 4> weights{};
  for (size_t i = 0; i < weights.size(); ++i)
    weights[i] = sharpness * SHARP_2021[i];
  weights[0] += 1.0 - sharpness;
  return weights;
}

// The quadratic B-spline.
double MagicKernel(double x)
{
  x = std::abs(x);
  if (x < 0.5)
    return 0.75 - x * x;
  if (x < 1.5)
    return 0.5 * (x - 1.5) * (x - 1.5);
  return 0.0;
}

// One output pixel's weights over the source texels first .. first + weights.size() - 1.
struct Row
{
  s64 first = 0;
  std::vector<double> weights;
};

Row ReduceRow(u32 source_length, u32 target_length, u32 index)
{
  const double scale = static_cast<double>(source_length) / target_length;
  const double center = (index + 0.5) * scale - 0.5;
  const s64 first_tap = static_cast<s64>(std::ceil(center - 1.5 * scale));
  const s64 last_tap = static_cast<s64>(std::floor(center + 1.5 * scale));
  const s64 last_texel = static_cast<s64>(source_length) - 1;

  Row row;
  row.first = std::clamp<s64>(first_tap, 0, last_texel);
  row.weights.assign(std::clamp<s64>(last_tap, 0, last_texel) - row.first + 1, 0.0);
  double sum = 0.0;
  for (s64 tap = first_tap; tap <= last_tap; ++tap)
  {
    const double weight = MagicKernel((tap - center) / scale);
    row.weights[std::clamp<s64>(tap, 0, last_texel) - row.first] += weight;
    sum += weight;
  }
  for (double& weight : row.weights)
    weight /= sum;
  return row;
}
}  // namespace

std::array<float, 4> GetOutputResamplingSharpWeights(float sharpness)
{
  const std::array<double, 4> weights = SharpWeights(sharpness);
  return {static_cast<float>(weights[0]), static_cast<float>(weights[1]),
          static_cast<float>(weights[2]), static_cast<float>(weights[3])};
}

OutputResamplingTable CreateOutputResamplingTable(OutputResamplingKernel kernel, u32 source_length,
                                                  u32 target_length, float sharpness)
{
  OutputResamplingTable table;
  if (source_length == 0 || target_length == 0)
    return table;

  std::vector<Row> rows(target_length);
  for (u32 i = 0; i < target_length; ++i)
    rows[i] = ReduceRow(source_length, target_length, i);

  if (kernel == OutputResamplingKernel::ReduceSharp)
  {
    // Sharpening output pixel i mixes the reduced pixels i - 3 .. i + 3, clamped to the image.
    const std::array<double, 4> sharp = SharpWeights(sharpness);
    const s64 last_row = static_cast<s64>(target_length) - 1;
    std::vector<Row> sharpened(target_length);
    for (s64 i = 0; i <= last_row; ++i)
    {
      const Row& lowest = rows[std::max<s64>(i - 3, 0)];
      const Row& highest = rows[std::min<s64>(i + 3, last_row)];
      Row& out = sharpened[i];
      out.first = lowest.first;
      out.weights.assign(
          static_cast<size_t>(highest.first + static_cast<s64>(highest.weights.size()) - out.first),
          0.0);
      for (s64 k = -3; k <= 3; ++k)
      {
        const Row& row = rows[std::clamp<s64>(i + k, 0, last_row)];
        const double weight = sharp[std::abs(k)];
        for (size_t tap = 0; tap < row.weights.size(); ++tap)
          out.weights[row.first - out.first + tap] += weight * row.weights[tap];
      }
    }
    rows = std::move(sharpened);
  }

  // The kernel is zero at its ends, which land exactly on texels at some ratios (3:2, 4:3, ...):
  // drop those taps instead of fetching them.
  size_t taps = 0;
  for (Row& row : rows)
  {
    constexpr double NEGLIGIBLE = 1e-9;
    while (row.weights.size() > 1 && std::abs(row.weights.back()) < NEGLIGIBLE)
      row.weights.pop_back();
    size_t leading = 0;
    while (leading + 1 < row.weights.size() && std::abs(row.weights[leading]) < NEGLIGIBLE)
      ++leading;
    row.weights.erase(row.weights.begin(), row.weights.begin() + leading);
    row.first += static_cast<s64>(leading);
    taps = std::max(taps, row.weights.size());
  }

  table.taps = static_cast<u32>(taps);
  table.rows = target_length;
  table.data.assign((taps + 1) * target_length, 0.0f);
  for (u32 i = 0; i < target_length; ++i)
  {
    // Rows shorter than the longest one are padded with zero weights. Slide them back from the
    // end of the source where needed, so the shader never reads past it.
    const Row& row = rows[i];
    const s64 first =
        std::min(row.first, static_cast<s64>(source_length) - static_cast<s64>(taps));
    float* const out = &table.data[(taps + 1) * i];
    out[0] = static_cast<float>(first);
    for (size_t tap = 0; tap < row.weights.size(); ++tap)
      out[1 + (row.first - first) + tap] = static_cast<float>(row.weights[tap]);
  }
  return table;
}
}  // namespace VideoCommon
