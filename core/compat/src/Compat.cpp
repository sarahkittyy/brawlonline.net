// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The compat stand-ins' definitions (core/standalone only, docs/design/brawl-online-core.md §10):
// Dolphin's Core/ answers the core in Dolphin; here the host (BrawlOnline::Host) does.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

#include "BrawlOnline/Compat.h"
#include "BrawlOnline/Host.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/IniFile.h"
#include "Common/Swap.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Present.h"

namespace BrawlOnline
{
namespace
{
Host* s_host = nullptr;
Common::HookableEvent<Core::State> s_state_event;
}  // namespace

Host& GetHost()
{
  if (!s_host)
  {
    std::fputs("BrawlOnline: no host set (SetHost at start-up)\n", stderr);
    std::abort();
  }
  return *s_host;
}

void SetHost(Host* host)
{
  s_host = host;
}

bool HasHost()
{
  return s_host != nullptr;
}

void NotifyStopping()
{
  s_state_event.Trigger(Core::State::Stopping);
}

namespace
{
// Dolphin's base config layer (Core/ConfigLoaders/BaseConfigLoader.cpp): the user's INI files.
class IniLayerLoader final : public Config::ConfigLayerLoader
{
public:
  IniLayerLoader() : ConfigLayerLoader(Config::LayerType::Base) {}

  void Load(Config::Layer* layer) override
  {
    for (const auto& [system, file] : Files())
    {
      Common::IniFile ini;
      ini.Load(File::GetUserPath(file));
      for (const auto& section : ini.GetSections())
      {
        for (const auto& [key, value] : section.GetValues())
          layer->Set(Config::Location{system, section.GetName(), key}, value);
      }
    }
  }

  void Save(Config::Layer* layer) override
  {
    for (const auto& [system, file] : Files())
    {
      Common::IniFile ini;
      ini.Load(File::GetUserPath(file));
      for (const auto& config : layer->GetLayerMap())
      {
        const Config::Location& location = config.first;
        if (location.system != system || !config.second)
          continue;
        ini.GetOrCreateSection(location.section)->Set(location.key, *config.second);
      }
      ini.Save(File::GetUserPath(file));
    }
  }

private:
  static std::vector<std::pair<Config::System, unsigned>> Files()
  {
    return {{Config::System::Main, F_DOLPHINCONFIG_IDX},
            {Config::System::GFX, F_GFXCONFIG_IDX},
            {Config::System::Logger, F_LOGGERCONFIG_IDX}};
  }
};
}  // namespace

void InitCompat(const std::string& user_dir)
{
  File::SetUserPath(D_USER_IDX, user_dir);
  File::CreateFullPath(File::GetUserPath(D_CONFIG_IDX));
  File::CreateFullPath(File::GetUserPath(D_LOGS_IDX));
  Config::Init();
  Config::AddLayer(std::make_unique<IniLayerLoader>());
}

void ShutdownCompat()
{
  Config::Shutdown();
}
}  // namespace BrawlOnline

// ---- Core/Core.h --------------------------------------------------------------------------------
namespace Core
{
Common::EventHook AddOnStateChangedCallback(StateChangedCallbackFunc callback)
{
  return BrawlOnline::s_state_event.Register(std::move(callback));
}

void Stop(System&)
{
  BrawlOnline::GetHost().StopGame();
}

void QueueHostJob(std::function<void(System&)> job, bool)
{
  BrawlOnline::GetHost().QueueHostJob([job = std::move(job)] { job(System::GetInstance()); });
}

// ---- Core/System.h ------------------------------------------------------------------------------
namespace
{
Memory::MemoryManager s_memory;
Fifo::FifoManager s_fifo;
PowerPC::PowerPCManager s_powerpc;
PowerPC::PowerPCState s_ppc_state;
}  // namespace

System& System::GetInstance()
{
  static System s;
  return s;
}
Memory::MemoryManager& System::GetMemory() const
{
  return s_memory;
}
Fifo::FifoManager& System::GetFifo() const
{
  return s_fifo;
}
PowerPC::PowerPCManager& System::GetPowerPC() const
{
  return s_powerpc;
}
PowerPC::PowerPCState& System::GetPPCState() const
{
  return s_ppc_state;
}
bool System::IsDualCoreMode() const
{
  return BrawlOnline::GetHost().DualCore();
}
}  // namespace Core

// ---- Core/HW/Memmap.h ---------------------------------------------------------------------------
namespace Memory
{
u32 MemoryManager::GetRamSizeReal() const
{
  return BrawlOnline::GetHost().Mem1Size();
}
u32 MemoryManager::GetExRamSizeReal() const
{
  return BrawlOnline::GetHost().Mem2Size();
}
u8* MemoryManager::GetRAM() const
{
  return BrawlOnline::GetHost().Mem1();
}
u8* MemoryManager::GetEXRAM() const
{
  return BrawlOnline::GetHost().Mem2();
}

std::span<u8> MemoryManager::GetSpanForAddress(u32 address) const
{
  auto& host = BrawlOnline::GetHost();
  const u32 phys = address & 0x1FFFFFFF;
  if (phys < host.Mem1Size())
    return {host.Mem1() + phys, host.Mem1Size() - phys};
  if (phys >= 0x10000000 && phys - 0x10000000 < host.Mem2Size())
    return {host.Mem2() + (phys - 0x10000000), host.Mem2Size() - (phys - 0x10000000)};
  return {};
}

u8* MemoryManager::GetPointerForRange(u32 address, size_t size) const
{
  return BrawlOnline::GetHost().GuestPointer(address, static_cast<u32>(size));
}

void MemoryManager::CopyFromEmu(void* data, u32 address, size_t size) const
{
  if (const u8* p = GetPointerForRange(address, size))
    std::memcpy(data, p, size);
  else
    std::memset(data, 0, size);
}

void MemoryManager::CopyToEmu(u32 address, const void* data, size_t size)
{
  if (u8* p = GetPointerForRange(address, size))
  {
    std::memcpy(p, data, size);
    BrawlOnline::GetHost().MarkDirty(address, static_cast<u32>(size));
  }
}

u8 MemoryManager::Read_U8(u32 address) const
{
  const u8* p = GetPointerForRange(address, 1);
  return p ? *p : 0;
}
u16 MemoryManager::Read_U16(u32 address) const
{
  u16 v = 0;
  CopyFromEmu(&v, address, 2);
  return Common::swap16(v);
}
u32 MemoryManager::Read_U32(u32 address) const
{
  u32 v = 0;
  CopyFromEmu(&v, address, 4);
  return Common::swap32(v);
}
void MemoryManager::Write_U8(u8 var, u32 address)
{
  CopyToEmu(address, &var, 1);
}
void MemoryManager::Write_U16(u16 var, u32 address)
{
  const u16 v = Common::swap16(var);
  CopyToEmu(address, &v, 2);
}
void MemoryManager::Write_U32(u32 var, u32 address)
{
  const u32 v = Common::swap32(var);
  CopyToEmu(address, &v, 4);
}
}  // namespace Memory

// ---- VideoCommon ----------------------------------------------------------------------------------
// The host's picture (Host::DrawAspectRatio, EmissaryRecomp's DisplaySettings::PictureAspect): GameBridge's PutAspect
// tells the plugin the shape the picture is shown at through it, and the plugin draws for that shape (docs/game-code.md,
// "Any screen shape"). It was none, so the plugin was always told 0 and drew Brawl's own 16:9, squeezed or stretched
// into a 4:3 or a 21:9 picture (EmissaryRecomp's F10 aspect and Fill Window), menus and matches alike.
static VideoCommon::Presenter s_presenter;
VideoCommon::Presenter* g_presenter = &s_presenter;

// With the UI (core/ui) the messages are its own; else they go to the host.
#ifdef BRAWL_ONLINE_NO_UI
namespace OSD
{
void AddMessage(std::string message, u32 ms, u32 argb, const void*)
{
  if (BrawlOnline::HasHost())
    BrawlOnline::GetHost().OnScreenMessage(message, ms, argb);
}
void AddTypedMessage(MessageType, std::string message, u32 ms, u32 argb, const void* icon)
{
  AddMessage(std::move(message), ms, argb, icon);
}
}  // namespace OSD
#endif

// ---- Config keys of Dolphin's the core reads (same names and defaults) ----------------------------
namespace Config
{
const Info<bool> GFX_SHOW_NETPLAY_PING{{System::GFX, "Settings", "ShowNetPlayPing"}, true};
const Info<StereoMode> GFX_STEREO_MODE{{System::GFX, "Stereoscopy", "StereoMode"}, StereoMode::Off};
const Info<bool> GFX_HACK_IMMEDIATE_XFB{{System::GFX, "Hacks", "ImmediateXFBEnable"}, true};
const Info<std::string> MAIN_GPU_DETERMINISM_MODE{{System::Main, "Core", "GPUDeterminismMode"},
                                                  "auto"};
}  // namespace Config

// ---- Dolphin-only diagnostics the core calls --------------------------------------------------
namespace Gprb
{
// Dolphin's guest CPU trace at the loop top (Core/Rollback/GprbCpuTrace.cpp).
void CpuTraceOnLoopTop(Core::System&)
{
}
void CpuTraceOnFrameEnd(Core::System&)
{
}
}  // namespace Gprb
