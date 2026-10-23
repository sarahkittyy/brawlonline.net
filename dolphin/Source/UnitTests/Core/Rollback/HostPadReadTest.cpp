// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Gprb::ReadHostPad (bug report #70): a network session's reads of the local controllers stand
// for their instant on the throttle's schedule. The harness's pad_set is the controller here (the
// Wii U adapter source without an adapter reads only the harness).

#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Harness/HarnessInternal.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/GameplayRollback.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"
#include "UICommon/UICommon.h"

using namespace std::chrono_literals;

namespace
{
class HostPadReadTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_profile_path = File::CreateTempDir();
    ASSERT_FALSE(m_profile_path.empty());
    Core::DeclareAsCPUThread();
    UICommon::SetUserDirectory(m_profile_path);
    Config::Init();
    SConfig::Init();
    m_system.GetPowerPC().Init(PowerPC::CPUCore::Interpreter);
    m_system.GetCoreTiming().Init();
  }
  void TearDown() override
  {
    Gprb::SetHostPadsScheduled(false);
    Harness::Internal::PadClear(0);
    m_system.GetCoreTiming().Shutdown();
    m_system.GetPowerPC().Shutdown();
    SConfig::Shutdown();
    Config::Shutdown();
    Core::UndeclareAsCPUThread();
    File::DeleteDirRecursively(m_profile_path);
  }

  static GCPadStatus Pad(u16 buttons)
  {
    GCPadStatus pad{};
    pad.button = buttons;
    pad.isConnected = true;
    return pad;
  }

  u16 Read() { return Gprb::ReadHostPad(m_system, 0, Gprb::HostPadSource::Adapter).button; }

  static void Spin(std::chrono::microseconds d)
  {
    const auto until = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < until)
    {
    }
  }

  Core::System& m_system = Core::System::GetInstance();
  std::string m_profile_path;
};
}  // namespace

TEST_F(HostPadReadTest, ReadsStandForTheirScheduledInstant)
{
  auto& ct = m_system.GetCoreTiming();
  Gprb::SetHostPadsScheduled(true);
  Gprb::TakeHostPadReadStats();

  // A held, then the schedule anchored: emulated time stands at an instant due now.
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_A));
  std::this_thread::sleep_for(2ms);
  ct.ResetThrottleToNow();
  ASSERT_TRUE(ct.ScheduledHostTime().has_value());
  EXPECT_EQ(Read(), PAD_BUTTON_A);

  // B pressed after the instant was due, and the read comes 10 ms late (a frame that ran long):
  // it gets what was held at the due time. (Waits that spin: a short sleep_for can take a
  // scheduler quantum on Windows, and a read more than a field late reads now.)
  Spin(4ms);
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_B));
  Spin(6ms);
  EXPECT_EQ(Read(), PAD_BUTTON_A);

  // A rollback's re-run: every read is the last one before it, whatever is held now.
  ct.SetRollbackResimulating(true);
  ASSERT_FALSE(ct.ScheduledHostTime().has_value());
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_X));
  EXPECT_EQ(Read(), PAD_BUTTON_A);
  EXPECT_EQ(Read(), PAD_BUTTON_A);

  // Presented again, on schedule (the throttle anchored now): read now.
  ct.SetRollbackResimulating(false);
  EXPECT_EQ(Read(), PAD_BUTTON_X);

  const Gprb::HostPadReadStats st = Gprb::TakeHostPadReadStats();
  EXPECT_EQ(st.on_time, 2u);
  EXPECT_EQ(st.late_history, 1u);
  EXPECT_EQ(st.late_live, 0u);
  EXPECT_EQ(st.latched, 2u);
  EXPECT_EQ(st.relatched, 0u);
  EXPECT_GE(st.late_max_ms, 9.0);
}

TEST_F(HostPadReadTest, ReadsFarBehindScheduleTakeTheControllerNow)
{
  auto& ct = m_system.GetCoreTiming();
  Gprb::SetHostPadsScheduled(true);
  Gprb::TakeHostPadReadStats();
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_A));
  ct.ResetThrottleToNow();
  EXPECT_EQ(Read(), PAD_BUTTON_A);

  // A PC that cannot keep full speed: the read comes 40 ms (over a field) behind its due instant.
  // Not the controller of then (A) but the one now (B).
  std::this_thread::sleep_for(10ms);
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_B));
  std::this_thread::sleep_for(30ms);
  EXPECT_EQ(Read(), PAD_BUTTON_B);

  // The re-run right after takes that read (it is fresh, whatever instant it stands for).
  ct.SetRollbackResimulating(true);
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_X));
  EXPECT_EQ(Read(), PAD_BUTTON_B);
  ct.SetRollbackResimulating(false);

  const Gprb::HostPadReadStats st = Gprb::TakeHostPadReadStats();
  EXPECT_EQ(st.on_time, 1u);
  EXPECT_EQ(st.late_far, 1u);
  EXPECT_EQ(st.late_history, 0u);
  EXPECT_EQ(st.latched, 1u);
  EXPECT_EQ(st.relatched, 0u);
}

TEST_F(HostPadReadTest, AReRunAfterALongWaitReadsAnew)
{
  auto& ct = m_system.GetCoreTiming();
  Gprb::SetHostPadsScheduled(true);
  Gprb::TakeHostPadReadStats();
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_A));
  ct.ResetThrottleToNow();
  EXPECT_EQ(Read(), PAD_BUTTON_A);

  // Emulated time stood still for longer than the latch lasts (a wait for the peer).
  std::this_thread::sleep_for(120ms);
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_Y));
  ct.SetRollbackResimulating(true);
  EXPECT_EQ(Read(), PAD_BUTTON_Y);  // read anew, and that read stands for the rest of the re-run
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_B));
  EXPECT_EQ(Read(), PAD_BUTTON_Y);
  ct.SetRollbackResimulating(false);

  const Gprb::HostPadReadStats st = Gprb::TakeHostPadReadStats();
  EXPECT_EQ(st.relatched, 1u);
  EXPECT_EQ(st.latched, 1u);
}

TEST_F(HostPadReadTest, OutsideASessionEveryReadIsNow)
{
  auto& ct = m_system.GetCoreTiming();
  Gprb::SetHostPadsScheduled(false);
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_A));
  ct.ResetThrottleToNow();
  std::this_thread::sleep_for(5ms);
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_B));
  EXPECT_EQ(Read(), PAD_BUTTON_B);
  ct.SetRollbackResimulating(true);
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_X));
  EXPECT_EQ(Read(), PAD_BUTTON_X);
  ct.SetRollbackResimulating(false);

  // A new session starts without the last session's reads.
  Gprb::SetHostPadsScheduled(true);
  ct.SetRollbackResimulating(true);
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_Y));
  EXPECT_EQ(Read(), PAD_BUTTON_Y);
  ct.SetRollbackResimulating(false);
}

// Host decode (GitHub issue #1): a read made into the gfPadStatus the pad thread makes of it.
TEST(HostDecodePadSlot, ACentredControllerIsItsRawValuesLessTheCentre)
{
  GCPadStatus st{};
  st.button = PAD_BUTTON_A | PAD_TRIGGER_Z | PAD_BUTTON_START | PAD_USE_ORIGIN | PAD_GET_ORIGIN;
  st.stickX = 200;
  st.stickY = 30;
  st.substickX = 128;
  st.substickY = 255;
  st.triggerLeft = 90;
  st.triggerRight = 0;
  st.isConnected = true;
  std::array<u8, 0x40> pad{};
  Gprb::HostDecodePadSlot(st, {0x80, 0x80, 0x80, 0x80, 0, 0}, pad.data());
  const u16 buttons = PAD_BUTTON_A | PAD_TRIGGER_Z | PAD_BUTTON_START;  // not the SI's origin bits
  EXPECT_EQ(pad[2], buttons >> 8);
  EXPECT_EQ(pad[3], buttons & 0xFF);
  EXPECT_EQ(pad[6], pad[2]);
  EXPECT_EQ(pad[7], pad[3]);
  EXPECT_EQ(static_cast<s8>(pad[0x30]), 72);
  EXPECT_EQ(static_cast<s8>(pad[0x31]), -98);
  EXPECT_EQ(static_cast<s8>(pad[0x32]), 0);
  EXPECT_EQ(static_cast<s8>(pad[0x33]), 127);
  EXPECT_EQ(pad[0x34], 90);
  EXPECT_EQ(pad[0x35], 0);
  EXPECT_EQ(pad[0x38], 0);
  for (const int i : {0, 1, 4, 5, 8, 0x10, 0x18, 0x2C, 0x36, 0x37, 0x39, 0x3C, 0x3F})
    EXPECT_EQ(pad[i], 0) << i;
}

TEST(HostDecodePadSlot, AnOffCentreOriginIsTakenOutAsTheSdkDoes)
{
  // A Wii U adapter's controller resting at stick 134,120, triggers 20/30 (PADRead's ClampS8 /
  // ClampU8): the rest reads 0, the far ends stay in range.
  const std::array<u8, 6> origin{134, 120, 128, 128, 20, 30};
  GCPadStatus st{};
  st.isConnected = true;
  std::array<u8, 0x40> pad{};
  st.stickX = 134;
  st.stickY = 120;
  st.substickX = 128;
  st.substickY = 128;
  st.triggerLeft = 20;
  st.triggerRight = 10;
  Gprb::HostDecodePadSlot(st, origin, pad.data());
  EXPECT_EQ(static_cast<s8>(pad[0x30]), 0);
  EXPECT_EQ(static_cast<s8>(pad[0x31]), 0);
  EXPECT_EQ(pad[0x34], 0);
  EXPECT_EQ(pad[0x35], 0);  // below its origin: 0
  st.stickX = 0;    // -128 + 6: held at -122, then less the origin: -128
  st.stickY = 255;  // 127 - 8: held at 119, less the origin: 127
  st.triggerLeft = 255;
  Gprb::HostDecodePadSlot(st, origin, pad.data());
  EXPECT_EQ(static_cast<s8>(pad[0x30]), -128);
  EXPECT_EQ(static_cast<s8>(pad[0x31]), 127);
  EXPECT_EQ(pad[0x34], 235);
}

TEST(HostDecodePadSlot, NoControllerIsTheErrorByte)
{
  GCPadStatus st{};
  st.isConnected = false;
  std::array<u8, 0x40> pad{};
  pad.fill(0x55);
  Gprb::HostDecodePadSlot(st, {0x80, 0x80, 0x80, 0x80, 0, 0}, pad.data());
  EXPECT_EQ(pad[0x38], 0xFF);
}

TEST_F(HostPadReadTest, TheNewestScheduledReadIsDecoded)
{
  auto& ct = m_system.GetCoreTiming();
  Gprb::SetHostPadsScheduled(true);
  Gprb::TakeHostPadReadStats();
  Gprb::HostPadOriginReported(0, 0x80, 0x80, 0x80, 0x80, 0, 0);
  std::array<u8, 0x40> pad{};
  std::chrono::steady_clock::time_point at{};
  // B held (stick right), then the schedule anchored: the read is on time.
  GCPadStatus st = Pad(PAD_BUTTON_B);
  st.stickX = 160;
  Harness::Internal::PadSet(0, st);
  std::this_thread::sleep_for(2ms);
  ct.ResetThrottleToNow();
  EXPECT_EQ(Read(), PAD_BUTTON_B);
  ASSERT_TRUE(Gprb::PadsHostDecoded(0, &pad, &at));
  EXPECT_EQ(pad[2], PAD_BUTTON_B >> 8);
  EXPECT_EQ(static_cast<s8>(pad[0x30]), 32);
  EXPECT_NE(at, std::chrono::steady_clock::time_point{});
  // A re-run's latched read is the same read: the same decode.
  ct.SetRollbackResimulating(true);
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_X));
  EXPECT_EQ(Read(), PAD_BUTTON_B);
  ct.SetRollbackResimulating(false);
  std::chrono::steady_clock::time_point at2{};
  ASSERT_TRUE(Gprb::PadsHostDecoded(0, &pad, &at2));
  EXPECT_EQ(at2, at);
  EXPECT_EQ(pad[2], PAD_BUTTON_B >> 8);
  // Outside a session nothing is decoded.
  Gprb::SetHostPadsScheduled(false);
  EXPECT_FALSE(Gprb::PadsHostDecoded(0, &pad, &at));
}
