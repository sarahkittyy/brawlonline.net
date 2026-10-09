// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// TCP server and command dispatch for the automation harness. See docs/harness-protocol.md.

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <fmt/format.h>
#include <picojson.h>
#include <xxhash.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/GekkoDisassembler.h"
#include "Common/Swap.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/CoreTiming.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/Harness/Harness.h"
#include "Core/Harness/HarnessInternal.h"
#include "Core/NetPlayClient.h"
#include "Core/Online/GameBridge.h"
#include "Core/Online/Matchmaking.h"
#include "Core/Online/OnlineClient.h"
#include "Core/Online/OnlineSession.h"
#include "Core/Online/RecentCodes.h"
#include "Core/Online/User.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/Rollback/GameplayRollback.h"
#include "Core/Rollback/GameplaySession.h"
#include "Core/Rollback/GameplayOnlineBackend.h"
#include "Core/Rollback/PresentStats.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FrameDumper.h"

namespace Harness::Internal
{
namespace
{
#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
void CloseSocket(socket_t s)
{
  closesocket(s);
}
#else
using socket_t = int;
constexpr socket_t INVALID_SOCK = -1;
void CloseSocket(socket_t s)
{
  close(s);
}
#endif

constexpr u64 MAX_READ_LEN = 16 * 1024 * 1024;

// Don't let a client that disconnects mid-response kill the process with SIGPIPE.
#if defined(MSG_NOSIGNAL)
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;
#endif

std::atomic<bool> s_server_running{false};
socket_t s_listen_socket = INVALID_SOCK;
std::thread s_accept_thread;

std::mutex s_client_mutex;
std::thread s_client_thread;
std::atomic<bool> s_client_busy{false};
socket_t s_client_socket = INVALID_SOCK;

// Serializes command execution (only one client, but keep it explicit).
std::mutex s_command_mutex;

bool SendAll(socket_t sock, std::string_view data)
{
  while (!data.empty())
  {
    const int sent = static_cast<int>(send(sock, data.data(), static_cast<int>(data.size()), SEND_FLAGS));
    if (sent <= 0)
      return false;
    data.remove_prefix(static_cast<size_t>(sent));
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Request helpers

struct CommandError
{
  std::string message;
};

using Args = picojson::object;
using Result = picojson::object;

[[noreturn]] void Fail(std::string message)
{
  throw CommandError{std::move(message)};
}

const picojson::value* Find(const Args& args, const std::string& key)
{
  const auto it = args.find(key);
  if (it == args.end() || it->second.is<picojson::null>())
    return nullptr;
  return &it->second;
}

std::optional<u64> ParseU64Value(const picojson::value& v)
{
  if (v.is<double>())
  {
    const double d = v.get<double>();
    if (d < 0 || d != std::floor(d) || d > 18446744073709551615.0)
      return std::nullopt;
    return static_cast<u64>(d);
  }
  if (v.is<std::string>())
  {
    u64 value = 0;
    std::string str = v.get<std::string>();
    if (TryParse(str, &value))  // handles 0x prefixes
      return value;
  }
  return std::nullopt;
}

u64 GetU64(const Args& args, const std::string& key, std::optional<u64> default_value = {})
{
  const picojson::value* v = Find(args, key);
  if (!v)
  {
    if (default_value)
      return *default_value;
    Fail(fmt::format("missing argument '{}'", key));
  }
  const auto parsed = ParseU64Value(*v);
  if (!parsed)
    Fail(fmt::format("argument '{}' must be a non-negative integer", key));
  return *parsed;
}

std::string GetString(const Args& args, const std::string& key,
                      std::optional<std::string> default_value = {})
{
  const picojson::value* v = Find(args, key);
  if (!v)
  {
    if (default_value)
      return *default_value;
    Fail(fmt::format("missing argument '{}'", key));
  }
  if (!v->is<std::string>())
    Fail(fmt::format("argument '{}' must be a string", key));
  return v->get<std::string>();
}

bool GetBool(const Args& args, const std::string& key, bool default_value)
{
  const picojson::value* v = Find(args, key);
  if (!v)
    return default_value;
  if (!v->is<bool>())
    Fail(fmt::format("argument '{}' must be a boolean", key));
  return v->get<bool>();
}

picojson::value Num(u64 value)
{
  return picojson::value(static_cast<double>(value));
}

std::string ToHex(const u8* data, size_t len)
{
  static constexpr char digits[] = "0123456789abcdef";
  std::string out(len * 2, '0');
  for (size_t i = 0; i < len; ++i)
  {
    out[2 * i] = digits[data[i] >> 4];
    out[2 * i + 1] = digits[data[i] & 0xf];
  }
  return out;
}

std::vector<u8> FromHex(const std::string& hex)
{
  if (hex.size() % 2 != 0)
    Fail("hex string must have an even number of digits");
  std::vector<u8> out(hex.size() / 2);
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < out.size(); ++i)
  {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0)
      Fail("invalid hex digit");
    out[i] = static_cast<u8>((hi << 4) | lo);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Emulator helpers

Core::System& Sys()
{
  return Core::System::GetInstance();
}

const char* StateName(Core::State state)
{
  switch (state)
  {
  case Core::State::Uninitialized:
    return "uninitialized";
  case Core::State::Paused:
    return "paused";
  case Core::State::Running:
    return "running";
  case Core::State::Stopping:
    return "stopping";
  case Core::State::Starting:
    return "starting";
  }
  return "unknown";
}

void RequireRunning()
{
  if (!Core::IsRunning(Sys()))
    Fail("emulation is not running");
}

// Translates an effective MEM1/MEM2 address range into a physical address. Fails if the range is
// not entirely inside MEM1 (0x80000000/0xC0000000 mirrors) or MEM2 (0x90000000/0xD0000000).
u32 TranslateRange(u64 addr, u64 len)
{
  auto& memory = Sys().GetMemory();
  if (len == 0)
    Fail("length must be > 0");
  if (addr > 0xFFFFFFFFull || addr + len - 1 > 0xFFFFFFFFull)
    Fail("address out of range");

  const u32 a = static_cast<u32>(addr);
  const u32 segment = a >> 28;
  u64 offset;
  u64 size;
  u32 phys_base;
  if (segment == 0x8 || segment == 0xC)
  {
    offset = a & 0x0FFFFFFF;
    size = memory.GetRamSizeReal();
    phys_base = 0;
  }
  else if (segment == 0x9 || segment == 0xD)
  {
    offset = a & 0x0FFFFFFF;
    size = memory.GetExRamSizeReal();
    phys_base = 0x10000000;
  }
  else
  {
    Fail(fmt::format("address {:#010x} is not in MEM1 or MEM2", a));
  }

  if (offset + len > size)
    Fail(fmt::format("range {:#010x}+{:#x} is outside the mapped memory region", a, len));
  return phys_base + static_cast<u32>(offset);
}

// ---------------------------------------------------------------------------------------------
// Pads

GCPadStatus ParsePad(const Args& args)
{
  GCPadStatus pad{};
  pad.isConnected = true;

  if (const picojson::value* buttons = Find(args, "buttons"))
  {
    if (!buttons->is<picojson::array>())
      Fail("'buttons' must be a list");
    for (const auto& b : buttons->get<picojson::array>())
    {
      if (!b.is<std::string>())
        Fail("button names must be strings");
      std::string name = b.get<std::string>();
      Common::ToUpper(&name);
      static const std::array<std::pair<const char*, u16>, 12> table{{
          {"A", PAD_BUTTON_A},
          {"B", PAD_BUTTON_B},
          {"X", PAD_BUTTON_X},
          {"Y", PAD_BUTTON_Y},
          {"Z", PAD_TRIGGER_Z},
          {"L", PAD_TRIGGER_L},
          {"R", PAD_TRIGGER_R},
          {"START", PAD_BUTTON_START},
          {"DUP", PAD_BUTTON_UP},
          {"DDOWN", PAD_BUTTON_DOWN},
          {"DLEFT", PAD_BUTTON_LEFT},
          {"DRIGHT", PAD_BUTTON_RIGHT},
      }};
      const auto it = std::ranges::find_if(table, [&](const auto& e) { return name == e.first; });
      if (it == table.end())
        Fail(fmt::format("unknown button '{}'", name));
      pad.button |= it->second;
    }
  }

  auto parse_xy = [&](const char* key, u8* x, u8* y) {
    const picojson::value* v = Find(args, key);
    if (!v)
      return;
    if (!v->is<picojson::array>() || v->get<picojson::array>().size() != 2)
      Fail(fmt::format("'{}' must be [x, y]", key));
    const auto& arr = v->get<picojson::array>();
    const auto px = ParseU64Value(arr[0]);
    const auto py = ParseU64Value(arr[1]);
    if (!px || !py || *px > 255 || *py > 255)
      Fail(fmt::format("'{}' values must be 0-255", key));
    *x = static_cast<u8>(*px);
    *y = static_cast<u8>(*py);
  };
  parse_xy("main", &pad.stickX, &pad.stickY);
  parse_xy("c", &pad.substickX, &pad.substickY);

  auto parse_trigger = [&](const char* key, u16 digital_bit) -> u8 {
    const u64 def = (pad.button & digital_bit) ? 255 : 0;
    const u64 value = GetU64(args, key, def);
    if (value > 255)
      Fail(fmt::format("'{}' must be 0-255", key));
    return static_cast<u8>(value);
  };
  pad.triggerLeft = parse_trigger("l", PAD_TRIGGER_L);
  pad.triggerRight = parse_trigger("r", PAD_TRIGGER_R);
  pad.analogA = (pad.button & PAD_BUTTON_A) ? 0xFF : 0;
  pad.analogB = (pad.button & PAD_BUTTON_B) ? 0xFF : 0;
  return pad;
}

int GetPadPort(const Args& args)
{
  const u64 port = GetU64(args, "port");
  if (port > 3)
    Fail("port must be 0-3");

  // In a harness netplay session the local controller port does not correspond 1:1 to an SI
  // channel (the netplay pad map decides), so only check the local configuration otherwise.
  if (!NetPlaySession::IsActive() && !NetPlay::IsNetPlayRunning())
  {
    const auto device = Config::Get(Config::GetInfoForSIDevice(static_cast<int>(port)));
    if (device == SerialInterface::SIDEVICE_NONE)
      Fail(fmt::format("port {} has no SI device configured", port));
  }
  return static_cast<int>(port);
}

// ---------------------------------------------------------------------------------------------
// PNG header

std::optional<std::pair<u32, u32>> ReadPngSize(const std::string& path)
{
  File::IOFile file(path, "rb");
  if (!file)
    return std::nullopt;
  std::array<u8, 24> header{};
  if (!file.ReadBytes(header.data(), header.size()))
    return std::nullopt;
  static constexpr std::array<u8, 8> sig{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if (!std::equal(sig.begin(), sig.end(), header.begin()))
    return std::nullopt;
  auto be32 = [&](size_t off) {
    return (u32(header[off]) << 24) | (u32(header[off + 1]) << 16) | (u32(header[off + 2]) << 8) |
           u32(header[off + 3]);
  };
  return std::make_pair(be32(16), be32(20));
}

// ---------------------------------------------------------------------------------------------
// Commands

std::atomic<bool> s_quit_requested{false};

Result CmdPing(const Args&)
{
  Result r;
  r["pong"] = picojson::value(true);
  r["version"] = Num(PROTOCOL_VERSION);
  return r;
}

Result CmdStatus(const Args&)
{
  auto& system = Sys();
  const Core::State state = Core::GetState(system);
  Result r;
  r["state"] = picojson::value(StateName(state));
  r["frame"] = Num(GetFrame());
  r["input_polls"] = Num(GetInputPolls());
  r["poll_source"] = picojson::value(GetPollSourceName());
  r["si_polls"] = Num(GetSIPolls());
  r["game_id"] = picojson::value(state == Core::State::Uninitialized ?
                                     std::string() :
                                     SConfig::GetInstance().GetGameID());
  r["cpu_thread"] = picojson::value(state == Core::State::Uninitialized ?
                                        Config::Get(Config::MAIN_CPU_THREAD) :
                                        system.IsDualCoreMode());
  r["video_backend"] = picojson::value(Config::Get(Config::MAIN_GFX_BACKEND));
  r["audio_muted"] = picojson::value(Config::Get(Config::MAIN_AUDIO_MUTED));
  {
    const auto ps = Rollback::PresentStats::Get();
    picojson::object p;
    p["present_resimulated"] = picojson::value(ps.present_resimulated);
    p["xfb_fields"] = Num(ps.xfb_fields);
    p["xfb_fields_skipped"] = Num(ps.xfb_fields_skipped);
    p["xfb_copies"] = Num(ps.xfb_copies);
    p["xfb_copies_skipped"] = Num(ps.xfb_copies_skipped);
    p["copy_decision_misses"] = Num(ps.copy_decision_misses);
    p["immediate_xfb"] = picojson::value(Config::Get(Config::GFX_HACK_IMMEDIATE_XFB));
    p["presents"] = Num(ps.presents);
    p["duplicate_presents"] = Num(ps.duplicate_presents);
    p["displayed_frames"] = Num(ps.displayed_frames);
    p["displayed_frames_after_resim"] = Num(ps.displayed_frames_after_resim);
    picojson::array all, after;
    for (int i = 0; i < Rollback::PresentStats::HISTOGRAM_SIZE; ++i)
    {
      all.emplace_back(Num(ps.outputs_per_frame[i]));
      after.emplace_back(Num(ps.outputs_per_frame_after_resim[i]));
    }
    p["outputs_per_frame"] = picojson::value(std::move(all));
    p["outputs_per_frame_after_resim"] = picojson::value(std::move(after));
    const auto gw = Rollback::PresentStats::GetGpuRamWrites();
    picojson::object g;
    g["efb_copies"] = Num(gw.efb_copies);
    g["efb_copies_deferred"] = Num(gw.efb_copies_deferred);
    g["efb_copy_flushes"] = Num(gw.efb_copy_flushes);
    g["xfb_copies"] = Num(gw.xfb_copies);
    g["fills"] = Num(gw.fills);
    g["bytes"] = Num(gw.bytes);
    g["readback_us_total"] = Num(gw.readback_us_total);
    g["readback_us_max"] = Num(gw.readback_us_max);
    p["gpu_ram_writes"] = picojson::value(std::move(g));
    r["presentation"] = picojson::value(std::move(p));
  }
  r["netplay"] = NetPlaySession::Status();
  return r;
}

Result CmdReadMem(const Args& args)
{
  const u64 addr = GetU64(args, "addr");
  const u64 len = GetU64(args, "len");
  if (len > MAX_READ_LEN)
    Fail("len must be <= 16 MiB");
  RequireRunning();
  std::vector<u8> buffer(len);
  {
    const Core::CPUThreadGuard guard(Sys());
    RequireRunning();
    const u32 phys = TranslateRange(addr, len);
    const u8* ptr = Sys().GetMemory().GetPointerForRange(phys, len);
    if (!ptr)
      Fail("memory not accessible");
    std::memcpy(buffer.data(), ptr, len);
  }
  Result r;
  r["hex"] = picojson::value(ToHex(buffer.data(), buffer.size()));
  return r;
}

Result CmdWriteMem(const Args& args)
{
  const u64 addr = GetU64(args, "addr");
  const std::vector<u8> data = FromHex(GetString(args, "hex"));
  if (data.empty())
    Fail("hex must not be empty");
  if (data.size() > MAX_READ_LEN)
    Fail("write must be <= 16 MiB");
  RequireRunning();
  {
    auto& system = Sys();
    const Core::CPUThreadGuard guard(system);
    RequireRunning();
    const u32 phys = TranslateRange(addr, data.size());
    system.GetMemory().CopyToEmu(phys, data.data(), data.size());
    system.GetJitInterface().InvalidateICache(static_cast<u32>(addr), static_cast<u32>(data.size()),
                                              true);
  }
  return {};
}

// Debug snapshot of the emulated CPU and interrupt state, for diagnosing game hangs.
Result CmdCpuState(const Args&)
{
  RequireRunning();
  Result r;
  const Core::CPUThreadGuard guard(Sys());
  RequireRunning();
  const auto& ppc = Sys().GetPPCState();
  r["pc"] = Num(ppc.pc);
  r["npc"] = Num(ppc.npc);
  r["msr"] = Num(ppc.msr.Hex);
  r["lr"] = Num(ppc.spr[SPR_LR]);
  r["ctr"] = Num(ppc.spr[SPR_CTR]);
  r["srr0"] = Num(ppc.spr[SPR_SRR0]);
  r["srr1"] = Num(ppc.spr[SPR_SRR1]);
  r["sp"] = Num(ppc.gpr[1]);
  r["exceptions"] = Num(ppc.Exceptions);
  r["pi_cause"] = Num(Sys().GetProcessorInterface().GetCause());
  r["pi_mask"] = Num(Sys().GetProcessorInterface().GetMask());
  picojson::array vi;
  for (u32 i = 0; i < 4; ++i)
    vi.emplace_back(Num(PowerPC::MMU::HostRead<u32>(guard, 0xCC002030 + 4 * i)));
  r["vi_display_interrupts"] = picojson::value(std::move(vi));
  r["current_thread"] = Num(PowerPC::MMU::HostRead<u32>(guard, 0x800000E4));
  picojson::array gpr;
  for (u32 v : ppc.gpr)
    gpr.emplace_back(Num(v));
  r["gpr"] = picojson::value(std::move(gpr));
  return r;
}

// The GPU-facing hardware state (debugging hangs on the draw-done/token interrupts): the CP FIFO
// as the command processor sees it, the PI's CPU FIFO, the PE interrupt control, and the
// processor interface's interrupt cause/mask.
Result CmdGpuState(const Args&)
{
  RequireRunning();
  Result r;
  const Core::CPUThreadGuard guard(Sys());
  RequireRunning();
  auto& fifo = Sys().GetCommandProcessor().GetFifo();
  r["cp_base"] = Num(fifo.CPBase.load());
  r["cp_end"] = Num(fifo.CPEnd.load());
  r["cp_hi_watermark"] = Num(fifo.CPHiWatermark);
  r["cp_lo_watermark"] = Num(fifo.CPLoWatermark);
  r["cp_rw_distance"] = Num(fifo.CPReadWriteDistance.load());
  r["cp_write_pointer"] = Num(fifo.CPWritePointer.load());
  r["cp_read_pointer"] = Num(fifo.CPReadPointer.load());
  r["cp_safe_read_pointer"] = Num(fifo.SafeCPReadPointer.load());
  r["cp_breakpoint"] = Num(fifo.CPBreakpoint.load());
  r["cp_gp_read_enable"] = Num(fifo.bFF_GPReadEnable.load());
  r["cp_gp_link_enable"] = Num(fifo.bFF_GPLinkEnable.load());
  r["cp_bp_enable"] = Num(fifo.bFF_BPEnable.load());
  r["cp_bp_int"] = Num(fifo.bFF_BPInt.load());
  r["cp_breakpoint_hit"] = Num(fifo.bFF_Breakpoint.load());
  r["cp_hi_watermark_int"] = Num(fifo.bFF_HiWatermarkInt.load());
  r["cp_lo_watermark_int"] = Num(fifo.bFF_LoWatermarkInt.load());
  r["cp_hi_watermark_hit"] = Num(fifo.bFF_HiWatermark.load());
  r["cp_lo_watermark_hit"] = Num(fifo.bFF_LoWatermark.load());
  auto& pi = Sys().GetProcessorInterface();
  r["pi_cause"] = Num(pi.GetCause());
  r["pi_mask"] = Num(pi.GetMask());
  // PI CPU FIFO: base, end, write pointer (MMIO 0xCC00300C/10/14).
  r["pi_fifo_base"] = Num(PowerPC::MMU::HostRead<u32>(guard, 0xCC00300C));
  r["pi_fifo_end"] = Num(PowerPC::MMU::HostRead<u32>(guard, 0xCC003010));
  r["pi_fifo_write_pointer"] = Num(PowerPC::MMU::HostRead<u32>(guard, 0xCC003014));
  // CP status/control, PE control and token (16-bit MMIO).
  r["cp_status"] = Num(PowerPC::MMU::HostRead<u16>(guard, 0xCC000000));
  r["cp_control"] = Num(PowerPC::MMU::HostRead<u16>(guard, 0xCC000002));
  r["pe_control"] = Num(PowerPC::MMU::HostRead<u16>(guard, 0xCC00100A));
  r["pe_token"] = Num(PowerPC::MMU::HostRead<u16>(guard, 0xCC00100E));
  r["deterministic_gpu_thread"] = picojson::value(Sys().GetFifo().UseDeterministicGPUThread());
  r["dual_core"] = picojson::value(Sys().IsDualCoreMode());
  r["ticks"] = Num(Sys().GetCoreTiming().GetTicks());
  return r;
}

Result CmdReadU32(const Args& args)
{
  const u64 addr = GetU64(args, "addr");
  RequireRunning();
  std::array<u8, 4> bytes{};
  {
    const Core::CPUThreadGuard guard(Sys());
    RequireRunning();
    const u32 phys = TranslateRange(addr, 4);
    Sys().GetMemory().CopyFromEmu(bytes.data(), phys, 4);
  }
  Result r;
  r["value"] = Num((u32(bytes[0]) << 24) | (u32(bytes[1]) << 16) | (u32(bytes[2]) << 8) | bytes[3]);
  return r;
}

Result CmdHashMem(const Args& args)
{
  const picojson::value* ranges_v = Find(args, "ranges");
  if (!ranges_v || !ranges_v->is<picojson::array>())
    Fail("'ranges' must be a list of [addr, len]");
  std::vector<std::pair<u64, u64>> ranges;
  for (const auto& rv : ranges_v->get<picojson::array>())
  {
    if (!rv.is<picojson::array>() || rv.get<picojson::array>().size() != 2)
      Fail("each range must be [addr, len]");
    const auto a = ParseU64Value(rv.get<picojson::array>()[0]);
    const auto l = ParseU64Value(rv.get<picojson::array>()[1]);
    if (!a || !l)
      Fail("range values must be non-negative integers");
    ranges.emplace_back(*a, *l);
  }
  RequireRunning();

  XXH3_state_t* xxh = XXH3_createState();
  XXH3_64bits_reset(xxh);
  try
  {
    const Core::CPUThreadGuard guard(Sys());
    RequireRunning();
    for (const auto& [addr, len] : ranges)
    {
      const u32 phys = TranslateRange(addr, len);
      const u8* ptr = Sys().GetMemory().GetPointerForRange(phys, len);
      if (!ptr)
        Fail("memory not accessible");
      XXH3_64bits_update(xxh, ptr, len);
    }
  }
  catch (...)
  {
    XXH3_freeState(xxh);
    throw;
  }
  const u64 hash = XXH3_64bits_digest(xxh);
  XXH3_freeState(xxh);

  Result r;
  r["xxh3_64"] = picojson::value(fmt::format("{:016x}", hash));
  return r;
}

Result CmdPadSet(const Args& args)
{
  const int port = GetPadPort(args);
  PadSet(port, ParsePad(args));
  return {};
}

Result CmdPadClear(const Args& args)
{
  const u64 port = GetU64(args, "port");
  if (port > 3)
    Fail("port must be 0-3");
  PadClear(static_cast<int>(port));
  return {};
}

Result CmdPadScript(const Args& args)
{
  const int port = GetPadPort(args);
  const picojson::value* frames_v = Find(args, "frames");
  if (!frames_v || !frames_v->is<picojson::array>())
    Fail("'frames' must be a list");

  std::vector<GCPadStatus> timeline;
  for (const auto& fv : frames_v->get<picojson::array>())
  {
    if (!fv.is<picojson::object>())
      Fail("each frame must be an object");
    const Args& frame = fv.get<picojson::object>();
    const GCPadStatus pad = ParsePad(frame);
    const u64 hold = GetU64(frame, "hold", 1);
    if (hold == 0)
      Fail("'hold' must be >= 1");
    if (timeline.size() + hold > 10'000'000)
      Fail("script too long");
    timeline.insert(timeline.end(), hold, pad);
  }

  std::optional<u64> start;
  if (const picojson::value* sv = Find(args, "start"))
  {
    if (sv->is<std::string>() && sv->get<std::string>() == "next")
      start = std::nullopt;
    else if (const auto parsed = ParseU64Value(*sv))
      start = *parsed;
    else
      Fail("'start' must be \"next\" or an absolute input_polls value");
  }

  ScriptTiming timing{};
  if (const auto error = PadScript(port, std::move(timeline), start, &timing))
    Fail(*error);
  Result r;
  r["starts_at"] = Num(timing.starts_at);
  r["ends_at"] = Num(timing.ends_at);
  return r;
}

Result CmdPadScriptStatus(const Args& args)
{
  const u64 port = GetU64(args, "port");
  if (port > 3)
    Fail("port must be 0-3");
  const ScriptStatus status = PadScriptStatus(static_cast<int>(port));
  Result r;
  r["active"] = picojson::value(status.active);
  r["remaining"] = Num(status.remaining);
  return r;
}

Result CmdWaitFrame(const Args& args)
{
  const bool has_frame = Find(args, "frame") != nullptr;
  const bool has_polls = Find(args, "input_polls") != nullptr;
  if (has_frame == has_polls)
    Fail("specify exactly one of 'frame' or 'input_polls'");
  const u64 timeout_ms = GetU64(args, "timeout_ms", 10000);
  const u64 target = GetU64(args, has_frame ? "frame" : "input_polls");

  const bool reached = WaitUntil(
      [&] {
        return s_quit_requested.load() || !s_server_running.load() ||
               (has_frame ? GetFrame() : GetInputPolls()) >= target;
      },
      std::chrono::milliseconds(timeout_ms));
  if (!reached || (has_frame ? GetFrame() : GetInputPolls()) < target)
    Fail("timeout");

  Result r;
  r["frame"] = Num(GetFrame());
  r["input_polls"] = Num(GetInputPolls());
  return r;
}

Result CmdPause(const Args&)
{
  auto& system = Sys();
  RequireRunning();
  CancelBreakAtFrame();
  Core::SetState(system, Core::State::Paused);
  if (!WaitUntil([&] { return Core::GetState(system) == Core::State::Paused; },
                 std::chrono::milliseconds(5000)))
  {
    Fail("failed to pause");
  }
  return {};
}

Result CmdResume(const Args&)
{
  auto& system = Sys();
  RequireRunning();
  CancelBreakAtFrame();
  Core::SetState(system, Core::State::Running);
  return {};
}

Result CmdFrameAdvance(const Args& args)
{
  auto& system = Sys();
  const u64 n = GetU64(args, "n", 1);
  if (n == 0)
    Fail("n must be >= 1");
  RequireRunning();
  if (Core::GetState(system) != Core::State::Paused)
    Fail("frame_advance requires the emulation to be paused");

  const u64 target = GetFrame() + n;
  RequestBreakAtFrame(target);
  Core::SetState(system, Core::State::Running, false);

  const auto timeout = std::chrono::milliseconds(10000 + n * 100);
  const bool done = WaitUntil(
      [&] {
        return !Core::IsRunning(system) ||
               (GetFrame() >= target && system.GetCPU().IsStepping());
      },
      timeout);
  if (!done || !Core::IsRunning(system))
  {
    CancelBreakAtFrame();
    Fail("timeout");
  }
  Core::NotifyStateChanged(Core::State::Paused);

  Result r;
  r["frame"] = Num(GetFrame());
  return r;
}

Result CmdScreenshot(const Args& args)
{
  auto& system = Sys();
  const std::string path = GetString(args, "path");
  std::string lower = path;
  Common::ToLower(&lower);
  if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".png")
    Fail("path must end in .png");
  const bool absolute =
#ifdef _WIN32
      path.size() > 2 && (path[1] == ':' || (path[0] == '\\' && path[1] == '\\'));
#else
      !path.empty() && path[0] == '/';
#endif
  if (!absolute)
    Fail("path must be absolute");

  if (Config::Get(Config::MAIN_GFX_BACKEND) == "Null")
    Fail("screenshots are not available with the Null video backend");
  RequireRunning();
  if (Core::GetState(system) == Core::State::Paused)
    Fail("screenshots require running emulation (resume first)");
  if (!g_frame_dumper)
    Fail("frame dumper not available");

  if (File::Exists(path))
    File::Delete(path);
  const std::string dir = path.substr(0, path.find_last_of("/\\") + 1);
  if (!dir.empty())
    File::CreateFullPath(dir);

  const u64 before = g_frame_dumper->GetCompletedScreenshotCount();
  g_frame_dumper->SaveScreenshot(path);
  const bool done =
      WaitUntil([&] { return g_frame_dumper->GetCompletedScreenshotCount() > before; },
                std::chrono::milliseconds(15000));
  if (!done)
    Fail("timeout waiting for the screenshot to be written");

  const auto size = ReadPngSize(path);
  if (!size)
    Fail("screenshot was not written");

  Result r;
  r["path"] = picojson::value(path);
  r["width"] = Num(size->first);
  r["height"] = Num(size->second);
  return r;
}

Result CmdSaveState(const Args& args)
{
  auto& system = Sys();
  const std::string path = GetString(args, "path");
  RequireRunning();
  if (File::Exists(path))
    File::Delete(path);
  {
    // Inside the guard this thread acts as the CPU thread, so the state is captured right away;
    // the compressed file is written by a worker thread which we wait for below.
    const Core::CPUThreadGuard guard(system);
    RequireRunning();
    State::SaveAs(system, path);
  }
  State::WaitForPendingSaves();
  if (!File::Exists(path))
    Fail("savestate was not written");
  return {};
}

Result CmdLoadState(const Args& args)
{
  auto& system = Sys();
  const std::string path = GetString(args, "path");
  if (!File::Exists(path))
    Fail("file does not exist");
  RequireRunning();
  if (NetPlay::IsNetPlayRunning())
    Fail("savestates cannot be loaded during netplay");
  {
    const Core::CPUThreadGuard guard(system);
    RequireRunning();
    State::LoadAs(system, path);
  }
  return {};
}

Result CmdNetPlayHost(const Args& args)
{
  const u64 port = GetU64(args, "port");
  if (port == 0 || port > 0xFFFF)
    Fail("invalid port");
  std::optional<int> delay;
  if (Find(args, "delay"))
    delay = static_cast<int>(GetU64(args, "delay"));
  if (const auto error = NetPlaySession::Host(static_cast<u16>(port), GetString(args, "game"),
                                              GetString(args, "name", "Harness Host"),
                                              GetBool(args, "rollback", false), delay))
  {
    Fail(*error);
  }
  return {};
}

Result CmdNetPlayJoin(const Args& args)
{
  const u64 port = GetU64(args, "port");
  if (port == 0 || port > 0xFFFF)
    Fail("invalid port");
  if (const auto error =
          NetPlaySession::Join(GetString(args, "host", "127.0.0.1"), static_cast<u16>(port),
                               GetString(args, "name", "Harness Client"),
                               GetString(args, "game", "")))
  {
    Fail(*error);
  }
  return {};
}

Result CmdNetPlayStart(const Args&)
{
  if (const auto error = NetPlaySession::Start())
    Fail(*error);
  return {};
}

Result CmdNetPlayStatus(const Args&)
{
  const picojson::value status = NetPlaySession::Status();
  if (status.is<picojson::object>())
    return status.get<picojson::object>();
  Result r;
  r["role"] = picojson::value();
  r["connected"] = picojson::value(false);
  r["players"] = picojson::value(picojson::array());
  r["game_running"] = picojson::value(false);
  r["rollback"] = picojson::value();
  return r;
}

Result CmdRollbackPadHistory(const Args& args)
{
  const picojson::value history =
      NetPlaySession::PadHistory(static_cast<s64>(GetU64(args, "since", 0)),
                                 GetBool(args, "raw", false));
  if (!history.is<picojson::object>())
    Fail("no netplay session");
  return history.get<picojson::object>();
}

// Per-snapshot timing samples (wall-clock save time, dual-core GPU sync time, pipelines compiled
// meanwhile) from sample index `since` on; `next` is the index to pass next time.
Result CmdRollbackTimings(const Args& args)
{
  std::vector<Rollback::RollbackManager::SaveSample> samples;
  const u64 next =
      Rollback::RollbackManager::Get().GetSaveSamples(GetU64(args, "since", 0), samples);
  picojson::array save_us, sync_us, compiles, evict_us, dostate_us, ram_us;
  for (const auto& s : samples)
  {
    save_us.emplace_back(static_cast<double>(s.save_us));
    sync_us.emplace_back(static_cast<double>(s.sync_us));
    compiles.emplace_back(static_cast<double>(s.compiles));
    evict_us.emplace_back(static_cast<double>(s.evict_us));
    dostate_us.emplace_back(static_cast<double>(s.dostate_us));
    ram_us.emplace_back(static_cast<double>(s.ram_us));
  }
  Result r;
  r["next"] = picojson::value(static_cast<double>(next));
  r["first"] = picojson::value(static_cast<double>(next - samples.size()));
  r["save_us"] = picojson::value(std::move(save_us));
  r["sync_us"] = picojson::value(std::move(sync_us));
  r["compiles"] = picojson::value(std::move(compiles));
  r["evict_us"] = picojson::value(std::move(evict_us));
  r["dostate_us"] = picojson::value(std::move(dostate_us));
  r["ram_us"] = picojson::value(std::move(ram_us));
  return r;
}

Result CmdRollbackChunkHashes(const Args& args)
{
  const picojson::value hashes =
      NetPlaySession::ChunkHashes(static_cast<s64>(GetU64(args, "frame")));
  if (!hashes.is<picojson::object>())
    Fail("no chunk hashes for that frame (set PPR_ROLLBACK_CHUNK_HASHES=1)");
  return hashes.get<picojson::object>();
}

Result CmdNetPlayLeave(const Args&)
{
  if (const auto error = NetPlaySession::Leave())
    Fail(*error);
  return {};
}

// ---------------------------------------------------------------------------------------------
// Gameplay-only rollback tooling (Core/Rollback/GameplayRollback.h)

std::vector<std::pair<u32, u32>> ParseRanges(const Args& args, const char* key)
{
  std::vector<std::pair<u32, u32>> out;
  const picojson::value* v = Find(args, key);
  if (!v)
    return out;
  if (!v->is<picojson::array>())
    Fail(fmt::format("'{}' must be a list of [addr, len]", key));
  for (const auto& rv : v->get<picojson::array>())
  {
    if (!rv.is<picojson::array>() || rv.get<picojson::array>().size() < 2)
      Fail("each range must be [addr, len]");
    const auto a = ParseU64Value(rv.get<picojson::array>()[0]);
    const auto l = ParseU64Value(rv.get<picojson::array>()[1]);
    if (!a || !l || !Gprb::IsMemAddr(static_cast<u32>(*a), static_cast<u32>(*l)))
      Fail("range must be inside MEM1/MEM2");
    out.emplace_back(static_cast<u32>(*a), static_cast<u32>(*l));
  }
  return out;
}

Result CmdFrameTraceConfig(const Args& args)
{
  Gprb::FrameTraceConfigure(GetBool(args, "enabled", true), ParseRanges(args, "ranges"));
  return {};
}

Result CmdFrameTrace(const Args& args)
{
  std::optional<u32> epoch;
  if (Find(args, "epoch"))
    epoch = static_cast<u32>(GetU64(args, "epoch"));
  const auto rows = Gprb::FrameTraceRows(epoch, static_cast<u32>(GetU64(args, "since", 0)),
                                         static_cast<u32>(GetU64(args, "max", 0)));
  picojson::array out;
  out.reserve(rows.size());
  for (const auto& r : rows)
  {
    // [game_frame, persistent, logic_steps, ticks, rng x3, fighters_crc, range_hash, game_set,
    //  resim, fighters (4 ports x {instance, damage, stocks, x, y, status})]
    picojson::array row;
    row.emplace_back(Num(r.game_frame));
    row.emplace_back(Num(r.persistent));
    row.emplace_back(Num(r.logic_steps));
    row.emplace_back(Num(r.ticks));
    for (u32 v : r.rng)
      row.emplace_back(Num(v));
    row.emplace_back(Num(r.fighters_crc));
    row.emplace_back(picojson::value(fmt::format("{:016x}", r.range_hash)));
    row.emplace_back(Num(r.game_set));
    row.emplace_back(picojson::value(r.resim));
    picojson::array f;
    for (const auto& port : r.fighters)
      for (u32 v : port)
        f.emplace_back(Num(v));
    row.emplace_back(picojson::value(std::move(f)));
    out.emplace_back(std::move(row));
  }
  Result res;
  res["epoch"] = Num(Gprb::FrameTraceEpoch());
  res["rows"] = picojson::value(std::move(out));
  return res;
}

Result CmdMemChunkHashes(const Args& args)
{
  const u64 chunk = GetU64(args, "chunk", 4096);
  if (chunk < 64 || chunk > (1u << 24))
    Fail("chunk must be 64..16 MiB");
  RequireRunning();
  picojson::array mem1, mem2;
  {
    const Core::CPUThreadGuard guard(Sys());
    RequireRunning();
    auto& memory = Sys().GetMemory();
    const std::array<std::tuple<const u8*, size_t, picojson::array*>, 2> regions{
        {{memory.GetRAM(), memory.GetRamSizeReal(), &mem1},
         {memory.GetEXRAM(), memory.GetExRamSizeReal(), &mem2}}};
    for (const auto& [base, size, out] : regions)
    {
      if (!base)
        continue;
      for (size_t off = 0; off < size; off += chunk)
      {
        const u64 h = XXH3_64bits(base + off, std::min<size_t>(chunk, size - off));
        out->emplace_back(picojson::value(fmt::format("{:016x}", h)));
      }
    }
  }
  Result r;
  r["chunk"] = Num(chunk);
  r["mem1"] = picojson::value(std::move(mem1));
  r["mem2"] = picojson::value(std::move(mem2));
  return r;
}

Result CmdDisasm(const Args& args)
{
  const u64 addr = GetU64(args, "addr");
  const u64 count = GetU64(args, "count", 16);
  if (count == 0 || count > 4096)
    Fail("count must be 1..4096");
  const u32 phys = TranslateRange(addr, count * 4);
  RequireRunning();
  picojson::array lines;
  const Core::CPUThreadGuard guard(Sys());
  const u8* ptr = Sys().GetMemory().GetPointerForRange(phys, count * 4);
  if (!ptr)
    Fail("memory not accessible");
  for (u64 i = 0; i < count; ++i)
  {
    u32 op;
    std::memcpy(&op, ptr + 4 * i, 4);
    op = Common::swap32(op);
    const u32 a = static_cast<u32>(addr + 4 * i);
    lines.emplace_back(picojson::value(
        fmt::format("{:08x} {:08x} {}", a, op, Common::GekkoDisassembler::Disassemble(op, a))));
  }
  Result r;
  r["lines"] = picojson::value(std::move(lines));
  return r;
}

// Debugging: let `cycles` of emulated time pass without executing code, at the current pause
// point. Shifts every later CoreTiming event (VI, audio DMA, DSP, IO completions, alarms) relative
// to the code. Used to test whether gameplay depends on the emulated-time phase.
Result CmdTimingNudge(const Args& args)
{
  const u64 cycles = GetU64(args, "cycles");
  if (cycles == 0 || cycles > 100'000'000)
    Fail("cycles must be 1..1e8");
  RequireRunning();
  if (Core::GetState(Sys()) != Core::State::Paused)
    Fail("timing_nudge requires the emulation to be paused");
  const Core::CPUThreadGuard guard(Sys());
  auto& ppc = Sys().GetPPCState();
  const u64 before = Sys().GetCoreTiming().GetTicks();
  ppc.downcount -= static_cast<int>(cycles);
  Result r;
  r["ticks_before"] = Num(before);
  r["downcount"] = picojson::value(static_cast<double>(ppc.downcount));
  return r;
}

Result CmdCpuTrace(const Args& args)
{
  if (Find(args, "path"))
  {
    const auto err = Gprb::CpuTraceArm(GetString(args, "path"),
                                       static_cast<u32>(GetU64(args, "first_frame")),
                                       static_cast<u32>(GetU64(args, "frames", 1)),
                                       GetU64(args, "max_records", 0),
                                       GetBool(args, "all_threads", false),
                                       GetBool(args, "include_irq", false),
                                       static_cast<u32>(GetU64(args, "occurrences", 1)));
    if (err)
      Fail(*err);
  }
  const auto st = Gprb::CpuTraceGetStatus();
  Result r;
  r["armed"] = picojson::value(st.armed);
  r["recording"] = picojson::value(st.recording);
  r["done"] = picojson::value(st.done);
  r["records"] = Num(st.records);
  r["main_thread"] = Num(st.main_thread);
  return r;
}

// Game-frame-anchored pads (GameplayRollback.h): record what each frame consumed, or inject
// per-frame gfPadStatus slots.
Result CmdGamePads(const Args& args)
{
  if (Find(args, "record"))
    Gprb::PadsSetRecording(GetBool(args, "record", false));
  if (GetBool(args, "clear", false))
    Gprb::PadsInjectClear();
  if (const picojson::value* rows = Find(args, "inject"))
  {
    // [[frame, "hex of 0x100 bytes"], ...]
    if (!rows->is<picojson::array>())
      Fail("'inject' must be a list of [frame, hex]");
    for (const auto& rv : rows->get<picojson::array>())
    {
      if (!rv.is<picojson::array>() || rv.get<picojson::array>().size() != 2 ||
          !rv.get<picojson::array>()[1].is<std::string>())
        Fail("each inject row must be [frame, hex]");
      const auto f = ParseU64Value(rv.get<picojson::array>()[0]);
      const std::vector<u8> bytes = FromHex(rv.get<picojson::array>()[1].get<std::string>());
      if (!f || bytes.size() != Gprb::PAD_SLOTS_SIZE)
        Fail("bad inject row");
      Gprb::PadSlots slots;
      std::copy(bytes.begin(), bytes.end(), slots.begin());
      Gprb::PadsInjectAdd(static_cast<u32>(*f), slots);
    }
  }
  if (Find(args, "ports"))
    Gprb::PadsInjectSetPorts(static_cast<u32>(GetU64(args, "ports")));
  if (Find(args, "anchor"))
    Gprb::PadsSetAnchor(static_cast<u32>(GetU64(args, "anchor")));
  Result r;
  if (Find(args, "since"))
  {
    picojson::array out;
    for (const auto& [frame, slots] : Gprb::PadsRecorded(static_cast<u32>(GetU64(args, "since"))))
    {
      picojson::array row;
      row.emplace_back(Num(frame));
      row.emplace_back(picojson::value(ToHex(slots.data(), slots.size())));
      out.emplace_back(std::move(row));
    }
    r["rows"] = picojson::value(std::move(out));
  }
  r["injected"] = Num(Gprb::PadsInjectedCount());
  return r;
}

// Gameplay-only rollback session (Core/Rollback/GameplaySession.h).
Result CmdGprbSyncTest(const Args& args)
{
  Gprb::Session::SyncTestOptions o;
  o.distance = static_cast<int>(GetU64(args, "distance", 2));
  o.region_set = GetString(args, "region_set", std::string("gp-v21"));
  o.hash_regions = GetBool(args, "hash_regions", true);
  o.start_frame = static_cast<u32>(GetU64(args, "start_frame", 240));
  o.ports = static_cast<u32>(GetU64(args, "ports", 3));
  o.suppress_resim_sounds = GetBool(args, "suppress_resim_sounds", false);
  o.dedupe_resim_sounds = GetBool(args, "dedupe_resim_sounds", false);
  o.inject_input = GetBool(args, "inject_input", false);
  o.mispredict_ports = static_cast<u32>(GetU64(args, "mispredict_ports", 0));
  // Signed (GetU64 refuses negative numbers).
  if (const auto it = args.find("mispredict_offset"); it != args.end() && it->second.is<double>())
    o.mispredict_offset = static_cast<int>(it->second.get<double>());
  o.mispredict_every = static_cast<int>(GetU64(args, "mispredict_every", 1));
  o.replay_path = GetString(args, "replay_path", std::string(""));
  o.no_rollback = GetBool(args, "no_rollback", false);
  if (const auto err = Gprb::Session::ArmSyncTest(o))
    Fail(*err);
  return Gprb::Session::Status().get<picojson::object>();
}

Result CmdGprbConnect(const Args& args)
{
  Gprb::Session::ConnectOptions o;
  const std::string role = GetString(args, "role");
  if (role != "host" && role != "join")
    Fail("role must be host or join");
  o.host = role == "host";
  o.local_port = static_cast<u16>(GetU64(args, "port", 0));
  o.remote_host = GetString(args, "host", std::string(""));
  o.remote_port = static_cast<u16>(GetU64(args, "remote_port", 0));
  o.region_set = GetString(args, "region_set", std::string("gp-v21"));
  o.start_frame = static_cast<u32>(GetU64(args, "start_frame", 240));
  o.delay = static_cast<int>(GetU64(args, "delay", 2));
  o.local_pad = static_cast<int>(GetU64(args, "local_pad", 0));
  o.sync_task_order = GetBool(args, "sync_task_order", true);
  o.hash_regions = GetBool(args, "hash_regions", false);
  o.suppress_resim_sounds = GetBool(args, "suppress_resim_sounds", false);
  o.dedupe_resim_sounds = GetBool(args, "dedupe_resim_sounds", !o.suppress_resim_sounds);
  o.name = GetString(args, "name", std::string(""));
  if (const auto err = Gprb::Session::Connect(o))
    Fail(*err);
  return Gprb::Session::Status().get<picojson::object>();
}

Result CmdGprbSetSelections(const Args& args)
{
  const picojson::value* sel = Find(args, "selections");
  if (!sel || !sel->is<picojson::object>())
    Fail("'selections' must be an object");
  if (const auto err = Gprb::Session::SetSelections(sel->get<picojson::object>()))
    Fail(*err);
  return Gprb::Session::Status().get<picojson::object>();
}

Result CmdGprbStatus(const Args&)
{
  return Gprb::Session::Status().get<picojson::object>();
}

Result CmdGprbChecksums(const Args& args)
{
  return Gprb::Session::Checksums(static_cast<s64>(GetU64(args, "since", 0))).get<picojson::object>();
}

// gprb_samples: with `every` (and optional `watch`: [[addr, size], ...]) configures region-set
// sampling for the next match; otherwise returns the samples (`frames`: full chunk hashes and watch
// bytes of these frames).
Result CmdGprbSamples(const Args& args)
{
  if (Find(args, "every"))
  {
    std::vector<std::pair<u32, u32>> watch;
    if (const picojson::value* w = Find(args, "watch"); w && w->is<picojson::array>())
    {
      for (const auto& e : w->get<picojson::array>())
      {
        if (!e.is<picojson::array>() || e.get<picojson::array>().size() != 2)
          Fail("watch entries must be [addr, size]");
        const auto& a = e.get<picojson::array>();
        const auto addr = ParseU64Value(a[0]);
        const auto size = ParseU64Value(a[1]);
        if (!addr || !size)
          Fail("watch entries must be [addr, size]");
        watch.emplace_back(static_cast<u32>(*addr), static_cast<u32>(*size));
      }
    }
    Gprb::Session::ConfigureSamples(static_cast<u32>(GetU64(args, "every")), std::move(watch));
    return {};
  }
  std::vector<s64> frames;
  if (const picojson::value* f = Find(args, "frames"); f && f->is<picojson::array>())
  {
    for (const auto& e : f->get<picojson::array>())
    {
      if (const auto v = ParseU64Value(e))
        frames.push_back(static_cast<s64>(*v));
    }
  }
  return Gprb::Session::Samples(frames).get<picojson::object>();
}

// gprb_sound_state: the sound archive player's allocated sounds (Gprb::Session::SoundState).
Result CmdGprbSoundState(const Args&)
{
  RequireRunning();
  const Core::CPUThreadGuard guard(Sys());
  return Gprb::Session::SoundState(guard).get<picojson::object>();
}

// gprb_census (PPR_GPRB_CENSUS=1): memory outside the region set written during the match.
Result CmdGprbCensus(const Args&)
{
  picojson::array out;
  for (const auto& [addr, size] : Rollback::RollbackManager::Get().CensusRanges())
    out.emplace_back(picojson::array{Num(addr), Num(size)});
  Result r;
  r["ranges"] = picojson::value(std::move(out));
  return r;
}

Result CmdGprbStop(const Args&)
{
  Gprb::Session::Stop();
  return Gprb::Session::Status().get<picojson::object>();
}

// ---------------------------------------------------------------------------------------------
// Online play (Core/Online)

Result CmdOnlineStatus(const Args&)
{
  return Online::Client::OnlineStatus();
}

Online::Matchmaking::Mode ParseMode(const std::string& name)
{
  using Mode = Online::Matchmaking::Mode;
  for (const Mode m : {Mode::Ranked, Mode::Unranked, Mode::Direct, Mode::Teams, Mode::Party})
  {
    if (name == Online::Matchmaking::ModeName(m))
      return m;
  }
  Fail("mode must be ranked, unranked, direct, teams or party");
}

Result StartSearch(const Args& args, Online::Matchmaking::Mode mode, std::string code)
{
  Online::Client::SearchOptions options;
  options.settings.mode = mode;
  options.settings.connect_code = std::move(code);
  // session: "auto" hands the connected match to the registered backend (netplay in NoGUI),
  // "none" keeps the P2P link open in the matchmaking.
  const std::string session = GetString(args, "session", "auto");
  if (session != "auto" && session != "none")
    Fail("session must be \"auto\" or \"none\"");
  options.hand_off = session == "auto";
  if (const picojson::value* sel = Find(args, "selections"))
  {
    if (!sel->is<picojson::object>())
      Fail("selections must be an object");
    options.selections = sel->get<picojson::object>();
  }
  // backend: which session the connected match goes to: "gameplay" (Gprb::Session,
  // gameplay-only rollback, the default [Online] SessionBackend) or "netplay" (whole-machine).
  // Omitted: the active backend stays.
  const std::string backend = GetString(args, "backend", "");
  if (backend == "gameplay")
  {
    Gprb::OnlineBackendOptions gp;
    if (Find(args, "delay"))
      gp.delay = static_cast<int>(GetU64(args, "delay"));
    gp.region_set = GetString(args, "region_set", gp.region_set);
    gp.dedupe_resim_sounds = GetBool(args, "dedupe_resim_sounds", gp.dedupe_resim_sounds);
    Gprb::SetOnlineBackendOptions(gp);
  }
  if (backend == "gameplay" || backend == "netplay")
  {
    if (const auto error = Online::Session::Select(backend))
      Fail(*error);
  }
  else if (!backend.empty())
  {
    Fail("backend must be \"netplay\" or \"gameplay\"");
  }
  NetPlaySession::OnlineOptions online;
  online.game = GetString(args, "game", "");
  if (Find(args, "delay"))
    online.delay = static_cast<int>(GetU64(args, "delay"));
  online.auto_start = GetBool(args, "auto_start", true);
  NetPlaySession::SetOnlineOptions(online);
  if (const auto error = Online::Client::FindMatch(options))
    Fail(*error);
  return Online::Client::MatchmakingStatus();
}

Result CmdMmSearchDirect(const Args& args)
{
  std::string code = GetString(args, "code");
  if (code.empty())
    Fail("code must not be empty");
  return StartSearch(args, Online::Matchmaking::Mode::Direct, std::move(code));
}

Result CmdMmSearch(const Args& args)
{
  return StartSearch(args, ParseMode(GetString(args, "mode")), GetString(args, "code", ""));
}

Result CmdMmStatus(const Args&)
{
  return Online::Client::MatchmakingStatus();
}

Result CmdMmCancel(const Args&)
{
  Online::Client::Cleanup();
  return Online::Client::MatchmakingStatus();
}

Result CmdOnlineSessionBackend(const Args& args)
{
  if (const auto error = Online::Session::Select(GetString(args, "backend")))
    Fail(*error);
  return Online::Client::MatchmakingStatus();
}

Result CmdGameBridgeStatus(const Args&)
{
  return Online::GameBridge::Status();
}

Result CmdGameBridgeConfig(const Args& args)
{
  if (Find(args, "enabled"))
    Online::GameBridge::SetEnabled(GetBool(args, "enabled", true));
  if (Find(args, "hand_off"))
    Online::GameBridge::SetHandOff(GetBool(args, "hand_off", true));
  return Online::GameBridge::Status();
}

// The recent connect codes behind the keypad's suggestions (Online/RecentCodes.h) of the
// logged-in account: list, and optionally clear or add first.
Result CmdOnlineRecentCodes(const Args& args)
{
  const std::string mode = GetString(args, "mode", "direct");
  if (mode != "direct" && mode != "teams")
    Fail("mode must be direct or teams");
  const auto kind = mode == "teams" ? Online::RecentCodes::Kind::Teams :
                                      Online::RecentCodes::Kind::Direct;
  const std::string uid = Online::Client::GetUser().GetUserInfo().uid;
  if (uid.empty())
    Fail("not logged in (no user.json read yet)");
  if (GetBool(args, "clear", false))
    Online::RecentCodes::Clear(kind, uid);
  if (Find(args, "add"))
  {
    const picojson::value* add = Find(args, "add");
    if (add->is<std::string>())
      Online::RecentCodes::Add(kind, uid, add->get<std::string>());
    else if (add->is<picojson::array>())
    {
      for (const auto& v : add->get<picojson::array>())
      {
        if (v.is<std::string>())
          Online::RecentCodes::Add(kind, uid, v.get<std::string>());
      }
    }
    else
    {
      Fail("add must be a code or a list of codes (oldest first)");
    }
  }
  Result result;
  result["mode"] = picojson::value(mode);
  result["path"] = picojson::value(Online::RecentCodes::PathFor(kind, uid));
  picojson::array codes;
  for (const auto& c : Online::RecentCodes::List(kind, uid))
    codes.emplace_back(c);
  result["codes"] = picojson::value(std::move(codes));
  if (Find(args, "prefix"))
  {
    // What the keypad's FETCH_CODE_SUGGESTION would get for this input.
    const u64 scroll = GetU64(args, "scroll", 3);
    if (scroll > 3)
      Fail("scroll must be 0 (none), 1 (older), 2 (newer) or 3 (reset)");
    const auto s = Online::RecentCodes::Suggest(kind, uid, GetString(args, "prefix"),
                                                static_cast<u32>(GetU64(args, "index", 0)),
                                                static_cast<Online::RecentCodes::Scroll>(scroll));
    picojson::object o;
    o["found"] = picojson::value(s.found);
    o["index"] = picojson::value(static_cast<double>(s.index));
    o["code"] = picojson::value(s.code);
    result["suggestion"] = picojson::value(std::move(o));
  }
  return result;
}

Result CmdLogMark(const Args& args)
{
  const std::string text = GetString(args, "text");
  NOTICE_LOG_FMT(HARNESS, "[HARNESS] {}", text);
  return {};
}

Result CmdQuit(const Args&)
{
  s_quit_requested = true;
  return {};
}

using Handler = Result (*)(const Args&);
const std::vector<std::pair<std::string_view, Handler>>& Handlers()
{
  static const std::vector<std::pair<std::string_view, Handler>> handlers{
      {"ping", CmdPing},
      {"status", CmdStatus},
      {"read_mem", CmdReadMem},
      {"write_mem", CmdWriteMem},
      {"read_u32", CmdReadU32},
      {"cpu_state", CmdCpuState},
      {"hash_mem", CmdHashMem},
      {"pad_set", CmdPadSet},
      {"pad_clear", CmdPadClear},
      {"pad_script", CmdPadScript},
      {"pad_script_status", CmdPadScriptStatus},
      {"wait_frame", CmdWaitFrame},
      {"pause", CmdPause},
      {"resume", CmdResume},
      {"frame_advance", CmdFrameAdvance},
      {"screenshot", CmdScreenshot},
      {"save_state", CmdSaveState},
      {"load_state", CmdLoadState},
      {"netplay_host", CmdNetPlayHost},
      {"netplay_join", CmdNetPlayJoin},
      {"netplay_start", CmdNetPlayStart},
      {"netplay_status", CmdNetPlayStatus},
      {"netplay_leave", CmdNetPlayLeave},
      {"online_status", CmdOnlineStatus},
      {"mm_search_direct", CmdMmSearchDirect},
      {"mm_search", CmdMmSearch},
      {"mm_status", CmdMmStatus},
      {"mm_cancel", CmdMmCancel},
      {"online_session_backend", CmdOnlineSessionBackend},
      {"game_bridge_status", CmdGameBridgeStatus},
      {"game_bridge_config", CmdGameBridgeConfig},
      {"online_recent_codes", CmdOnlineRecentCodes},
      {"rollback_pad_history", CmdRollbackPadHistory},
      {"rollback_chunk_hashes", CmdRollbackChunkHashes},
      {"frame_trace", CmdFrameTrace},
      {"frame_trace_config", CmdFrameTraceConfig},
      {"mem_chunk_hashes", CmdMemChunkHashes},
      {"disasm", CmdDisasm},
      {"timing_nudge", CmdTimingNudge},
      {"cpu_trace", CmdCpuTrace},
      {"game_pads", CmdGamePads},
      {"gprb_synctest", CmdGprbSyncTest},
      {"gprb_connect", CmdGprbConnect},
      {"gprb_set_selections", CmdGprbSetSelections},
      {"gprb_status", CmdGprbStatus},
      {"gprb_checksums", CmdGprbChecksums},
      {"gprb_stop", CmdGprbStop},
      {"gprb_samples", CmdGprbSamples},
      {"gpu_state", CmdGpuState},
      {"gprb_census", CmdGprbCensus},
      {"gprb_sound_state", CmdGprbSoundState},
      {"rollback_timings", CmdRollbackTimings},
      {"log_mark", CmdLogMark},
      {"quit", CmdQuit},
  };
  return handlers;
}

std::string HandleLine(const std::string& line)
{
  picojson::value id;  // null unless provided
  picojson::object response;

  picojson::value request;
  const std::string parse_error = picojson::parse(request, line);
  if (!parse_error.empty() || !request.is<picojson::object>())
  {
    response["id"] = id;
    response["ok"] = picojson::value(false);
    response["error"] = picojson::value(parse_error.empty() ? std::string("request must be a JSON object") :
                                                              "invalid JSON: " + parse_error);
    return picojson::value(response).serialize() + "\n";
  }

  const Args& args = request.get<picojson::object>();
  if (const auto it = args.find("id"); it != args.end())
    id = it->second;
  response["id"] = id;

  try
  {
    const std::string cmd = GetString(args, "cmd");
    const auto& handlers = Handlers();
    const auto it = std::ranges::find_if(handlers, [&](const auto& h) { return h.first == cmd; });
    if (it == handlers.end())
      Fail(fmt::format("unknown command '{}'", cmd));

    Result result;
    {
      std::lock_guard lk(s_command_mutex);
      result = it->second(args);
    }
    response["ok"] = picojson::value(true);
    response["result"] = picojson::value(result);
  }
  catch (const CommandError& e)
  {
    response["ok"] = picojson::value(false);
    response["error"] = picojson::value(e.message);
  }
  catch (const std::exception& e)
  {
    response["ok"] = picojson::value(false);
    response["error"] = picojson::value(std::string("internal error: ") + e.what());
  }
  return picojson::value(response).serialize() + "\n";
}

void RequestQuit()
{
  INFO_LOG_FMT(HARNESS, "Quit requested by harness client");
  Core::QueueHostJob(
      [](Core::System&) {
        NetPlaySession::ShutdownSession();
        if (GetHost().request_quit)
          GetHost().request_quit();
      },
      true);
}

// Every request is one JSON object per line. Anything else (an HTTP request a web page made the
// browser send to 127.0.0.1, say) closes the connection before a later line can be taken as a
// command: a page cannot read the answers, but without this its request body would still run.
bool LooksLikeRequest(const std::string& line)
{
  const size_t first = line.find_first_not_of(" \t");
  return first != std::string::npos && line[first] == '{';
}

// A line longer than this without a newline is not a harness request.
constexpr size_t MAX_REQUEST_BYTES = 64 * 1024 * 1024;

void ClientThread(socket_t sock)
{
  Common::SetCurrentThreadName("Harness client");
  std::string buffer;
  std::array<char, 65536> chunk;

  while (s_server_running.load())
  {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(sock, &read_set);
    timeval tv{0, 200 * 1000};
    const int ready = select(static_cast<int>(sock + 1), &read_set, nullptr, nullptr, &tv);
    if (ready < 0)
      break;
    if (ready == 0)
      continue;

    const int received =
        static_cast<int>(recv(sock, chunk.data(), static_cast<int>(chunk.size()), 0));
    if (received <= 0)
      break;
    buffer.append(chunk.data(), static_cast<size_t>(received));
    if (buffer.size() > MAX_REQUEST_BYTES && buffer.find('\n') == std::string::npos)
      break;

    size_t newline;
    bool closed = false;
    while ((newline = buffer.find('\n')) != std::string::npos)
    {
      std::string line = buffer.substr(0, newline);
      buffer.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty())
        continue;
      if (!LooksLikeRequest(line))
      {
        WARN_LOG_FMT(HARNESS, "Harness: a line that is not a JSON request; closing the connection");
        (void)SendAll(sock, "{\"id\":null,\"ok\":false,\"error\":\"not a harness request\"}\n");
        closed = true;
        break;
      }

      const std::string response = HandleLine(line);
      if (!SendAll(sock, response))
      {
        closed = true;
        break;
      }

      if (s_quit_requested.exchange(false))
        RequestQuit();
    }
    if (closed)
      break;
  }

  {
    std::lock_guard lk(s_client_mutex);
    CloseSocket(sock);
    s_client_socket = INVALID_SOCK;
  }
  s_client_busy = false;
}

void AcceptThread()
{
  Common::SetCurrentThreadName("Harness server");
  while (s_server_running.load())
  {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(s_listen_socket, &read_set);
    timeval tv{0, 200 * 1000};
    const int ready =
        select(static_cast<int>(s_listen_socket + 1), &read_set, nullptr, nullptr, &tv);
    if (ready <= 0)
      continue;

    sockaddr_in addr{};
#ifdef _WIN32
    int addr_len = sizeof(addr);
#else
    socklen_t addr_len = sizeof(addr);
#endif
    const socket_t client = accept(s_listen_socket, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (client == INVALID_SOCK)
      continue;

    int nodelay = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
               sizeof(nodelay));
#ifdef SO_NOSIGPIPE
    int nosigpipe = 1;
    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

    if (s_client_busy.load())
    {
      SendAll(client,
              "{\"id\":null,\"ok\":false,\"error\":\"another harness client is already "
              "connected\"}\n");
      CloseSocket(client);
      continue;
    }

    std::lock_guard lk(s_client_mutex);
    if (s_client_thread.joinable())
      s_client_thread.join();
    s_client_busy = true;
    s_client_socket = client;
    s_client_thread = std::thread(ClientThread, client);
  }
}
}  // namespace

bool StartServer(u16 port)
{
#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
  {
    ERROR_LOG_FMT(HARNESS, "WSAStartup failed");
    return false;
  }
#endif

  s_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s_listen_socket == INVALID_SOCK)
  {
    ERROR_LOG_FMT(HARNESS, "Failed to create the harness socket");
    return false;
  }

#ifndef _WIN32
  int reuse = 1;
  setsockopt(s_listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#else
  // No other socket may bind this port while we hold it (Windows lets a SO_REUSEADDR socket of
  // the same user take over a bound port otherwise).
  BOOL exclusive = TRUE;
  setsockopt(s_listen_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
             reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s_listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      listen(s_listen_socket, 4) != 0)
  {
    ERROR_LOG_FMT(HARNESS, "Failed to listen on 127.0.0.1:{}", port);
    fprintf(stderr, "Harness: failed to listen on 127.0.0.1:%u\n", port);
    CloseSocket(s_listen_socket);
    s_listen_socket = INVALID_SOCK;
    return false;
  }

  s_server_running = true;
  s_accept_thread = std::thread(AcceptThread);
  return true;
}

void StopServer()
{
  if (!s_server_running.exchange(false))
    return;

  NotifyWaiters();
  if (s_accept_thread.joinable())
    s_accept_thread.join();

  {
    std::lock_guard lk(s_client_mutex);
    if (s_client_socket != INVALID_SOCK)
    {
#ifdef _WIN32
      shutdown(s_client_socket, SD_BOTH);
#else
      shutdown(s_client_socket, SHUT_RDWR);
#endif
    }
  }
  if (s_client_thread.joinable())
    s_client_thread.join();

  CloseSocket(s_listen_socket);
  s_listen_socket = INVALID_SOCK;
}
}  // namespace Harness::Internal
