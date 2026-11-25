// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Dolphin's Controllers settings for an engine that is not Dolphin (BrawlOnline/DolphinPads.h).
// What Dolphin does for its GameCube ports, without the rest of Dolphin: InputConfig::LoadConfig's
// read of GCPadNew.ini (no per-game profiles: Brawl Online's netplay User has none), the devices
// changed hook that binds the configs again, and Core::UpdateInputGate's gate at each read.

#include "BrawlOnline/DolphinPads.h"

#include <array>
#include <filesystem>
#include <mutex>
#include <system_error>

#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/WindowSystemInfo.h"
#include "Core/HW/GCPadEmu.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace BrawlOnline::DolphinPads
{
namespace
{
std::mutex s_mutex;
bool s_initialized = false;
bool s_active = false;
std::array<std::unique_ptr<GCPad>, 4> s_pads;
std::array<int, 4> s_si_devices{SI_GC_ADAPTER, SI_GC_ADAPTER, SI_GC_ADAPTER, SI_GC_ADAPTER};
bool s_background_input = false;
std::string s_user_folder;
std::filesystem::file_time_type s_pads_time{};
std::filesystem::file_time_type s_dolphin_time{};
Common::EventHook s_devices_changed;
std::function<bool()> s_has_focus;

std::filesystem::path ConfigPath(const std::string& name)
{
  return std::filesystem::path(s_user_folder) / "Config" / name;
}

std::filesystem::file_time_type TimeOf(const std::filesystem::path& path)
{
  std::error_code ec;
  const auto time = std::filesystem::last_write_time(path, ec);
  return ec ? std::filesystem::file_time_type{} : time;
}

void BindAll()
{
  for (auto& pad : s_pads)
  {
    if (pad)
      pad->UpdateReferences(g_controller_interface);
  }
}

// Called with s_mutex held.
bool LoadLocked()
{
  const auto pads_path = ConfigPath("GCPadNew.ini");
  const auto dolphin_path = ConfigPath("Dolphin.ini");
  s_pads_time = TimeOf(pads_path);
  s_dolphin_time = TimeOf(dolphin_path);

  Common::IniFile dolphin;
  dolphin.Load(dolphin_path.string());
  for (int i = 0; i < 4; ++i)
  {
    // Dolphin's default for every port is the GameCube adapter (Config::GetInfoForSIDevice).
    int si = SI_GC_ADAPTER;
    dolphin.GetOrCreateSection("Core")->Get("SIDevice" + std::to_string(i), &si, SI_GC_ADAPTER);
    s_si_devices[i] = si;
  }
  dolphin.GetOrCreateSection("Input")->Get("BackgroundInput", &s_background_input, false);

  Common::IniFile pads;
  std::error_code ec;
  if (!std::filesystem::exists(pads_path, ec) || !pads.Load(pads_path.string()) ||
      pads.GetSections().empty())
  {
    s_active = false;
    return false;
  }
  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  for (int i = 0; i < 4; ++i)
  {
    if (!s_pads[i])
      s_pads[i] = std::make_unique<GCPad>(i);
    Common::IniFile::Section config = *pads.GetOrCreateSection(s_pads[i]->GetName());
    s_pads[i]->LoadConfig(&config);
    s_pads[i]->UpdateReferences(g_controller_interface);
  }
  s_active = true;
  return true;
}
}  // namespace

bool Load(const std::string& user_folder, void* window)
{
  std::lock_guard lk(s_mutex);
  s_user_folder = user_folder;
  if (!s_initialized)
  {
    s_initialized = true;
#ifdef _WIN32
    const WindowSystemInfo wsi(WindowSystemType::Windows, nullptr, window, window);
#elif defined(__APPLE__)
    // The game window's NSView: Quartz's keyboard and mouse.
    const WindowSystemInfo wsi(WindowSystemType::MacOS, nullptr, window, window);
#else
    const WindowSystemInfo wsi(WindowSystemType::Headless, nullptr, window, window);
#endif
    g_controller_interface.Initialize(wsi);
    s_devices_changed = g_controller_interface.RegisterDevicesChangedCallback([] {
      const auto lock = ControllerEmu::EmulatedController::GetStateLock();
      BindAll();
    });
  }
  const bool loaded = LoadLocked();
  for (int i = 0; i < 4; ++i)
  {
    INFO_LOG_FMT(CONTROLLERINTERFACE, "Dolphin's controls: port {} SIDevice {} device {}", i + 1,
                 s_si_devices[i], loaded && s_pads[i] ? s_pads[i]->GetDefaultDevice().ToString() : "-");
  }
  return loaded;
}

bool Active()
{
  std::lock_guard lk(s_mutex);
  return s_active;
}

bool ReloadIfChanged()
{
  std::lock_guard lk(s_mutex);
  if (!s_initialized)
    return false;
  if (TimeOf(ConfigPath("GCPadNew.ini")) == s_pads_time &&
      TimeOf(ConfigPath("Dolphin.ini")) == s_dolphin_time)
  {
    return false;
  }
  LoadLocked();
  return true;
}

int SiDevice(int port)
{
  std::lock_guard lk(s_mutex);
  return port >= 0 && port < 4 ? s_si_devices[port] : SI_NONE;
}

void UpdateInput(bool window_has_focus)
{
  {
    std::lock_guard lk(s_mutex);
    if (!s_initialized)
      return;
    // Core::UpdateInputGate(!BackgroundInput): the controls count only with the game in front.
    ControlReference::SetInputGate(window_has_focus || s_background_input);
  }
  ControllerInterface::SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
  g_controller_interface.UpdateInput();
}

GCPadStatus GetStatus(int port)
{
  std::lock_guard lk(s_mutex);
  if (!s_active || port < 0 || port >= 4 || !s_pads[port])
    return {};
  ControllerInterface::SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
  return s_pads[port]->GetInput();
}

void SetRumble(int port, double strength)
{
  std::lock_guard lk(s_mutex);
  if (s_active && port >= 0 && port < 4 && s_pads[port])
    s_pads[port]->SetOutput(strength);
}

std::string DefaultDevice(int port)
{
  std::lock_guard lk(s_mutex);
  if (!s_active || port < 0 || port >= 4 || !s_pads[port])
    return {};
  return s_pads[port]->GetDefaultDevice().ToString();
}

void AddDevice(std::shared_ptr<ciface::Core::Device> device)
{
  g_controller_interface.AddDevice(std::move(device));
}

void RemoveDevices(const std::function<bool(const ciface::Core::Device*)>& which)
{
  g_controller_interface.RemoveDevice(which);
}

#if !BRAWL_ONLINE_DOLPHIN_INPUT_SDL
// Built without SDL (SdlDevices.cpp): no SDL devices to list.
void SyncSdlDevices()
{
}
#endif

void SetFocusQuery(std::function<bool()> has_focus)
{
  std::lock_guard lk(s_mutex);
  s_has_focus = std::move(has_focus);
}

bool HasFocus()
{
  std::lock_guard lk(s_mutex);
  return s_has_focus ? s_has_focus() : false;
}

void Shutdown()
{
  std::lock_guard lk(s_mutex);
  if (!s_initialized)
    return;
  s_devices_changed.reset();
  for (auto& pad : s_pads)
    pad.reset();
  g_controller_interface.Shutdown();
  s_initialized = false;
  s_active = false;
}
}  // namespace BrawlOnline::DolphinPads
