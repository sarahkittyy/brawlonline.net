// Copyright 2026 brawlonline.net
// SPDX-License-Identifier: GPL-2.0-or-later

// Dolphin's controllers for input suggestions (InputSuggestSource.h).

#include "Core/Rollback/InputSuggestSource.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"

namespace Gprb::Suggest::Source
{
void SetAdapterReportHook(ReportHook hook)
{
  GCAdapter::SetReportHook(hook);
}

bool HasSamplerThread()
{
  return true;
}

void SamplerThreadStarts()
{
  // Its own input channel: a relative input (a mouse's motion) gives each channel the motion since
  // that channel's last update, so the SI poll's is left alone.
  g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Suggestions);
}

std::optional<GCPadStatus> SampleMapped(int chan)
{
  if (!g_controller_interface.IsInit() || !Pad::IsInitialized())
    return std::nullopt;
  g_controller_interface.UpdateInput();
  // As the CPU thread before an SI poll: the controllers count only while the game window has
  // focus, unless background input is on.
  Core::UpdateInputGate(!Config::Get(Config::MAIN_INPUT_BACKGROUND_INPUT),
                        Config::Get(Config::MAIN_LOCK_CURSOR));
  return Pad::GetStatus(chan);
}
}  // namespace Gprb::Suggest::Source
