// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// `pads`: the GameCube buttons the player's controllers press, as Dolphin reads them with the
// given User folder's controller settings (the launcher's netplay folder), for Settings > Clips >
// Manual Clip Hotkey: an adapter port reads the adapter, a standard controller port reads its
// mapped device through the controller interface. Nothing is booted and nothing is written.
//
// Prints JSON lines on stdout until stdin closes (or --seconds pass):
//   {"ports":[{"port":1,"kind":"adapter","device":"GameCube adapter","plugged":true}, ...],
//    "adapter":"detected"|"not_detected"|"error"|"unused","adapter_error":"..."}
// whenever that changes, and
//   {"buttons":N}
// whenever the buttons held on all those ports together change: PAD_BUTTON_* / PAD_TRIGGER_*
// bits, which are also the low bits of Brawl's gfPadStatus buttons (a replay's pads).

#include "DolphinTool/PadsCommand.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <OptionParser.h>
#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI_Device.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"
#include "UICommon/UICommon.h"

namespace DolphinTool
{
namespace
{
constexpr u16 BUTTONS = PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_DOWN | PAD_BUTTON_UP |
                        PAD_TRIGGER_Z | PAD_TRIGGER_R | PAD_TRIGGER_L | PAD_BUTTON_A |
                        PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y | PAD_BUTTON_START;

// The base config of this command: the GameCube ports' devices from Dolphin.ini and nothing else.
// Dolphin's own base loader also reads the Wii settings through the NAND, which writes SYSCONF
// back (SysConf's destructor) in a folder the player's Dolphin may have open. Never saved.
class PortsConfigLoader final : public Config::ConfigLayerLoader
{
public:
  PortsConfigLoader() : ConfigLayerLoader(Config::LayerType::Base) {}

  void Load(Config::Layer* layer) override
  {
    Common::IniFile ini;
    ini.Load(File::GetUserPath(F_DOLPHINCONFIG_IDX));
    const Common::IniFile::Section* core = ini.GetSection("Core");
    for (int i = 0; i < 4; ++i)
    {
      const Config::Location& location = Config::GetInfoForSIDevice(i).GetLocation();
      std::string value;
      if (core && core->Get(location.key, &value))
        layer->Set(location, value);
    }
  }

  void Save(Config::Layer*) override {}
};

void PrintLine(const picojson::value& v)
{
  std::fputs((v.serialize() + "\n").c_str(), stdout);
  std::fflush(stdout);
}
}  // namespace

int PadsCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser;
  parser.usage("usage: pads -u USER [options]...");

  parser.add_option("-u", "--user")
      .type("string")
      .action("store")
      .help("User folder whose controller settings (Dolphin.ini, GCPadNew.ini) are read.")
      .set_default("");

  parser.add_option("-s", "--seconds")
      .type("int")
      .action("store")
      .help("Stop after this many seconds (default 600).")
      .set_default(600);

  const optparse::Values& options = parser.parse_args(args);
  if (options["user"].empty())
  {
    std::fputs("Error: No user folder set\n", stderr);
    return EXIT_FAILURE;
  }

  // Not UICommon::Init: it restores and writes the Wii settings (SYSCONF) of a folder the
  // player's Dolphin may have open. Nothing is written: the config is PortsConfigLoader's, and
  // SConfig (which InputConfig asks for the game's ID) saves the config when deleted, so it never is.
  UICommon::SetUserDirectory(options["user"]);
  Config::Init();
  Config::AddLayer(std::make_unique<PortsConfigLoader>());
  SConfig::Init();

  g_controller_interface.Initialize(WindowSystemInfo{});
  GCAdapter::Init();
  Pad::Initialize();

  std::atomic<bool> stdin_closed{false};
  std::thread([&stdin_closed] {
    std::string line;
    while (std::getline(std::cin, line))
    {
    }
    stdin_closed = true;
  }).detach();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(static_cast<int>(options.get("seconds")));
  std::string last_status;
  int last_buttons = -1;
  while (!stdin_closed && std::chrono::steady_clock::now() < deadline)
  {
    g_controller_interface.UpdateInput();

    u16 buttons = 0;
    bool any_adapter = false;
    picojson::array ports;
    for (int i = 0; i < 4; ++i)
    {
      const auto si = Config::Get(Config::GetInfoForSIDevice(i));
      picojson::object port;
      port["port"] = picojson::value(static_cast<double>(i + 1));
      bool plugged = false;
      if (si == SerialInterface::SIDEVICE_WIIU_ADAPTER)
      {
        any_adapter = true;
        plugged = GCAdapter::DeviceConnected(i);
        if (plugged)
          buttons |= GCAdapter::Peek(i).button;
        port["kind"] = picojson::value("adapter");
        port["device"] = picojson::value("GameCube adapter");
      }
      else if (si == SerialInterface::SIDEVICE_GC_CONTROLLER)
      {
        const auto device = Pad::GetConfig()->GetController(i)->GetDefaultDevice();
        plugged = g_controller_interface.FindDevice(device) != nullptr;
        if (plugged)
          buttons |= Pad::GetStatus(i).button;
        port["kind"] = picojson::value("controller");
        port["device"] = picojson::value(device.ToString());
      }
      else
      {
        continue;
      }
      port["plugged"] = picojson::value(plugged);
      ports.emplace_back(std::move(port));
    }

    picojson::object status;
    status["ports"] = picojson::value(std::move(ports));
    const char* adapter_error = nullptr;
    if (!any_adapter)
      status["adapter"] = picojson::value("unused");
    else if (GCAdapter::IsDetected(&adapter_error))
      status["adapter"] = picojson::value("detected");
    else
      status["adapter"] = picojson::value(adapter_error ? "error" : "not_detected");
    if (adapter_error)
      status["adapter_error"] = picojson::value(adapter_error);
    const picojson::value status_value(std::move(status));
    if (std::string s = status_value.serialize(); s != last_status)
    {
      last_status = std::move(s);
      PrintLine(status_value);
    }

    buttons &= BUTTONS;
    if (buttons != last_buttons)
    {
      last_buttons = buttons;
      picojson::object line;
      line["buttons"] = picojson::value(static_cast<double>(buttons));
      PrintLine(picojson::value(std::move(line)));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }

  Pad::Shutdown();
  GCAdapter::Shutdown();
  g_controller_interface.Shutdown();
  return EXIT_SUCCESS;
}
}  // namespace DolphinTool
