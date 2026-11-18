// Copyright 2026 brawlonline.net
// SPDX-License-Identifier: GPL-2.0-or-later

// Input suggestions in GekkoNet's input buffer (Gekko::InputBuffer, gekko_add_input_suggestion): a
// remote player's frame that has to be predicted gets the newest suggestion for it instead of the
// last input, and every prediction is still checked against the real input.

#include <array>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Core/Rollback/InputSuggest.h"
#include "input.h"

namespace
{
constexpr unsigned SIZE = 4;
constexpr unsigned char WINDOW = 10;

std::array<unsigned char, SIZE> In(unsigned char v)
{
  return {v, 0, 0, 0};
}

// A remote player's buffer with the inputs of frames 0..9 received (all 1).
Gekko::InputBuffer Remote()
{
  Gekko::InputBuffer b;
  b.Init(0, WINDOW, SIZE);
  for (int f = 0; f <= 9; ++f)
  {
    auto in = In(1);
    b.AddInput(f, in.data());
  }
  return b;
}

void Suggest(Gekko::InputBuffer& b, int frame, unsigned seq, unsigned char v)
{
  auto in = In(v);
  b.AddSuggestion(frame, seq, in.data());
}

void Receive(Gekko::InputBuffer& b, int frame, unsigned char v)
{
  auto in = In(v);
  b.AddInput(frame, in.data());
}

unsigned char Predict(Gekko::InputBuffer& b, int frame)
{
  auto in = b.GetInput(frame, true);
  EXPECT_EQ(in->frame, frame);
  return in->input[0];
}
}  // namespace

TEST(InputSuggest, PredictsWithEachFramesNewestSuggestion)
{
  auto b = Remote();
  Suggest(b, 11, 0, 7);
  Suggest(b, 13, 1, 8);
  EXPECT_EQ(Predict(b, 10), 1);  // before the first: the last input
  EXPECT_EQ(Predict(b, 11), 7);
  EXPECT_EQ(Predict(b, 12), 7);
  EXPECT_EQ(Predict(b, 13), 8);
  EXPECT_EQ(b.GetSuggestionStats().changed, 3u);
}

TEST(InputSuggest, NewestBySequenceWinsWhenReordered)
{
  auto b = Remote();
  Suggest(b, 10, 5, 8);
  Suggest(b, 10, 4, 7);  // older, arrived later
  EXPECT_EQ(Predict(b, 10), 8);
}

TEST(InputSuggest, DropsSuggestionsOutOfRange)
{
  auto b = Remote();
  Suggest(b, 9, 0, 7);     // its input came
  Suggest(b, -5, 1, 7);    // a peer's garbage
  Suggest(b, 1000, 2, 7);  // past the buffer
  EXPECT_EQ(b.GetSuggestionStats().stale, 3u);
  EXPECT_EQ(b.GetSuggestionStats().received, 0u);
  EXPECT_EQ(Predict(b, 10), 1);
}

TEST(InputSuggest, RightSuggestionAvoidsTheRollback)
{
  auto b = Remote();
  Suggest(b, 10, 0, 7);
  EXPECT_EQ(Predict(b, 10), 7);
  Receive(b, 10, 7);
  EXPECT_EQ(b.GetIncorrectPredictionFrame(), -1);
  EXPECT_EQ(b.GetSuggestionStats().helped, 1u);
}

TEST(InputSuggest, WrongSuggestionStillRollsBack)
{
  auto b = Remote();
  Suggest(b, 10, 0, 7);
  EXPECT_EQ(Predict(b, 10), 7);
  Receive(b, 10, 1);
  EXPECT_EQ(b.GetIncorrectPredictionFrame(), 10);
  EXPECT_EQ(b.GetSuggestionStats().hurt, 1u);
}

TEST(InputSuggest, LateSuggestionLeavesAPredictedFrameAlone)
{
  // Frame 10 ran with the last input; a suggestion that comes after must not change what the
  // buffer says it ran with, or the real input would match it and the rollback would be skipped.
  auto b = Remote();
  EXPECT_EQ(Predict(b, 10), 1);
  Suggest(b, 10, 0, 7);
  EXPECT_EQ(Predict(b, 10), 1);
  EXPECT_EQ(Predict(b, 11), 7);  // a frame predicted after it takes it
  Receive(b, 10, 7);
  EXPECT_EQ(b.GetIncorrectPredictionFrame(), 10);
}

TEST(InputSuggest, RefillAfterAWrongPredictionTakesNewerSuggestions)
{
  auto b = Remote();
  EXPECT_EQ(Predict(b, 10), 1);
  EXPECT_EQ(Predict(b, 11), 1);
  EXPECT_EQ(Predict(b, 12), 1);
  // A change for 12 arrives, then the real input of 10 (also changed): the re-run takes 10's input
  // for 11 and the suggestion for 12.
  Suggest(b, 12, 0, 8);
  Receive(b, 10, 5);
  EXPECT_EQ(b.GetIncorrectPredictionFrame(), 10);
  EXPECT_EQ(Predict(b, 11), 5);
  EXPECT_EQ(Predict(b, 12), 8);
}

TEST(InputSuggest, ConfirmedSuggestionIsTheLastInput)
{
  auto b = Remote();
  Suggest(b, 10, 0, 7);
  EXPECT_EQ(Predict(b, 10), 7);
  EXPECT_EQ(Predict(b, 11), 7);
  Receive(b, 10, 7);
  EXPECT_EQ(Predict(b, 12), 7);  // not the 1 from before the suggestion
  Receive(b, 11, 7);
  Receive(b, 12, 7);
  EXPECT_EQ(b.GetIncorrectPredictionFrame(), -1);
  // Without the suggestion 10 would have been wrong and 11, 12 re-run right: one check helped.
  EXPECT_EQ(b.GetSuggestionStats().helped, 1u);
  EXPECT_EQ(b.GetSuggestionStats().wrong, 0u);
}

TEST(InputSuggest, ShownWrongCountsFirstPredictions)
{
  // 10 and 11 shown with the last input; 10's real input changed. 11's refill takes it and is
  // right, but the screen showed 11 with the old input.
  auto b = Remote();
  EXPECT_EQ(Predict(b, 10), 1);
  EXPECT_EQ(Predict(b, 11), 1);
  Receive(b, 10, 5);
  Receive(b, 11, 5);
  EXPECT_EQ(b.GetSuggestionStats().wrong, 1u);
  EXPECT_EQ(b.GetSuggestionStats().shown_wrong, 2u);
}

// The judge (gekko_set_suggestion_judge): Slippi's leaky bucket over suggestions that did worse
// than the player's last input.
namespace
{
// A suggestion is worse when its first byte is wrong and the last input's was right.
unsigned int FirstByteJudge(const unsigned char* previous, const unsigned char* suggested,
                            const unsigned char* real)
{
  return suggested[0] != real[0] && previous[0] == real[0] ? 3 : 0;
}

struct JudgeScope
{
  JudgeScope() { gekko_set_suggestion_judge(FirstByteJudge); }
  ~JudgeScope() { gekko_set_suggestion_judge(nullptr); }
};
}  // namespace

TEST(InputSuggest, JudgeStopsFollowingAPlayerThatKeepsLying)
{
  JudgeScope judge;
  auto b = Remote();
  int f = 10;
  for (; f < 40 && b.GetSuggestionStats().ignored == 0; ++f)
  {
    Suggest(b, f, static_cast<unsigned>(f), 9);  // a press that never comes
    Receive(b, f, 1);
  }
  EXPECT_EQ(b.GetSuggestionStats().ignored, 1u);
  // 15 points of 3 each, draining one input in 200: the sixth wrong one tips it over.
  EXPECT_EQ(f, 16);
  // Its suggestions are not used any more.
  Suggest(b, f, 100, 9);
  EXPECT_EQ(Predict(b, f), 1);
}

TEST(InputSuggest, JudgeKeepsFollowingAnHonestPlayer)
{
  JudgeScope judge;
  auto b = Remote();
  // One wrong suggestion in 700 frames for a minute: each drains (3 points of 200 inputs) before
  // the next.
  for (int f = 10; f < 3610; ++f)
  {
    Suggest(b, f, static_cast<unsigned>(f), f % 700 == 0 ? 9 : 1);
    Receive(b, f, 1);
  }
  EXPECT_EQ(b.GetSuggestionStats().ignored, 0u);
  EXPECT_EQ(b.GetSuggestionStats().worse, 5u);
}

TEST(InputSuggest, NoJudgeNoBucket)
{
  auto b = Remote();
  for (int f = 10; f < 40; ++f)
  {
    Suggest(b, f, static_cast<unsigned>(f), 9);
    Receive(b, f, 1);
  }
  EXPECT_EQ(b.GetSuggestionStats().ignored, 0u);
  EXPECT_EQ(b.GetSuggestionStats().worse, 0u);
}

// The rules on gfPadStatus inputs (Gprb::Suggest::Meaningful, WorseCost).
namespace
{
std::array<u8, 0x40> Pad(s8 x = 0, s8 y = 0, s8 cx = 0, s8 cy = 0, u8 l = 0, u8 r = 0, u8 b2 = 0,
                         u8 b3 = 0)
{
  std::array<u8, 0x40> p{};
  p[2] = p[6] = b2;
  p[3] = p[7] = b3;
  p[0x30] = static_cast<u8>(x);
  p[0x31] = static_cast<u8>(y);
  p[0x32] = static_cast<u8>(cx);
  p[0x33] = static_cast<u8>(cy);
  p[0x34] = l;
  p[0x35] = r;
  return p;
}

bool Means(const std::array<u8, 0x40>& a, const std::array<u8, 0x40>& b)
{
  return Gprb::Suggest::Meaningful(a.data(), b.data());
}
}  // namespace

TEST(InputSuggest, MeaningfulChanges)
{
  // Buttons, both ways; not the D-pad.
  EXPECT_TRUE(Means(Pad(), Pad(0, 0, 0, 0, 0, 0, 0x01)));          // A
  EXPECT_TRUE(Means(Pad(0, 0, 0, 0, 0, 0, 0x10), Pad()));          // Start released
  EXPECT_TRUE(Means(Pad(), Pad(0, 0, 0, 0, 0, 0, 0, 0x10)));       // Z
  EXPECT_FALSE(Means(Pad(), Pad(0, 0, 0, 0, 0, 0, 0, 0x08)));      // D-pad up
  // Noise and moves inside a zone are not.
  EXPECT_FALSE(Means(Pad(0), Pad(3)));
  EXPECT_FALSE(Means(Pad(40), Pad(50)));
  EXPECT_FALSE(Means(Pad(0, 0, 0, 0, 10), Pad(0, 0, 0, 0, 20)));
  // Lines, both ways and both signs.
  EXPECT_TRUE(Means(Pad(0), Pad(28)));     // walk
  EXPECT_TRUE(Means(Pad(28), Pad(0)));
  EXPECT_TRUE(Means(Pad(0), Pad(-28)));
  EXPECT_TRUE(Means(Pad(50), Pad(71)));    // dash
  EXPECT_TRUE(Means(Pad(0, 0), Pad(0, 68)));    // tap jump
  EXPECT_TRUE(Means(Pad(0, 0), Pad(0, -61)));   // crouch
  // Triggers: light shield.
  EXPECT_TRUE(Means(Pad(0, 0, 0, 0, 0, 0), Pad(0, 0, 0, 0, 0, 80)));
  EXPECT_FALSE(Means(Pad(0, 0, 0, 0, 0, 0), Pad(0, 0, 0, 0, 0, 20)));
}

TEST(InputSuggest, WorseCostOfGfPadStatus)
{
  using Gprb::Suggest::WorseCost;
  const auto real = Pad(60, 0, 0, 0, 0, 0, 0x01);  // A, stick forward
  // A wrong button the last input had right: 3.
  EXPECT_EQ(WorseCost(real.data(), Pad(60).data(), real.data()), 3u);
  // Right where the last input was wrong: 0.
  EXPECT_EQ(WorseCost(Pad(60).data(), real.data(), real.data()), 0u);
  // Same buttons, a stick further off: 1; closer: 0.
  EXPECT_EQ(WorseCost(Pad(55, 0, 0, 0, 0, 0, 0x01).data(), Pad(20, 0, 0, 0, 0, 0, 0x01).data(),
                      real.data()),
            1u);
  EXPECT_EQ(WorseCost(Pad(20, 0, 0, 0, 0, 0, 0x01).data(), Pad(55, 0, 0, 0, 0, 0, 0x01).data(),
                      real.data()),
            0u);
  // A stick that jitters inside a zone is no worse (bytes further off, zones the same).
  EXPECT_EQ(WorseCost(Pad(48, 0, 0, 0, 0, 0, 0x01).data(), Pad(46, 0, 0, 0, 0, 0, 0x01).data(),
                      Pad(47, 0, 0, 0, 0, 0, 0x01).data()),
            0u);
}

// Profile (run with --gtest_also_run_disabled_tests --gtest_filter=*InputSuggestBench*): the cost
// per frame of a remote player's buffer with and without suggestions and the judge, and of the
// sender's filter, on random gfPadStatus-sized inputs. Prints ns per operation.
#include <chrono>
#include <cstdio>
#include <random>

TEST(InputSuggest, DISABLED_InputSuggestBench)
{
  using Clk = std::chrono::steady_clock;
  constexpr unsigned BIG = 0x40;
  std::mt19937 rng(35);
  auto rand_pad = [&](std::array<unsigned char, BIG>& p) {
    for (auto& b : p)
      b = static_cast<unsigned char>(rng() % 3 == 0 ? rng() : 0);
  };
  for (const bool with_suggestions : {false, true, false, true})
  {
    if (with_suggestions)
      gekko_set_suggestion_judge(Gprb::Suggest::WorseCost);
    Gekko::InputBuffer b;
    b.Init(0, 8, BIG);
    std::array<unsigned char, BIG> in{};
    b.AddInput(0, in.data());
    constexpr int FRAMES = 200000;
    // The remote's input arrives 3 frames late: each frame predicts 3 ahead, then confirms one.
    const auto t0 = Clk::now();
    for (int f = 1; f < FRAMES; ++f)
    {
      if (with_suggestions && f % 4 == 0)
      {
        rand_pad(in);
        b.AddSuggestion(f + 2, static_cast<u32>(f), in.data());
      }
      for (int k = 0; k < 3; ++k)
        (void)b.GetInput(f + k, true);
      rand_pad(in);
      b.AddInput(f, in.data());
      b.ClearIncorrectFrames(f);
    }
    const double ns = std::chrono::duration<double, std::nano>(Clk::now() - t0).count() / FRAMES;
    std::printf("buffer frame (3 predictions + 1 input%s): %.1f ns\n",
                with_suggestions ? ", a suggestion every 4 frames, judge" : "", ns);
    gekko_set_suggestion_judge(nullptr);
  }
  std::array<unsigned char, BIG> a{}, c{}, d{};
  unsigned sink = 0;
  constexpr int N = 2000000;
  const auto t1 = Clk::now();
  for (int i = 0; i < N; ++i)
  {
    rand_pad(a);
    sink += Gprb::Suggest::Meaningful(c.data(), a.data());
    c = a;
  }
  const auto t2 = Clk::now();
  for (int i = 0; i < N; ++i)
  {
    rand_pad(a);
    sink += Gprb::Suggest::WorseCost(c.data(), a.data(), d.data());
    d = c;
    c = a;
  }
  const auto t3 = Clk::now();
  for (int i = 0; i < N; ++i)
  {
    rand_pad(a);
    sink += a[5];
  }
  const auto t4 = Clk::now();
  const double base = std::chrono::duration<double, std::nano>(t4 - t3).count() / N;
  std::printf("Meaningful: %.1f ns, WorseCost: %.1f ns (random pad generation %.1f ns subtracted; "
              "sink %u)\n",
              std::chrono::duration<double, std::nano>(t2 - t1).count() / N - base,
              std::chrono::duration<double, std::nano>(t3 - t2).count() / N - base, base, sink);
}
