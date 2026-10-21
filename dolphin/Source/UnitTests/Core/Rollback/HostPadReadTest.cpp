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
  // it gets what was held at the due time.
  std::this_thread::sleep_for(4ms);
  Harness::Internal::PadSet(0, Pad(PAD_BUTTON_B));
  std::this_thread::sleep_for(6ms);
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
