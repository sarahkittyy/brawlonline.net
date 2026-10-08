// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <string>

#include "Common/CommonTypes.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HLE/HLE_Misc.h"
#include "Core/HLE/HLE_OS.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace HLE
{
// Map addresses to the HLE hook index
static std::map<u32, u32> s_hooked_addresses;

// clang-format off
constexpr std::array<Hook, 46> os_patches{{
    // Placeholder, os_patches[0] is the "non-existent function" index
    {"FAKE_TO_SKIP_0",               HLE_Misc::UnimplementedFunction,       HookType::Replace, HookFlag::Generic},

    // Name doesn't matter, installed in CBoot::BootUp()
    {"HBReload",                     HLE_Misc::HBReload,                    HookType::Replace, HookFlag::Fixed},

    // Debug/OS Support
    {"OSPanic",                      HLE_OS::HLE_OSPanic,                   HookType::Replace, HookFlag::Debug},

    // This needs to be put before vprintf (because vprintf is called indirectly by this)
    {"JUTWarningConsole_f",          HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},

    {"OSReport",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"DEBUGPrint",                   HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"WUD_DEBUGPrint",               HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"__DSP_debug_printf",           HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"vprintf",                      HLE_OS::HLE_GeneralDebugVPrint,        HookType::Start,   HookFlag::Debug},
    {"printf",                       HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"vdprintf",                     HLE_OS::HLE_LogVDPrint,                HookType::Start,   HookFlag::Debug},
    {"dprintf",                      HLE_OS::HLE_LogDPrint,                 HookType::Start,   HookFlag::Debug},
    {"vfprintf",                     HLE_OS::HLE_LogVFPrint,                HookType::Start,   HookFlag::Debug},
    {"fprintf",                      HLE_OS::HLE_LogFPrint,                 HookType::Start,   HookFlag::Debug},
    {"nlPrintf",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"DWC_Printf",                   HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"RANK_Printf",                  HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"puts",                         HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug}, // gcc-optimized printf?
    {"___blank",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug}, // used for early init things (normally)
    {"__write_console",              HLE_OS::HLE_write_console,             HookType::Start,   HookFlag::Debug}, // used by sysmenu (+more?)

    {"GeckoCodehandler",             HLE_Misc::GeckoCodeHandlerICacheFlush, HookType::Start,   HookFlag::Fixed},
    {"GeckoHandlerReturnTrampoline", HLE_Misc::GeckoReturnTrampoline,       HookType::Replace, HookFlag::Fixed},
    {"AppLoaderReport",              HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Fixed}, // apploader needs OSReport-like function
    {"BrawlbackGekkoNetUnconditionalFrame", HLE_Misc::BrawlbackGekkoNetUnconditionalFrame, HookType::Replace, HookFlag::Fixed},
    {"BrawlbackGekkoNetFrameEnd",    HLE_Misc::BrawlbackGekkoNetFrameEnd,   HookType::Replace, HookFlag::Fixed},
    {"BrawlbackGekkoNetLoopEnd",     HLE_Misc::BrawlbackGekkoNetLoopEnd,    HookType::Replace, HookFlag::Fixed},
    {"BrawlbackDVDCancelSleepHook",  HLE_Misc::BrawlbackDVDCancelSleepHook, HookType::Replace, HookFlag::Fixed},
    {"BrawlbackCancelTaskSleepHook", HLE_Misc::BrawlbackCancelTaskSleepHook, HookType::Replace, HookFlag::Fixed},
    {"BrawlbackDVDReadPrioSleepHook", HLE_Misc::BrawlbackDVDReadPrioSleepHook, HookType::Replace, HookFlag::Fixed},
    {"BrawlbackVIWaitForRetraceSleepHook", HLE_Misc::BrawlbackVIWaitForRetraceSleepHook, HookType::Replace, HookFlag::Fixed},
    {"BrawlbackSkipResimPadThreadReadHook", HLE_Misc::BrawlbackSkipResimPadThreadReadHook, HookType::Replace, HookFlag::Fixed},
    {"BrawlbackCapturePadThreadReadHook", HLE_Misc::BrawlbackCapturePadThreadReadHook, HookType::Start, HookFlag::Fixed},
    {"GprbPadThreadLoopHook", HLE_Misc::GprbPadThreadLoopHook, HookType::Start, HookFlag::Fixed},
    {"GprbPacingElapsedHook", HLE_Misc::GprbPacingElapsedHook, HookType::Start, HookFlag::Fixed},
    {"GprbPacingStepsHook", HLE_Misc::GprbPacingStepsHook, HookType::Start, HookFlag::Fixed},
    {"GprbRngTraceHook", HLE_Misc::GprbRngTraceHook, HookType::Start, HookFlag::Fixed},
    {"GprbProbeHook", HLE_Misc::GprbProbeHook, HookType::Start, HookFlag::Fixed},
    {"BrawlbackSkipResimWaveSoundAllocHook", HLE_Misc::BrawlbackSkipResimWaveSoundAllocHook, HookType::Replace, HookFlag::Fixed},
    {"BrawlbackSkipResimSeqSoundAllocHook", HLE_Misc::BrawlbackSkipResimSeqSoundAllocHook, HookType::Replace, HookFlag::Fixed},
    {"BrawlbackSkipResimStrmSoundAllocHook", HLE_Misc::BrawlbackSkipResimStrmSoundAllocHook, HookType::Replace, HookFlag::Fixed},
    {"BrawlbackSyncCharSelectRandomSeedHook", HLE_Misc::BrawlbackSyncCharSelectRandomSeedHook, HookType::Start, HookFlag::Fixed},
    {"BrawlbackSkipResimRenderHook", HLE_Misc::BrawlbackSkipResimRenderHook, HookType::Replace, HookFlag::Fixed},
    {"BrawlbackGXDrawDoneSleepHook", HLE_Misc::BrawlbackGXDrawDoneSleepHook, HookType::Replace, HookFlag::Fixed},
    {"BrawlbackFrameBufferSyncWaitHook", HLE_Misc::BrawlbackFrameBufferSyncWaitHook, HookType::Replace, HookFlag::Fixed},
    {"GprbSoundAttachHook", HLE_Misc::GprbSoundAttachHook, HookType::Start, HookFlag::Fixed},
    {"GprbStageCreateHook", HLE_Misc::GprbStageCreateHook, HookType::Start, HookFlag::Fixed}
  }};
// clang-format on

void Patch(Core::System& system, u32 addr, std::string_view func_name)
{
  auto& ppc_state = system.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();
  for (u32 i = 1; i < os_patches.size(); ++i)
  {
    if (os_patches[i].name == func_name)
    {
      s_hooked_addresses[addr] = i;
      ppc_state.iCache.Invalidate(memory, jit_interface, addr);
      Host_JitCacheInvalidation();
      return;
    }
  }
}

void PatchFixedFunctions(Core::System& system)
{
  // MIOS puts patch data in low MEM1 (0x1800-0x3000) for its own use.
  // Overwriting data in this range can cause the IPL to crash when launching games
  // that get patched by MIOS. See https://bugs.dolphin-emu.org/issues/11952 for more info.
  // Not applying the Gecko HLE patches means that Gecko codes will not work under MIOS,
  // but this is better than the alternative of having specific games crash.
  if (system.IsMIOS())
    return;

  // HLE jump to loader (homebrew).  Disabled when Gecko is active as it interferes with the code
  // handler
  if (!Config::AreCheatsEnabled())
  {
    Patch(system, 0x80001800, "HBReload");
    auto& memory = system.GetMemory();
    memory.CopyToEmu(0x00001804, "STUBHAXX", 8);
  }

  // Not part of the binary itself, but either we or Gecko OS might insert
  // this, and it doesn't clear the icache properly.
  Patch(system, Gecko::ENTRY_POINT, "GeckoCodehandler");
  // This has to always be installed even if cheats are not enabled because of the possibility of
  // loading a savestate where PC is inside the code handler while cheats are disabled.
  Patch(system, Gecko::HLE_TRAMPOLINE_ADDRESS, "GeckoHandlerReturnTrampoline");

  // Brawlback frame handling hooks
  Patch(system, 0x800171b4, "BrawlbackGekkoNetUnconditionalFrame"); // BRAWL_UNCONDITIONAL_HOOK_ADDR
  Patch(system, 0x80017504, "BrawlbackGekkoNetFrameEnd");    // BRAWL_GAME_LOOP_HOOK_ADDR - loop entry (inside conditionals)
  Patch(system, 0x80017508, "BrawlbackGekkoNetLoopEnd");     // BRAWL_LOOP_END_ADDR - controls outer game loop iteration (b 0x800171b4)
  Patch(system, 0x801fb1a8, "BrawlbackDVDCancelSleepHook");  // `bl OSSleepThread` inside DVDCancel's wait loop (dvd.o)
  Patch(system, 0x801cff38, "BrawlbackCancelTaskSleepHook"); // `bl OSSleepThread` inside TaskManager::CancelTask's wait loop
  Patch(system, 0x801f68ec, "BrawlbackDVDReadPrioSleepHook"); // instruction before `bl OSSleepThread` inside DVDReadPrio's wait loop
  Patch(system, 0x801e894c, "BrawlbackVIWaitForRetraceSleepHook"); // `bl OSSleepThread` inside VIWaitForRetrace's wait loop
  Patch(system, 0x80029464, "BrawlbackSkipResimPadThreadReadHook"); // `bl updateLowGC` inside updateLow (gf_pad.o)
  Patch(system, 0x80029468, "BrawlbackCapturePadThreadReadHook"); // return from updateLowGC inside updateLow (gf_pad.o)
  Patch(system, 0x8002ba84, "GprbPadThreadLoopHook"); // pad thread loop, after updateLow and P+'s post-updateLow code
  Patch(system, 0x800172d0, "GprbPacingElapsedHook"); // mainLoop: before consumeFrameCounter(r20 = VI fields elapsed)
  Patch(system, 0x8001730c, "GprbPacingStepsHook");   // mainLoop: before the logic-step accumulator update
  if (std::getenv("PPR_GPRB_RNG_LOG"))
    Patch(system, 0x8003fac4, "GprbRngTraceHook");    // mtRand::generate entry (diagnostics)
  // PPR_GPRB_PROBE=addr[,addr...]: log registers whenever the CPU reaches these addresses during a
  // gameplay session (diagnostics; also works for REL code once it is loaded there).
  if (const char* probes = std::getenv("PPR_GPRB_PROBE"))
  {
    std::string list(probes);
    size_t pos = 0;
    while (pos < list.size())
    {
      const size_t end = std::min(list.find(',', pos), list.size());
      const u32 addr = static_cast<u32>(std::strtoul(list.substr(pos, end - pos).c_str(), nullptr, 0));
      if (addr)
        Patch(system, addr, "GprbProbeHook");
      pos = end + 1;
    }
  }
  Patch(system, 0x801c9bfc, "BrawlbackSkipResimWaveSoundAllocHook"); // `bl detail_AllocWaveSound` in SoundArchivePlayer::detail_SetupSound
  Patch(system, 0x801c9aac, "BrawlbackSkipResimSeqSoundAllocHook"); // `bl detail_AllocSeqSound` in SoundArchivePlayer::detail_SetupSound
  Patch(system, 0x801c9b54, "BrawlbackSkipResimStrmSoundAllocHook"); // `bl detail_AllocStrmSound` in SoundArchivePlayer::detail_SetupSound
  Patch(system, 0x801c9c70, "GprbSoundAttachHook"); // detail_SetupSound success path: handle r28 gets sound r24
  Patch(system, 0x809435f0, "GprbStageCreateHook"); // __ct__7stMelee (sora_melee .text+0x238BDC): match start before the stage is built
  Patch(system, 0x8003fb4c, "BrawlbackSyncCharSelectRandomSeedHook"); // srandi__mt_prng_o_ - intercept all global mtRand PRNG seed initialization
  Patch(system, 0x803f8c5c, "BrawlbackSyncCharSelectRandomSeedHook"); // srand__rand_o_ - intercept all global C rand PRNG seed initialization
  Patch(system, 0x80017404, "BrawlbackSkipResimRenderHook"); // render dispatch branch in gfApplication::mainLoopSub
  Patch(system, 0x801f0ac0, "BrawlbackGXDrawDoneSleepHook"); // `bl OSSleepThread` inside GXDrawDone's wait loop (GXMisc.o)
  Patch(system, 0x80023b1c, "BrawlbackFrameBufferSyncWaitHook"); // `bl VIWaitForRetrace` inside gfFrameBuffer::sync's busy-wait loop
}

void PatchFunctions(Core::System& system)
{
  auto& power_pc = system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();
  auto& ppc_symbol_db = power_pc.GetSymbolDB();

  // Remove all hooks that aren't fixed address hooks
  for (auto i = s_hooked_addresses.begin(); i != s_hooked_addresses.end();)
  {
    if (os_patches[i->second].flags != HookFlag::Fixed)
    {
      ppc_state.iCache.Invalidate(memory, jit_interface, i->first);
      i = s_hooked_addresses.erase(i);
    }
    else
    {
      ++i;
    }
  }

  for (u32 i = 1; i < os_patches.size(); ++i)
  {
    // Fixed hooks don't map to symbols
    if (os_patches[i].flags == HookFlag::Fixed)
      continue;

    for (const auto& symbol : ppc_symbol_db.GetSymbolsFromName(os_patches[i].name))
    {
      for (u32 addr = symbol->address; addr < symbol->address + symbol->size; addr += 4)
      {
        s_hooked_addresses[addr] = i;
        ppc_state.iCache.Invalidate(memory, jit_interface, addr);
      }
      INFO_LOG_FMT(OSHLE, "Patching {} {:08x}", os_patches[i].name, symbol->address);
    }
  }

  Host_JitCacheInvalidation();
}

void Clear()
{
  s_hooked_addresses.clear();
}

void Reload(Core::System& system)
{
  Clear();
  PatchFixedFunctions(system);
  PatchFunctions(system);
}

void Execute(const Core::CPUThreadGuard& guard, u32 current_pc, u32 hook_index)
{
  hook_index &= 0xFFFFF;
  if (hook_index > 0 && hook_index < os_patches.size())
  {
    os_patches[hook_index].function(guard);
  }
  else
  {
    PanicAlertFmt("HLE system tried to call an undefined HLE function {}.", hook_index);
  }
}

void ExecuteFromJIT(u32 current_pc, u32 hook_index, Core::System& system)
{
  ASSERT(Core::IsCPUThread());
  Core::CPUThreadGuard guard(system);
  Execute(guard, current_pc, hook_index);
}

u32 GetHookByAddress(u32 address)
{
  auto iter = s_hooked_addresses.find(address);
  return (iter != s_hooked_addresses.end()) ? iter->second : 0;
}

u32 GetHookByFunctionAddress(PPCSymbolDB& ppc_symbol_db, u32 address)
{
  const u32 index = GetHookByAddress(address);
  // Fixed hooks use a fixed address and don't patch the whole function
  if (index == 0 || os_patches[index].flags == HookFlag::Fixed)
    return index;

  const Common::Symbol* const symbol = ppc_symbol_db.GetSymbolFromAddr(address);
  return (symbol && symbol->address == address) ? index : 0;
}

const char* GetHookNameByIndex(u32 index)
{
  return os_patches[index].name;
}

HookType GetHookTypeByIndex(u32 index)
{
  return os_patches[index].type;
}

HookFlag GetHookFlagsByIndex(u32 index)
{
  return os_patches[index].flags;
}

TryReplaceFunctionResult TryReplaceFunction(PPCSymbolDB& ppc_symbol_db, u32 address,
                                            PowerPC::CoreMode mode)
{
  const u32 hook_index = GetHookByFunctionAddress(ppc_symbol_db, address);
  if (hook_index == 0)
    return {};

  const HookType type = GetHookTypeByIndex(hook_index);
  if (type != HookType::Start && type != HookType::Replace)
    return {};

  const HookFlag flags = GetHookFlagsByIndex(hook_index);
  if (!IsEnabled(flags, mode))
    return {};

  return {type, hook_index};
}

bool IsEnabled(HookFlag flag, PowerPC::CoreMode mode)
{
  return flag != HLE::HookFlag::Debug || Config::IsDebuggingEnabled() ||
         mode == PowerPC::CoreMode::Interpreter;
}

u32 UnPatch(Core::System& system, std::string_view patch_name)
{
  const auto patch = std::ranges::find(os_patches, patch_name, &Hook::name);
  if (patch == std::end(os_patches))
    return 0;

  auto& power_pc = system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();

  if (patch->flags == HookFlag::Fixed)
  {
    const u32 patch_idx = static_cast<u32>(std::distance(os_patches.begin(), patch));
    u32 addr = 0;
    // Reverse search by OSPatch key instead of address
    for (auto i = s_hooked_addresses.begin(); i != s_hooked_addresses.end();)
    {
      if (i->second == patch_idx)
      {
        addr = i->first;
        ppc_state.iCache.Invalidate(memory, jit_interface, i->first);
        i = s_hooked_addresses.erase(i);
      }
      else
      {
        ++i;
      }
    }
    Host_JitCacheInvalidation();
    return addr;
  }

  const auto symbols = power_pc.GetSymbolDB().GetSymbolsFromName(patch_name);
  if (!symbols.empty())
  {
    const Common::Symbol* const symbol = symbols.front();
    for (u32 addr = symbol->address; addr < symbol->address + symbol->size; addr += 4)
    {
      s_hooked_addresses.erase(addr);
      ppc_state.iCache.Invalidate(memory, jit_interface, addr);
    }
    Host_JitCacheInvalidation();
    return symbol->address;
  }

  return 0;
}

u32 UnpatchRange(Core::System& system, u32 start_addr, u32 end_addr)
{
  auto& ppc_state = system.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();

  u32 count = 0;

  auto i = s_hooked_addresses.lower_bound(start_addr);
  while (i != s_hooked_addresses.end() && i->first < end_addr)
  {
    INFO_LOG_FMT(OSHLE, "Unpatch HLE hooks [{:08x};{:08x}): {} at {:08x}", start_addr, end_addr,
                 os_patches[i->second].name, i->first);
    ppc_state.iCache.Invalidate(memory, jit_interface, i->first);
    i = s_hooked_addresses.erase(i);
    count += 1;
  }
  Host_JitCacheInvalidation();

  return count;
}
}  // namespace HLE
