// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <numbers>
#include <vector>

#include <gtest/gtest.h>

#include "VideoCommon/FrameInterpolation.h"

using FrameInterpolation::BlendLinear;
using FrameInterpolation::BlendRigid;
using FrameInterpolation::DrawKey;
using FrameInterpolation::KeyIndex;
using FrameInterpolation::Mat34;
using FrameInterpolation::MatchDraws;

namespace
{
// Rotation about Z by `angle`, scaled by `scale` on every axis, translated by (tx, ty, tz).
Mat34 RotZ(float angle, float scale, float tx, float ty, float tz)
{
  const float c = std::cos(angle) * scale;
  const float s = std::sin(angle) * scale;
  return {c, -s, 0, tx, s, c, 0, ty, 0, 0, scale, tz};
}

void ExpectNear(const Mat34& a, const Mat34& b, float tolerance)
{
  for (size_t i = 0; i < a.size(); ++i)
    EXPECT_NEAR(a[i], b[i], tolerance) << "element " << i;
}

DrawKey Key(u64 exact, u64 stable, float x, float y = 0.0f, float z = 0.0f)
{
  DrawKey key;
  key.exact = exact;
  key.mesh = exact;  // another mesh wherever the exact key differs, unless a test says otherwise
  key.stable = stable;
  key.perspective = true;
  key.translation = {x, y, z};
  return key;
}
}  // namespace

TEST(FrameInterpolation, RigidBlendTurnsAndMovesByTheWeight)
{
  // A quarter of the way from prev to cur (not the other way round), at the same size: a linear
  // blend of two rotations would shrink the object.
  const Mat34 prev = RotZ(0.0f, 2.0f, 0, 0, 0);
  const Mat34 cur = RotZ(std::numbers::pi_v<float> / 4, 2.0f, 12, -4, 2);
  Mat34 out;
  ASSERT_TRUE(BlendRigid(prev, cur, 0.25f, &out));
  // (A normalized lerp: within 0.002 rad of a quarter of the angle.)
  ExpectNear(out, RotZ(std::numbers::pi_v<float> / 16, 2.0f, 3, -1, 0.5f), 5e-3f);
}

TEST(FrameInterpolation, RigidBlendScalesAndMirrors)
{
  Mat34 prev = RotZ(0.1f, 1.0f, 0, 0, 0);
  Mat34 cur = RotZ(0.3f, 3.0f, 0, 0, 0);
  // Mirror X in both: still a rotation times a (negative) scale.
  for (int row = 0; row < 3; ++row)
  {
    prev[row * 4] = -prev[row * 4];
    cur[row * 4] = -cur[row * 4];
  }
  Mat34 out;
  ASSERT_TRUE(BlendRigid(prev, cur, 0.5f, &out));
  Mat34 expected = RotZ(0.2f, 2.0f, 0, 0, 0);
  for (int row = 0; row < 3; ++row)
    expected[row * 4] = -expected[row * 4];
  ExpectNear(out, expected, 1e-5f);
}

TEST(FrameInterpolation, RigidBlendFallsBackToLinear)
{
  // A shear is not a rotation times a scale.
  const Mat34 prev = {1, 0.5f, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  const Mat34 cur = {1, 0.7f, 0, 2, 0, 1, 0, 0, 0, 0, 1, 0};
  Mat34 out;
  EXPECT_FALSE(BlendRigid(prev, cur, 0.5f, &out));
  EXPECT_FLOAT_EQ(out[1], 0.6f);
  EXPECT_FLOAT_EQ(out[3], 1.0f);

  // More than 90 degrees in one frame is not one motion.
  Mat34 turned;
  EXPECT_FALSE(BlendRigid(RotZ(0.0f, 1.0f, 0, 0, 0), RotZ(2.0f, 1.0f, 0, 0, 0), 0.5f, &turned));

  // A draw that did not move is kept as it is, whatever its matrix.
  EXPECT_TRUE(BlendRigid(cur, cur, 0.5f, &out));
  EXPECT_EQ(out, cur);
}

TEST(FrameInterpolation, LinearBlendKeepsCurrentWhereNotFinite)
{
  const float prev[3] = {0.0f, NAN, 4.0f};
  const float cur[3] = {2.0f, 1.0f, INFINITY};
  float out[3];
  BlendLinear(prev, cur, 3, 0.25f, out);
  EXPECT_FLOAT_EQ(out[0], 0.5f);
  EXPECT_FLOAT_EQ(out[1], 1.0f);
  EXPECT_EQ(out[2], INFINITY);
}

TEST(FrameInterpolation, MatchesByExactKeyThenStableKey)
{
  std::vector<DrawKey> prev = {Key(1, 10, 0), Key(2, 20, 5), Key(3, 30, 9)};
  KeyIndex index;
  index.Build(prev);
  // Draw 0: same mesh. Draw 1: the same material, its vertices changed (stable key only).
  // Draw 2: nothing like it last frame.
  std::vector<DrawKey> cur = {Key(1, 10, 1), Key(99, 20, 6), Key(4, 40, 0)};
  std::vector<int> partner;
  MatchDraws(prev, index, &cur, &partner);
  EXPECT_EQ(partner, (std::vector<int>{0, 1, -1}));
  EXPECT_TRUE(cur[0].has_prediction);
  EXPECT_FLOAT_EQ(cur[0].predicted[0], 2.0f);  // moving +1 a frame
  EXPECT_FALSE(cur[2].has_prediction);
}

TEST(FrameInterpolation, AMeshWithAnotherMaterialPairsAfterExactMatches)
{
  // A fighter whose material changes: the same vertices and textures, another pipeline (neither
  // the exact nor the stable key is the previous frame's). Two copies of the mesh: the one whose
  // material did not change keeps its own partner.
  std::vector<DrawKey> prev = {Key(1, 10, 0), Key(1, 10, 50)};
  KeyIndex index;
  index.Build(prev);
  std::vector<DrawKey> cur = {Key(2, 20, 51), Key(1, 10, 49)};
  cur[0].mesh = prev[0].mesh;
  std::vector<int> partner;
  MatchDraws(prev, index, &cur, &partner);
  EXPECT_EQ(partner, (std::vector<int>{0, 1}));
}

TEST(FrameInterpolation, RepeatedMeshesPairWithTheNearest)
{
  // Three copies of one mesh, in another order this frame.
  std::vector<DrawKey> prev = {Key(7, 70, 0), Key(7, 70, 100), Key(7, 70, 200)};
  KeyIndex index;
  index.Build(prev);
  std::vector<DrawKey> cur = {Key(7, 70, 202), Key(7, 70, 3), Key(7, 70, 98)};
  std::vector<int> partner;
  MatchDraws(prev, index, &cur, &partner);
  EXPECT_EQ(partner, (std::vector<int>{2, 0, 1}));
}

TEST(FrameInterpolation, PredictionDecidesBetweenCrossingCopies)
{
  // Two copies crossing: by position they would swap, by where each was going they don't.
  std::vector<DrawKey> prev = {Key(5, 50, 0), Key(5, 50, 10)};
  prev[0].predicted = {6, 0, 0};
  prev[0].has_prediction = true;
  prev[1].predicted = {4, 0, 0};
  prev[1].has_prediction = true;
  KeyIndex index;
  index.Build(prev);
  std::vector<DrawKey> cur = {Key(5, 50, 4), Key(5, 50, 6)};
  std::vector<int> partner;
  MatchDraws(prev, index, &cur, &partner);
  EXPECT_EQ(partner, (std::vector<int>{1, 0}));
}

TEST(FrameInterpolation, ExactMatchTakesPriorityOverStableMatch)
{
  // The deformed mesh comes first, but cannot take the exact mesh's only partner.
  std::vector<DrawKey> prev = {Key(1, 10, 0)};
  KeyIndex index;
  index.Build(prev);
  std::vector<DrawKey> cur = {Key(2, 10, 0), Key(1, 10, 1)};
  std::vector<int> partner;
  MatchDraws(prev, index, &cur, &partner);
  EXPECT_EQ(partner, (std::vector<int>{-1, 0}));
}

TEST(FrameInterpolation, GateAndLayoutKeepDrawsApart)
{
  std::vector<DrawKey> prev = {Key(1, 10, 0), Key(2, 20, 0)};
  prev[1].per_vertex = true;
  prev[1].used_rows = 0b111;
  KeyIndex index;
  index.Build(prev);
  // Teleported past the gate; and a palette draw using other matrix rows.
  std::vector<DrawKey> cur = {Key(1, 10, FrameInterpolation::MATCH_GATE * 2), Key(2, 20, 0)};
  cur[1].per_vertex = true;
  cur[1].used_rows = 0b1111;
  std::vector<int> partner;
  MatchDraws(prev, index, &cur, &partner);
  EXPECT_EQ(partner, (std::vector<int>{-1, -1}));
}

TEST(FrameInterpolation, TwoDimensionalDrawsAreNotPaired)
{
  // Neither a 2D draw of the previous frame nor one of this frame, with a 3D draw of the same key.
  std::vector<DrawKey> prev = {Key(1, 10, 0), Key(2, 20, 0)};
  prev[0].perspective = false;
  KeyIndex index;
  index.Build(prev);
  std::vector<DrawKey> cur = {Key(1, 10, 0), Key(2, 20, 0)};
  cur[1].perspective = false;
  std::vector<int> partner;
  MatchDraws(prev, index, &cur, &partner);
  EXPECT_EQ(partner, (std::vector<int>{-1, -1}));
}

TEST(FrameInterpolation, ACrowdOfOneMeshPairsInDrawOrder)
{
  // Too many copies of one mesh to compare each with each (200 x 200): the same order both
  // frames, though the nearest of the previous frame is the next one along.
  std::vector<DrawKey> prev, cur;
  for (int i = 0; i < 200; ++i)
  {
    prev.push_back(Key(9, 90, 10.0f * i));
    cur.push_back(Key(9, 90, 10.0f * i + 6.0f));
  }
  KeyIndex index;
  index.Build(prev);
  std::vector<int> partner;
  MatchDraws(prev, index, &cur, &partner);
  for (int i = 0; i < 200; ++i)
    EXPECT_EQ(partner[i], i);
}
