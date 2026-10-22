// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// File description
// -------------
// Purpose of this file: Collect boot settings for Core::Init()

// Call sequence: This file has one of the first function called when a game is booted,
// the boot sequence in the code is:

// DolphinWX:    FrameTools.cpp         StartGame
// Core          BootManager.cpp        BootCore
//               Core.cpp               Init                     Thread creation
//                                      EmuThread                Calls CBoot::BootUp
//               Boot.cpp               CBoot::BootUp()
//                                      CBoot::EmulatedBS2_Wii() / GC() or Load_BS2()

#include "Core/BootManager.h"

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/BaseConfigLoader.h"
#include "Core/ConfigLoaders/NetPlayConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Sram.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/Rollback/MachineSession.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"

#include "DiscIO/Enums.h"

namespace BootManager
{
// Boot the ISO or file
bool BootCore(Core::System& system, std::unique_ptr<BootParameters> boot,
              const WindowSystemInfo& wsi)
{
  if (!boot)
    return false;

  SConfig& StartUp = SConfig::GetInstance();

  if (!StartUp.SetPathsAndGameMetadata(system, *boot))
    return false;

  // Movie settings
  auto& movie = system.GetMovie();
  if (movie.IsPlayingInput() && movie.IsConfigSaved())
  {
    for (ExpansionInterface::Slot slot : ExpansionInterface::MEMCARD_SLOTS)
    {
      if (movie.IsUsingMemcard(slot) && movie.IsStartingFromClearSave() && !system.IsWii())
      {
        const auto raw_path =
            File::GetUserPath(D_GCUSER_IDX) +
            fmt::format("Movie{}.raw", slot == ExpansionInterface::Slot::A ? 'A' : 'B');
        if (File::Exists(raw_path))
          File::Delete(raw_path);

        const auto movie_path = File::GetUserPath(D_GCUSER_IDX) + "Movie";
        if (File::Exists(movie_path))
          File::DeleteDirRecursively(movie_path);
      }
    }
  }

  if (NetPlay::IsNetPlayRunning())
  {
    const NetPlay::NetSettings* netplay_settings = boot->boot_session_data.GetNetplaySettings();
    if (!netplay_settings)
      return false;

    Config::AddLayer(ConfigLoaders::GenerateNetPlayConfigLoader(*netplay_settings));
    StartUp.bCopyWiiSaveNetplay = netplay_settings->savedata_load;
    StartUp.bIsSpectator = netplay_settings->spectator_mode;
  }
  else if (Machine::IsArmed())
  {
    // A whole-machine session (Brawl Mods): the settings that change what the game computes are
    // the session's, through the same layer as Dolphin's netplay.
    const NetPlay::NetSettings settings = Machine::MakeNetSettings();
    // Real Wii Remotes play through Dolphin's controller interface ("Connect Wii Remotes for
    // Emulated Controllers"): the session reads an emulated Wii Remote they drive and sends its
    // state, as for any other controller. Set before the layer makes every Wii Remote emulated, so
    // a real one connected now goes to the controller interface; only when one is set up as Real.
    Machine::ConnectRealWiimotesIfSetUp();
    Config::AddLayer(ConfigLoaders::GenerateNetPlayConfigLoader(settings));
    StartUp.bCopyWiiSaveNetplay = false;
    StartUp.bIsSpectator = false;
    // Every player's port is a standard controller (the session reads each player's own
    // controller, adapter or not, and sends it); the other ports have none.
    for (int i = 0; i < SerialInterface::MAX_SI_CHANNELS; ++i)
    {
      Config::SetCurrent(Config::GetInfoForSIDevice(i),
                         settings.pad_map[i] > 0 ? SerialInterface::SIDEVICE_GC_CONTROLLER :
                                                   SerialInterface::SIDEVICE_NONE);
    }
  }

  // Bluetooth passthrough hands the Bluetooth adapter to the game, so its Wii Remotes never reach
  // Dolphin's input: online games, rooms and replays cannot read or send them. And an adapter
  // libusb cannot open (Windows' own driver on it) stops the emulated console at boot: every P+
  // launch ended there, "The game did not reach its menus in time" (bug #99, launcher 0.1.91, Intel
  // 8087:0033). Brawl Online's Dolphin always emulates the Wii's Bluetooth; real Wii Remotes play
  // through "Real Wii Remote" or the controller interface. The player's file keeps its setting.
  if (Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
  {
    WARN_LOG_FMT(BOOT, "Bluetooth passthrough is on in the settings: emulating the Wii's "
                       "Bluetooth instead (Brawl Online reads Wii Remotes through Dolphin)");
    Config::SetCurrent(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED, false);
  }

  // Override out-of-region languages/countries to prevent games from crashing or behaving oddly
  if (!Config::Get(Config::MAIN_OVERRIDE_REGION_SETTINGS))
  {
    Config::SetCurrent(
        Config::MAIN_GC_LANGUAGE,
        DiscIO::ToGameCubeLanguage(StartUp.GetLanguageAdjustedForRegion(false, StartUp.m_region)));

    if (system.IsWii())
    {
      const u32 wii_language =
          static_cast<u32>(StartUp.GetLanguageAdjustedForRegion(true, StartUp.m_region));
      if (wii_language != Config::Get(Config::SYSCONF_LANGUAGE))
        Config::SetCurrent(Config::SYSCONF_LANGUAGE, wii_language);

      const u8 country_code = static_cast<u8>(Config::Get(Config::SYSCONF_COUNTRY));
      if (StartUp.m_region != DiscIO::SysConfCountryToRegion(country_code))
      {
        switch (StartUp.m_region)
        {
        case DiscIO::Region::NTSC_J:
          Config::SetCurrent(Config::SYSCONF_COUNTRY, 0x01);  // Japan
          break;
        case DiscIO::Region::NTSC_U:
          Config::SetCurrent(Config::SYSCONF_COUNTRY, 0x31);  // United States
          break;
        case DiscIO::Region::PAL:
          Config::SetCurrent(Config::SYSCONF_COUNTRY, 0x6c);  // Switzerland
          break;
        case DiscIO::Region::NTSC_K:
          Config::SetCurrent(Config::SYSCONF_COUNTRY, 0x88);  // South Korea
          break;
        default:
          break;
        }
      }
    }
  }

  // Some NTSC Wii games such as Doc Louis's Punch-Out!! and
  // 1942 (Virtual Console) crash if the PAL60 option is enabled
  if (system.IsWii() && DiscIO::IsNTSC(StartUp.m_region) && Config::Get(Config::SYSCONF_PAL60))
    Config::SetCurrent(Config::SYSCONF_PAL60, false);

  // Disable loading time emulation for Riivolution-patched games until we have proper emulation.
  if (!boot->riivolution_patches.empty())
    Config::SetCurrent(Config::MAIN_FAST_DISC_SPEED, true);

  if (system.IsTriforce())
  {
    // Attach Triforce Baseboard hardware if not overridden in any layer.
    // Users may set these in their GameConfig if they want them not automatically attached.
    if (GetActiveLayerForConfig(Config::MAIN_SERIAL_PORT_1) == Config::LayerType::Base)
    {
      Config::SetCurrent(Config::MAIN_SERIAL_PORT_1, ExpansionInterface::EXIDeviceType::Baseboard);
    }
    if (GetActiveLayerForConfig(Config::GetInfoForSIDevice(0)) == Config::LayerType::Base)
    {
      Config::SetCurrent(Config::GetInfoForSIDevice(0),
                         SerialInterface::SIDevices::SIDEVICE_AM_BASEBOARD);
    }

    // Mario Kart Arcade GP has widescreen heuristic issues.
    // All Triforce games are 4:3 so we'll just disable the heuristic for now.
    if (GetActiveLayerForConfig(Config::GFX_SUGGESTED_ASPECT_RATIO) == Config::LayerType::Base)
    {
      Config::SetCurrent(Config::GFX_SUGGESTED_ASPECT_RATIO, AspectMode::ForceStandard);
    }
  }

  system.Initialize();

  Core::UpdateWantDeterminism(system, /*initial*/ true);

  ConfigLoaders::TransferSYSCONFControlToGuest();

  if (system.IsWii())
  {
    Core::InitializeWiiRoot(Core::WantsDeterminism());

    // Ensure any new settings are written to the SYSCONF
    if (!Core::WantsDeterminism())
    {
      Core::BackupWiiSettings();
      ConfigLoaders::SaveToSYSCONF(Config::LayerType::Meta,
                                   ConfigLoaders::SkipIfControlledByGuest::No);
    }
    else
    {
      ConfigLoaders::SaveToSYSCONF(
          Config::LayerType::Meta, ConfigLoaders::SkipIfControlledByGuest::No,
          [](const Config::Location& location) {
            return Config::GetActiveLayerForConfig(location) >= Config::LayerType::Movie;
          });
    }
  }

  AchievementManager::GetInstance().CloseGame();

  const bool load_ipl = !system.IsWii() && !Config::Get(Config::MAIN_SKIP_IPL) &&
                        std::holds_alternative<BootParameters::Disc>(boot->parameters);
  if (load_ipl)
  {
    return Core::Init(
        system,
        std::make_unique<BootParameters>(
            BootParameters::IPL{StartUp.m_region,
                                std::move(std::get<BootParameters::Disc>(boot->parameters))},
            std::move(boot->boot_session_data)),
        wsi);
  }
  return Core::Init(system, std::move(boot), wsi);
}

void RestoreConfig()
{
  Core::ShutdownWiiRoot();

  if (Core::WiiRootIsTemporary())
  {
    ConfigLoaders::TransferSYSCONFControlFromGuest(ConfigLoaders::WriteBackChangedValues::No);
  }
  else
  {
    Core::RestoreWiiSettings(Core::RestoreReason::EmulationEnd);
    ConfigLoaders::TransferSYSCONFControlFromGuest(ConfigLoaders::WriteBackChangedValues::Yes);
  }

  Config::ClearCurrentRunLayer();
  Config::RemoveLayer(Config::LayerType::Movie);
  Config::RemoveLayer(Config::LayerType::Netplay);
  Config::RemoveLayer(Config::LayerType::GlobalGame);
  Config::RemoveLayer(Config::LayerType::LocalGame);
  SConfig::GetInstance().ResetRunningGameMetadata();
}

}  // namespace BootManager
