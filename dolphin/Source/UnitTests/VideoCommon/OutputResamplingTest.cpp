// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "VideoCommon/OutputResampling.h"

namespace VideoCommon
{
namespace
{
using Type = OutputResamplingPassType;
using Kernel = OutputResamplingKernel;

constexpr u32 HORIZONTAL = 0;
constexpr u32 VERTICAL = 1;

void ExpectTable(const OutputResamplingPass& pass, u32 axis, Kernel kernel, u32 source_length,
                 u32 target_length, u32 width, u32 height)
{
  EXPECT_EQ(pass.type, Type::Table);
  EXPECT_EQ(pass.axis, axis);
  EXPECT_EQ(pass.kernel, kernel);
  EXPECT_EQ(pass.source_length, source_length);
  EXPECT_EQ(pass.target_length, target_length);
  EXPECT_EQ(pass.width, width);
  EXPECT_EQ(pass.height, height);
}

void ExpectSharp(const OutputResamplingPass& pass, u32 axis, u32 width, u32 height)
{
  EXPECT_EQ(pass.type, Type::Sharp);
  EXPECT_EQ(pass.axis, axis);
  EXPECT_EQ(pass.width, width);
  EXPECT_EQ(pass.height, height);
}

void ExpectEnlarge(const OutputResamplingPass& pass, u32 axes, u32 width, u32 height)
{
  EXPECT_EQ(pass.type, Type::Enlarge);
  EXPECT_EQ(pass.enlarge_axes, axes);
  EXPECT_EQ(pass.width, width);
  EXPECT_EQ(pass.height, height);
}

// The reference the tables are checked against: dense matrices in the plainest form.
using Matrix = std::vector<std::vector<double>>;

double MagicKernel(double x)
{
  x = std::abs(x);
  return x < 0.5 ? 0.75 - x * x : x < 1.5 ? 0.5 * (x - 1.5) * (x - 1.5) : 0.0;
}

Matrix ReduceMatrix(u32 source, u32 target)
{
  Matrix matrix(target, std::vector<double>(source, 0.0));
  const double scale = static_cast<double>(source) / target;
  for (u32 out = 0; out < target; ++out)
  {
    const double center = (out + 0.5) * scale - 0.5;
    double sum = 0.0;
    for (int tap = static_cast<int>(std::ceil(center - 1.5 * scale));
         tap <= static_cast<int>(std::floor(center + 1.5 * scale)); ++tap)
    {
      const double weight = MagicKernel((tap - center) / scale);
      matrix[out][std::clamp(tap, 0, static_cast<int>(source) - 1)] += weight;
      sum += weight;
    }
    for (double& weight : matrix[out])
      weight /= sum;
  }
  return matrix;
}

Matrix SharpenRows(const Matrix& reduced, double sharpness)
{
  const double sharp[4] = {204.0 / 144.0, -35.0 / 144.0, 6.0 / 144.0, -1.0 / 144.0};
  const int rows = static_cast<int>(reduced.size());
  Matrix matrix(rows, std::vector<double>(reduced[0].size(), 0.0));
  for (int i = 0; i < rows; ++i)
  {
    for (int k = -3; k <= 3; ++k)
    {
      const auto& row = reduced[std::clamp(i + k, 0, rows - 1)];
      const double weight = sharpness * sharp[std::abs(k)] + (k == 0 ? 1.0 - sharpness : 0.0);
      for (size_t tap = 0; tap < row.size(); ++tap)
        matrix[i][tap] += weight * row[tap];
    }
  }
  return matrix;
}

void ExpectTableMatches(Kernel kernel, u32 source, u32 target, float sharpness)
{
  SCOPED_TRACE(::testing::Message() << source << " -> " << target << ", sharpness " << sharpness);
  const OutputResamplingTable table =
      CreateOutputResamplingTable(kernel, source, target, sharpness);
  Matrix expected = ReduceMatrix(source, target);
  if (kernel == Kernel::ReduceSharp)
    expected = SharpenRows(expected, sharpness);

  ASSERT_EQ(table.rows, target);
  ASSERT_GE(table.taps, 1u);
  ASSERT_LE(table.taps, source);
  ASSERT_EQ(table.data.size(), static_cast<size_t>(table.taps + 1) * target);
  for (u32 row = 0; row < target; ++row)
  {
    const float* const values = &table.data[static_cast<size_t>(table.taps + 1) * row];
    const float first = values[0];
    ASSERT_EQ(first, std::floor(first));
    ASSERT_GE(first, 0.0f);
    ASSERT_LE(static_cast<u32>(first) + table.taps, source);

    std::vector<double> actual(source, 0.0);
    double sum = 0.0;
    for (u32 tap = 0; tap < table.taps; ++tap)
    {
      actual[static_cast<u32>(first) + tap] = values[1 + tap];
      sum += values[1 + tap];
    }
    EXPECT_NEAR(sum, 1.0, 1e-6) << "row " << row;
    for (u32 texel = 0; texel < source; ++texel)
      ASSERT_NEAR(actual[texel], expected[row][texel], 1e-6) << "row " << row << " texel " << texel;
  }
}
}  // namespace

TEST(OutputResampling, BothAxesShrinkReducesTheSmallerIntermediateFirst)
{
  // 2560x1920 -> 1920x1080: rows first leaves 2560x1080, columns first 1920x1920.
  auto plan = CreateOutputResamplingPlan(2560, 1920, 1920, 1080);
  ASSERT_EQ(plan.count, 3u);
  ExpectTable(plan.passes[0], VERTICAL, Kernel::Reduce, 1920, 1080, 2560, 1080);
  ExpectTable(plan.passes[1], HORIZONTAL, Kernel::ReduceSharp, 2560, 1920, 1920, 1080);
  ExpectSharp(plan.passes[2], VERTICAL, 1920, 1080);

  // 3840x1080 -> 1280x720: columns first leaves 1280x1080, rows first 3840x720.
  plan = CreateOutputResamplingPlan(3840, 1080, 1280, 720);
  ASSERT_EQ(plan.count, 3u);
  ExpectTable(plan.passes[0], HORIZONTAL, Kernel::Reduce, 3840, 1280, 1280, 1080);
  ExpectTable(plan.passes[1], VERTICAL, Kernel::ReduceSharp, 1080, 720, 1280, 720);
  ExpectSharp(plan.passes[2], HORIZONTAL, 1280, 720);
}

TEST(OutputResampling, OneAxisShrinks)
{
  auto plan = CreateOutputResamplingPlan(1920, 1584, 1920, 1080);
  ASSERT_EQ(plan.count, 2u);
  ExpectTable(plan.passes[0], VERTICAL, Kernel::Reduce, 1584, 1080, 1920, 1080);
  ExpectSharp(plan.passes[1], VERTICAL, 1920, 1080);

  plan = CreateOutputResamplingPlan(2560, 1080, 1920, 1080);
  ASSERT_EQ(plan.count, 2u);
  ExpectTable(plan.passes[0], HORIZONTAL, Kernel::Reduce, 2560, 1920, 1920, 1080);
  ExpectSharp(plan.passes[1], HORIZONTAL, 1920, 1080);
}

TEST(OutputResampling, EnlargingSharpensOnTheSourceGrid)
{
  auto plan = CreateOutputResamplingPlan(640, 528, 1920, 1080);
  ASSERT_EQ(plan.count, 3u);
  ExpectSharp(plan.passes[0], HORIZONTAL, 640, 528);
  ExpectSharp(plan.passes[1], VERTICAL, 640, 528);
  ExpectEnlarge(plan.passes[2], 3, 1920, 1080);

  plan = CreateOutputResamplingPlan(640, 1080, 1920, 1080);
  ASSERT_EQ(plan.count, 2u);
  ExpectSharp(plan.passes[0], HORIZONTAL, 640, 1080);
  ExpectEnlarge(plan.passes[1], 1, 1920, 1080);

  plan = CreateOutputResamplingPlan(1920, 528, 1920, 1080);
  ASSERT_EQ(plan.count, 2u);
  ExpectSharp(plan.passes[0], VERTICAL, 1920, 528);
  ExpectEnlarge(plan.passes[1], 2, 1920, 1080);
}

TEST(OutputResampling, OneAxisShrinksWhileTheOtherGrows)
{
  auto plan = CreateOutputResamplingPlan(1920, 1584, 2560, 1440);
  ASSERT_EQ(plan.count, 3u);
  ExpectSharp(plan.passes[0], HORIZONTAL, 1920, 1584);
  ExpectTable(plan.passes[1], VERTICAL, Kernel::ReduceSharp, 1584, 1440, 1920, 1440);
  ExpectEnlarge(plan.passes[2], 1, 2560, 1440);

  plan = CreateOutputResamplingPlan(2560, 528, 1920, 1080);
  ASSERT_EQ(plan.count, 3u);
  ExpectSharp(plan.passes[0], VERTICAL, 2560, 528);
  ExpectTable(plan.passes[1], HORIZONTAL, Kernel::ReduceSharp, 2560, 1920, 1920, 528);
  ExpectEnlarge(plan.passes[2], 2, 1920, 1080);
}

TEST(OutputResampling, EveryScaleDirectionEndsOnTheTargetSize)
{
  for (const u32 width : {16u, 32u, 64u})
  {
    for (const u32 height : {12u, 24u, 48u})
    {
      SCOPED_TRACE(::testing::Message() << width << "x" << height);
      const auto plan = CreateOutputResamplingPlan(32, 24, width, height);
      if (width == 32 && height == 24)
      {
        EXPECT_EQ(plan.count, 0u);
        continue;
      }
      ASSERT_GE(plan.count, 2u);
      ASSERT_LE(plan.count, 3u);
      // Sharp 2021 runs once per changed axis: on its own, or inside a table.
      std::array<u32, 2> sharpened{};
      for (u32 i = 0; i < plan.count; ++i)
      {
        const auto& pass = plan.passes[i];
        EXPECT_EQ(pass.decode_input, i == 0);
        EXPECT_EQ(pass.final_pass, i == plan.count - 1);
        // Sharp 2021 comes before the enlargement of an axis.
        if (i == 0)
        {
          EXPECT_NE(pass.type, Type::Enlarge);
        }
        if (pass.type == Type::Sharp)
          ++sharpened[pass.axis];
        else if (pass.type == Type::Table && pass.kernel == Kernel::ReduceSharp)
          ++sharpened[pass.axis];
      }
      EXPECT_EQ(sharpened[0], width != 32 ? 1u : 0u);
      EXPECT_EQ(sharpened[1], height != 24 ? 1u : 0u);
      EXPECT_EQ(plan.passes[plan.count - 1].width, width);
      EXPECT_EQ(plan.passes[plan.count - 1].height, height);
    }
  }
}

TEST(OutputResampling, InvalidDimensionsDoNotProducePasses)
{
  EXPECT_EQ(CreateOutputResamplingPlan(0, 24, 16, 12).count, 0u);
  EXPECT_EQ(CreateOutputResamplingPlan(32, 0, 16, 12).count, 0u);
  EXPECT_EQ(CreateOutputResamplingPlan(32, 24, 0, 12).count, 0u);
  EXPECT_EQ(CreateOutputResamplingPlan(32, 24, 16, 0).count, 0u);
}

TEST(OutputResampling, SharpWeights)
{
  const auto full = GetOutputResamplingSharpWeights(1.0f);
  EXPECT_FLOAT_EQ(full[0], 204.0f / 144.0f);
  EXPECT_FLOAT_EQ(full[1], -35.0f / 144.0f);
  EXPECT_FLOAT_EQ(full[2], 6.0f / 144.0f);
  EXPECT_FLOAT_EQ(full[3], -1.0f / 144.0f);

  const auto none = GetOutputResamplingSharpWeights(0.0f);
  EXPECT_EQ(none[0], 1.0f);
  EXPECT_EQ(none[1], 0.0f);
  EXPECT_EQ(none[2], 0.0f);
  EXPECT_EQ(none[3], 0.0f);

  const auto half = GetOutputResamplingSharpWeights(0.5f);
  EXPECT_NEAR(half[0] + 2.0f * (half[1] + half[2] + half[3]), 1.0f, 1e-6f);
}

TEST(OutputResampling, TablesMatchTheDenseReference)
{
  const std::pair<u32, u32> sizes[] = {
      {2560, 1920}, {1920, 1080}, {64, 16},  {39, 17}, {513, 7}, {23, 11},
      {23, 1},      {19, 5},      {2, 1},    {7, 6},   {9, 8},   {1000, 3},
  };
  for (const auto& [source, target] : sizes)
  {
    ExpectTableMatches(Kernel::Reduce, source, target, 1.0f);
    for (const float sharpness : {0.0f, 0.35f, 1.0f})
      ExpectTableMatches(Kernel::ReduceSharp, source, target, sharpness);
  }
}

TEST(OutputResampling, TableSizes)
{
  // The Magic Kernel spans 3 target pixels and with Sharp 2021 9: at 4:3, 4 and 12 texels.
  EXPECT_EQ(CreateOutputResamplingTable(Kernel::Reduce, 2560, 1920, 1.0f).taps, 4u);
  EXPECT_EQ(CreateOutputResamplingTable(Kernel::ReduceSharp, 2560, 1920, 1.0f).taps, 12u);
  // A reduction to one pixel is the mean of the whole source.
  const auto mean = CreateOutputResamplingTable(Kernel::Reduce, 2, 1, 1.0f);
  ASSERT_EQ(mean.taps, 2u);
  EXPECT_EQ(mean.data[0], 0.0f);
  EXPECT_FLOAT_EQ(mean.data[1], 0.5f);
  EXPECT_FLOAT_EQ(mean.data[2], 0.5f);
  EXPECT_EQ(CreateOutputResamplingTable(Kernel::Reduce, 0, 1, 1.0f).rows, 0u);
}
}  // namespace VideoCommon
