// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/MachineSession.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <enet/enet.h>
#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/ENet.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Core/Brawlback/include/brawlback-common/BrawlbackConstants.h"
#include "Core/Brawlback/include/gekkonet/GekkoLib/include/gekkonet.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/WII_IPC.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/Harness/Harness.h"
#include "Core/NetPlayProto.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/PresentStats.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace HLE_Misc
{
bool IsBootScene(const Core::CPUThreadGuard& guard, u32 app_ptr);
}

namespace Machine
{
namespace
{
using Clock = std::chrono::steady_clock;

// The Brawl game loop (main.dol; the same in every Brawl build).
constexpr u32 BRAWL_LOOP_TOP = 0x800171b4;      // li r25,1
constexpr u32 BRAWL_FRAME_END = 0x80017504;     // stw r0,0x100(r23)
constexpr u32 BRAWL_LOOP_END = 0x80017508;      // b loop top
constexpr u32 BRAWL_LOOP_TOP_OPCODE = 0x3B200001;

// ENet channels of the session's links.
constexpr u8 CHANNEL_GEKKO = 0;    // GekkoNet's packets (unsequenced)
constexpr u8 CHANNEL_CONTROL = 1;  // small JSON messages (reliable)
constexpr u8 CHANNEL_COUNT = 3;    // 2: machine images (later)
constexpr size_t MAX_CONTROL_SIZE = 4096;
constexpr size_t MAX_GEKKO_PACKET = 4096;
constexpr int MAX_ADVANCE = MAX_ROLLBACK_FRAMES + 2;
constexpr size_t HISTORY_SIZE = 1 << 14;
// GekkoNet drops a peer it has not heard from for this long.
constexpr unsigned int DISCONNECT_TIMEOUT_MS = 5000;

enum class Phase
{
  Idle,       // nothing armed
  Armed,      // armed, the game has not reached its first boundary
  Connecting, // at the first boundary, waiting for every peer
  Starting,   // GekkoNet runs its handshake / the start barrier
  Running,
  Stopped,
  Error,
};

const char* PhaseName(Phase p)
{
  switch (p)
  {
  case Phase::Idle: return "idle";
  case Phase::Armed: return "armed";
  case Phase::Connecting: return "connecting";
  case Phase::Starting: return "starting";
  case Phase::Running: return "running";
  case Phase::Stopped: return "stopped";
  case Phase::Error: return "error";
  }
  return "?";
}

struct PeerLink
{
  int slot = -1;
  std::string host;
  u16 port = 0;
  ENetPeer* peer = nullptr;  // the connection kept for this slot
  bool hello = false;        // its hello arrived on `peer`
  bool started = false;      // its GekkoNet session started (start barrier)
  bool go = false;           // the barrier's host told it to go
  bool left = false;
  Clock::time_point last_connect{};
  double rtt_ms = -1;
};

struct PendingOps
{
  int count = 0;
  std::array<s32, MAX_ADVANCE> frame{};
  std::array<bool, MAX_ADVANCE> save_after{};
  std::array<unsigned int*, MAX_ADVANCE> checksum{};
  std::array<std::array<Input, MAX_PLAYERS>, MAX_ADVANCE> inputs{};
  void Clear()
  {
    count = 0;
    save_after.fill(false);
    checksum.fill(nullptr);
  }
};

struct HistoryEntry
{
  s64 frame = -1;
  std::array<Input, MAX_PLAYERS> inputs{};
  u32 checksum = 0;
};

struct State
{
  std::recursive_timed_mutex mutex;
  Options opts;
  std::atomic<Phase> phase{Phase::Idle};
  std::string error;
  bool env_checked = false;
  // The player's controller is a GameCube controller adapter (their own SI device setting).
  bool local_adapter = false;
  std::array<Controller, MAX_PLAYERS> controller{};
  u32 wiimote_captures = 0;
  std::optional<WiimoteOverride> wiimote_override;

  // Network (net thread, under `net_mutex`).
  std::mutex net_mutex;
  ENetHost* host = nullptr;
  std::thread net_thread;
  std::atomic<bool> net_run{false};
  std::array<PeerLink, MAX_PLAYERS> peers;  // by slot (this player's unused)
  std::deque<std::pair<int, std::vector<u8>>> gekko_rx;  // (slot, packet)
  std::vector<GekkoNetResult*> gekko_rx_ptrs;
  std::vector<std::pair<int, std::vector<u8>>> gekko_tx;  // (slot, packet)

  // GekkoNet and the frame loop (CPU thread).
  GekkoSession* gekko = nullptr;
  GekkoNetAdapter adapter{};
  bool gekko_started = false;
  bool barrier_done = false;
  std::optional<Clock::time_point> start_at;
  Clock::time_point last_barrier_send{};
  std::array<int, MAX_PLAYERS> handle_slot{-1, -1, -1, -1};
  std::array<int, MAX_PLAYERS> slot_handle{-1, -1, -1, -1};
  int local_handle = -1;
  u32 players = 0;  // a bit per slot that plays
  PendingOps ops;
  int iteration = 0;
  bool resim = false;
  struct
  {
    bool pending = false;
    s64 frame = 0;
    unsigned int* checksum = nullptr;
  } queued_save;
  std::array<Input, MAX_PLAYERS> latched{};  // the inputs of the frame running now
  Input local_input{};
  s64 current_frame = -1;
  double speed = 1.0;
  s64 last_timesync_frame = -1;

  // Stats.
  u64 rollbacks = 0, max_rollback = 0, frames_resimulated = 0, stall_polls = 0, skipped_loads = 0;
  u64 desyncs = 0, saves = 0;
  s64 last_desync_frame = -1;
  float frames_ahead = 0;
  std::vector<HistoryEntry> history = std::vector<HistoryEntry>(HISTORY_SIZE);
  // The NAND journal mark of each snapshot in RollbackManager's ring, oldest first: a load drops
  // the newer ones, and the journal forgets changes older than the oldest snapshot.
  std::deque<u64> journal_marks;
  u64 journal_errors = 0;
  std::vector<std::string> log;
};

State s;

const char* s_gekko_addr[MAX_PLAYERS] = {"p0", "p1", "p2", "p3"};

void AfterSave(Core::System& system);
void AfterLoad(Core::System& system, int frames_back);

void AddLog(std::string line)
{
  INFO_LOG_FMT(BRAWLBACK, "machine: {}", line);
  if (s.log.size() >= 64)
    s.log.erase(s.log.begin());
  s.log.push_back(std::move(line));
}

void BroadcastControl(const picojson::object& msg);

void Fail(std::string why)
{
  ERROR_LOG_FMT(BRAWLBACK, "machine: {}", why);
  s.error = std::move(why);
  s.phase = Phase::Error;
  OSD::AddMessage("Session ended: " + s.error, 10000, OSD::Color::RED);
  // The others stop too, instead of waiting for frames that never come.
  std::lock_guard lk(s.net_mutex);
  if (s.host)
  {
    picojson::object msg;
    msg["type"] = picojson::value("leave");
    msg["reason"] = picojson::value(s.error);
    BroadcastControl(msg);
    enet_host_flush(s.host);
  }
}

// ------------------------------------------------------------------------------ network thread

void SendControl(int slot, const picojson::object& msg)
{
  // net_mutex held
  PeerLink& p = s.peers[slot];
  if (!p.peer)
    return;
  const std::string text = picojson::value(msg).serialize();
  ENetPacket* packet =
      enet_packet_create(text.data(), text.size(), ENET_PACKET_FLAG_RELIABLE);
  enet_peer_send(p.peer, CHANNEL_CONTROL, packet);
}

void BroadcastControl(const picojson::object& msg)
{
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    if (slot != s.opts.local_slot && s.peers[slot].slot >= 0 && s.peers[slot].hello)
      SendControl(slot, msg);
  }
}

// ENetPeer::data: the peer's slot + 1 (0: not known yet), with OUTGOING for a connection this
// side started.
constexpr intptr_t OUTGOING = 0x100;

int SlotOf(ENetPeer* peer)
{
  return static_cast<int>((reinterpret_cast<intptr_t>(peer->data) & 0xFF) - 1);
}

// Both sides connect to each other (needed through NATs); of two connections for the same pair,
// both keep the one the lower slot started, so they agree without another round trip.
void OnHello(ENetPeer* peer, const picojson::object& msg)
{
  const auto it = msg.find("slot");
  if (it == msg.end() || !it->second.is<double>())
    return;
  const double v = it->second.get<double>();
  if (v < 0 || v >= MAX_PLAYERS)
    return;
  const int slot = static_cast<int>(v);
  if (slot == s.opts.local_slot || s.peers[slot].slot != slot)
  {
    enet_peer_disconnect(peer, 0);
    return;
  }
  PeerLink& p = s.peers[slot];
  const bool lower_is_me = s.opts.local_slot < slot;
  const bool this_is_mine = (reinterpret_cast<intptr_t>(peer->data) & OUTGOING) != 0;
  const bool keep = lower_is_me ? this_is_mine : !this_is_mine;
  if (!keep)
    return;  // the other connection is the one both keep; this one idles until it drops
  if (p.peer && p.peer != peer)
    enet_peer_disconnect_later(p.peer, 0);
  p.peer = peer;
  p.hello = true;
  AddLog(fmt::format("peer {} connected ({}:{})", slot, p.host, p.port));
}

void HandleControl(ENetPeer* peer, const u8* data, size_t size)
{
  if (size > MAX_CONTROL_SIZE)
    return;
  picojson::value v;
  const std::string err = picojson::parse(v, std::string(reinterpret_cast<const char*>(data), size));
  if (!err.empty() || !v.is<picojson::object>())
    return;
  const auto& msg = v.get<picojson::object>();
  const auto type_it = msg.find("type");
  if (type_it == msg.end() || !type_it->second.is<std::string>())
    return;
  const std::string& type = type_it->second.get<std::string>();
  if (type == "hello")
  {
    OnHello(peer, msg);
    return;
  }
  const int slot = SlotOf(peer);
  if (slot < 0 || slot >= MAX_PLAYERS || s.peers[slot].peer != peer)
    return;
  PeerLink& p = s.peers[slot];
  if (type == "started")
    p.started = true;
  else if (type == "go")
    p.go = true;
  else if (type == "leave")
    p.left = true;
}

void NetThread()
{
  Common::SetCurrentThreadName("Machine session");
  while (s.net_run.load())
  {
    std::unique_lock lk(s.net_mutex);
    if (!s.host)
      break;
    // Outgoing: GekkoNet's packets queued by the CPU thread.
    for (auto& [slot, data] : s.gekko_tx)
    {
      PeerLink& p = s.peers[slot];
      if (!p.peer || !p.hello)
        continue;
      ENetPacket* packet =
          enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_UNSEQUENCED);
      enet_peer_send(p.peer, CHANNEL_GEKKO, packet);
    }
    s.gekko_tx.clear();
    // Keep connecting to every peer without a link (both sides do).
    const auto now = Clock::now();
    for (PeerLink& p : s.peers)
    {
      if (p.slot < 0 || p.hello || p.host.empty() || now - p.last_connect < std::chrono::seconds(1))
        continue;
      p.last_connect = now;
      ENetAddress addr{};
      if (enet_address_set_host(&addr, p.host.c_str()) != 0)
        continue;
      addr.port = p.port;
      ENetPeer* peer = enet_host_connect(s.host, &addr, CHANNEL_COUNT, 0);
      if (peer)
        peer->data = reinterpret_cast<void*>(static_cast<intptr_t>(p.slot + 1) | OUTGOING);
    }
    ENetEvent ev;
    int r = enet_host_service(s.host, &ev, 0);
    while (r > 0)
    {
      switch (ev.type)
      {
      case ENET_EVENT_TYPE_CONNECT:
      {
        enet_peer_timeout(ev.peer, 0, 5000, 10000);
        picojson::object hello;
        hello["type"] = picojson::value("hello");
        hello["slot"] = picojson::value(static_cast<double>(s.opts.local_slot));
        hello["name"] = picojson::value(s.opts.name);
        const std::string text = picojson::value(hello).serialize();
        enet_peer_send(ev.peer, CHANNEL_CONTROL,
                       enet_packet_create(text.data(), text.size(), ENET_PACKET_FLAG_RELIABLE));
        break;
      }
      case ENET_EVENT_TYPE_RECEIVE:
      {
        if (ev.channelID == CHANNEL_CONTROL)
        {
          // An incoming connection learns its slot from the hello.
          if (ev.peer->data == nullptr)
          {
            picojson::value v;
            const std::string text(reinterpret_cast<const char*>(ev.packet->data),
                                   std::min<size_t>(ev.packet->dataLength, MAX_CONTROL_SIZE));
            if (picojson::parse(v, text).empty() && v.is<picojson::object>())
            {
              const auto& o = v.get<picojson::object>();
              const auto it = o.find("slot");
              if (it != o.end() && it->second.is<double>())
              {
                const double sl = it->second.get<double>();
                if (sl >= 0 && sl < MAX_PLAYERS)
                  ev.peer->data = reinterpret_cast<void*>(static_cast<intptr_t>(sl) + 1);
              }
            }
          }
          HandleControl(ev.peer, ev.packet->data, ev.packet->dataLength);
        }
        else if (ev.channelID == CHANNEL_GEKKO && ev.packet->dataLength <= MAX_GEKKO_PACKET)
        {
          const int slot = SlotOf(ev.peer);
          if (slot >= 0 && slot < MAX_PLAYERS && s.peers[slot].peer == ev.peer)
          {
            s.gekko_rx.emplace_back(slot, std::vector<u8>(ev.packet->data,
                                                          ev.packet->data + ev.packet->dataLength));
            if (s.gekko_rx.size() > 4096)
              s.gekko_rx.pop_front();
          }
        }
        enet_packet_destroy(ev.packet);
        break;
      }
      case ENET_EVENT_TYPE_DISCONNECT:
      {
        for (PeerLink& p : s.peers)
        {
          if (p.peer == ev.peer)
          {
            p.peer = nullptr;
            p.hello = false;
            if (s.phase == Phase::Running || s.phase == Phase::Starting)
              p.left = true;
            AddLog(fmt::format("peer {} disconnected", p.slot));
          }
        }
        break;
      }
      default:
        break;
      }
      r = enet_host_check_events(s.host, &ev);
    }
    for (PeerLink& p : s.peers)
    {
      if (p.peer)
        p.rtt_ms = p.peer->roundTripTime;
    }
    lk.unlock();
    // ~1 ms: GekkoNet's packets go out at most this late.
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
}

bool StartNetwork()
{
  std::lock_guard lk(s.net_mutex);
  if (s.opts.synctest > 0)
    return true;
  ENetAddress addr{};
  addr.host = ENET_HOST_ANY;
  addr.port = s.opts.local_port;
  s.host = enet_host_create(&addr, MAX_PLAYERS * 2, CHANNEL_COUNT, 0, 0);
  if (!s.host)
  {
    s.error = fmt::format("could not bind UDP port {}", s.opts.local_port);
    return false;
  }
  s.net_run = true;
  s.net_thread = std::thread(NetThread);
  return true;
}

void StopNetwork()
{
  s.net_run = false;
  if (s.net_thread.joinable())
    s.net_thread.join();
  std::lock_guard lk(s.net_mutex);
  if (s.host)
  {
    for (PeerLink& p : s.peers)
    {
      if (p.peer)
        enet_peer_disconnect_now(p.peer, 0);
      p.peer = nullptr;
      p.hello = false;
    }
    enet_host_flush(s.host);
    enet_host_destroy(s.host);
    s.host = nullptr;
  }
  s.gekko_rx.clear();
  s.gekko_tx.clear();
}

// ------------------------------------------------------------------------------ GekkoNet adapter

void GekkoSend(GekkoNetAddress* addr, const char* data, int length)
{
  if (!addr || !addr->data || addr->size != 2 || length <= 0)
    return;
  const char* a = static_cast<const char*>(addr->data);
  const int slot = a[1] - '0';
  if (slot < 0 || slot >= MAX_PLAYERS)
    return;
  std::lock_guard lk(s.net_mutex);
  s.gekko_tx.emplace_back(slot, std::vector<u8>(data, data + length));
}

GekkoNetResult** GekkoReceive(int* length)
{
  std::lock_guard lk(s.net_mutex);
  s.gekko_rx_ptrs.clear();
  for (auto& [slot, data] : s.gekko_rx)
  {
    // GekkoNet frees every pointer it is handed with free_data (GekkoFree).
    auto* result = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    result->addr.size = 2;
    result->addr.data = std::malloc(2);
    std::memcpy(result->addr.data, s_gekko_addr[slot], 2);
    result->data_len = static_cast<unsigned int>(data.size());
    result->data = std::malloc(data.size());
    std::memcpy(result->data, data.data(), data.size());
    s.gekko_rx_ptrs.push_back(result);
  }
  s.gekko_rx.clear();
  *length = static_cast<int>(s.gekko_rx_ptrs.size());
  return s.gekko_rx_ptrs.data();
}

void GekkoFree(void* p)
{
  std::free(p);
}

void DestroyGekko()
{
  if (s.gekko)
    gekko_destroy(&s.gekko);
  s.gekko = nullptr;
  s.gekko_started = false;
  s.handle_slot.fill(-1);
  s.slot_handle.fill(-1);
  s.local_handle = -1;
}

bool CreateGekko()
{
  DestroyGekko();
  const bool stress = s.opts.synctest > 0;
  if (!gekko_create(&s.gekko, stress ? GekkoStressSession : GekkoGameSession))
    return false;
  int n = 0;
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
    n += (s.players >> slot) & 1;
  GekkoConfig cfg{};
  cfg.num_players = static_cast<unsigned char>(n);
  cfg.max_spectators = 0;
  cfg.input_prediction_window = MAX_ROLLBACK_FRAMES;
  cfg.input_size = INPUT_SIZE;
  cfg.state_size = sizeof(u32);
  cfg.limited_saving = false;
  cfg.desync_detection = true;
  cfg.check_distance = stress ? static_cast<unsigned int>(s.opts.synctest) : 0;
  gekko_start(s.gekko, &cfg);
  if (!stress)
  {
    s.adapter.send_data = GekkoSend;
    s.adapter.receive_data = GekkoReceive;
    s.adapter.free_data = GekkoFree;
    gekko_net_adapter_set(s.gekko, &s.adapter);
  }
  // Handles follow the slots in order, the same on every machine.
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    if (!((s.players >> slot) & 1))
      continue;
    int h;
    if (slot == s.opts.local_slot)
    {
      h = gekko_add_actor(s.gekko, GekkoLocalPlayer, nullptr);
      s.local_handle = h;
    }
    else
    {
      GekkoNetAddress addr{const_cast<char*>(s_gekko_addr[slot]), 2};
      h = gekko_add_actor(s.gekko, GekkoRemotePlayer, &addr);
    }
    if (h < 0 || h >= MAX_PLAYERS)
    {
      DestroyGekko();
      return false;
    }
    s.handle_slot[h] = slot;
    s.slot_handle[slot] = h;
  }
  if (!stress)
  {
    gekko_set_local_delay(s.gekko, s.local_handle, static_cast<unsigned char>(s.opts.delay));
    gekko_set_disconnect_timeout(s.gekko, DISCONNECT_TIMEOUT_MS);
  }
  else
  {
    s.gekko_started = true;
  }
  return true;
}

// ------------------------------------------------------------------------------ frame loop

void CaptureLocalWiimote(Core::System& system, Input* in)
{
  auto* hid = WiimoteCommon::GetHIDWiimoteSource(static_cast<unsigned int>(s.opts.local_pad));
  WiimoteEmu::DesiredWiimoteState state;
  if (hid)
  {
    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Bluetooth);
    g_controller_interface.UpdateInput();
    const bool bar = static_cast<bool>(system.GetWiiIPC().GetGPIOOutFlags()[IOS::GPIO::SENSOR_BAR]);
    // The motion simulation (swing, shake, tilt) steps at the Wii Remote's 200 Hz: three or four
    // steps per frame, 3.33 on average.
    const int steps = (s.wiimote_captures++ % 3 == 0) ? 4 : 3;
    for (int i = 0; i < steps; ++i)
    {
      hid->PrepareInput(&state, bar ? WiimoteCommon::HIDWiimote::SensorBarState::Enabled :
                                      WiimoteCommon::HIDWiimote::SensorBarState::Disabled);
    }
  }
  if (s.wiimote_override)
  {
    const WiimoteOverride& o = *s.wiimote_override;
    state.buttons.hex = o.buttons & WiimoteCommon::ButtonData::BUTTON_MASK;
    if (o.nunchuk)
    {
      WiimoteEmu::Nunchuk::DataFormat nc{};
      nc.jx = o.stick_x;
      nc.jy = o.stick_y;
      nc.SetAccel({0x200, 0x200, 0x2CC});
      nc.SetButtons(o.nunchuk_buttons);
      state.extension.data = nc;
    }
    else
    {
      state.extension.data = std::monostate{};
    }
  }
  const WiimoteEmu::SerializedWiimoteState ser = WiimoteEmu::SerializeDesiredState(state);
  const u8 len = std::min<u8>(ser.length, 31);
  (*in)[0] |= INPUT_WIIMOTE_CONNECTED;
  (*in)[9] = len;
  std::memcpy(in->data() + 10, ser.data.data(), len);
}

void CaptureLocalInput()
{
  if (s.controller[s.opts.local_slot] == Controller::WiiRemote)
  {
    Input in{};
    CaptureLocalWiimote(Core::System::GetInstance(), &in);
    s.local_input = in;
    return;
  }
  GCPadStatus pad;
  if (s.local_adapter)
  {
    pad = GCAdapter::Input(s.opts.local_pad);
    Harness::ApplyPadOverride(s.opts.local_pad, &pad);
  }
  else
  {
    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
    g_controller_interface.UpdateInput();
    pad = Pad::GetStatus(s.opts.local_pad);
  }
  Input in{};
  EncodePad(pad, &in);
  s.local_input = in;
}

void RecordHistory(s64 frame, const std::array<Input, MAX_PLAYERS>& inputs, u32 checksum)
{
  if (frame < 0)
    return;
  HistoryEntry& e = s.history[static_cast<size_t>(frame) % HISTORY_SIZE];
  e.frame = frame;
  e.inputs = inputs;
  e.checksum = checksum;
}

void ProcessSessionEvents()
{
  int count = 0;
  GekkoSessionEvent** events = gekko_session_events(s.gekko, &count);
  for (int i = 0; i < count; ++i)
  {
    const GekkoSessionEvent& e = *events[i];
    switch (e.type)
    {
    case GekkoSessionStarted:
      s.gekko_started = true;
      AddLog("GekkoNet session started");
      break;
    case GekkoPlayerDisconnected:
    {
      const int h = e.data.disconnected.handle;
      const int slot = h >= 0 && h < MAX_PLAYERS ? s.handle_slot[h] : -1;
      AddLog(fmt::format("player {} dropped by GekkoNet", slot));
      // Until epochs exist (phase 3), a dropped player ends a 2-player session.
      Fail(fmt::format("player {} disconnected", slot + 1));
      break;
    }
    case GekkoDesyncDetected:
      s.desyncs++;
      s.last_desync_frame = e.data.desynced.frame;
      WARN_LOG_FMT(BRAWLBACK, "machine: desync at frame {} (local {:08x}, remote {:08x})",
                   e.data.desynced.frame, e.data.desynced.local_checksum,
                   e.data.desynced.remote_checksum);
      break;
    default:
      break;
    }
  }
}

// Polls GekkoNet with this frame's local input and turns its events into the passes to run.
void Update(Core::System& system)
{
  s.ops.Clear();
  gekko_network_poll(s.gekko);
  gekko_add_local_input(s.gekko, s.local_handle, s.local_input.data());
  int count = 0;
  GekkoGameEvent** events = gekko_update_session(s.gekko, &count);
  ProcessSessionEvents();
  if (s.phase == Phase::Error)
    return;

  bool pending_load = false;
  int load_frame = 0;
  int load_before = -1;
  bool initial_save = false;
  unsigned int* initial_checksum = nullptr;
  for (int i = 0; i < count; ++i)
  {
    GekkoGameEvent& e = *events[i];
    switch (e.type)
    {
    case GekkoSaveEvent:
      if (e.data.save.state_len)
        *e.data.save.state_len = sizeof(u32);
      if (s.ops.count > 0)
      {
        s.ops.save_after[s.ops.count - 1] = true;
        s.ops.checksum[s.ops.count - 1] = e.data.save.checksum;
      }
      else
      {
        initial_save = true;
        initial_checksum = e.data.save.checksum;
      }
      break;
    case GekkoLoadEvent:
      pending_load = true;
      load_frame = e.data.load.frame;
      break;
    case GekkoAdvanceEvent:
    {
      if (s.ops.count >= MAX_ADVANCE)
      {
        ERROR_LOG_FMT(BRAWLBACK, "machine: more than {} advances in one update", MAX_ADVANCE);
        break;
      }
      const int k = s.ops.count++;
      s.ops.frame[k] = e.data.adv.frame;
      if (pending_load && load_before < 0)
        load_before = k;
      pending_load = false;
      for (auto& in : s.ops.inputs[k])
        in.fill(0);
      if (e.data.adv.inputs && e.data.adv.input_len >= INPUT_SIZE)
      {
        const int n = static_cast<int>(e.data.adv.input_len / INPUT_SIZE);
        for (int h = 0; h < n && h < MAX_PLAYERS; ++h)
        {
          const int slot = s.handle_slot[h];
          if (slot >= 0)
            std::memcpy(s.ops.inputs[k][slot].data(), e.data.adv.inputs + h * INPUT_SIZE,
                        INPUT_SIZE);
        }
      }
      break;
    }
    default:
      break;
    }
  }

  if (load_before >= 0)
  {
    // GekkoNet's load names the frame whose end state to restore; the ring's newest slot holds the
    // end of `newest - 1` (NetPlayClient::HandleGekkoFrame explains the arithmetic).
    auto& rb = Rollback::RollbackManager::Get();
    const int newest = s.ops.frame[s.ops.count - 1];
    const int frames_back = (newest - 1) - load_frame;
    if (frames_back >= 1 && frames_back <= MAX_ROLLBACK_FRAMES && rb.m_ring_count >= 2 &&
        frames_back < rb.m_ring_count)
    {
      rb.LoadFrame(system, frames_back);
      AfterLoad(system, frames_back);
      s.rollbacks++;
      s.max_rollback = std::max<u64>(s.max_rollback, static_cast<u64>(frames_back));
    }
    else if (frames_back != 0)
    {
      s.skipped_loads++;
      ERROR_LOG_FMT(BRAWLBACK, "machine: cannot load {} frame(s) back (ring {})", frames_back,
                    rb.m_ring_count);
    }
  }
  if (initial_save)
  {
    Rollback::RollbackManager::Get().SaveFrame(system);
    AfterSave(system);
    s.saves++;
    if (initial_checksum)
      *initial_checksum = Rollback::RollbackManager::Get().LastSaveChecksum();
  }
  if (s.ops.count == 0)
    s.stall_polls++;
  if (s.ops.count > 1)
    s.frames_resimulated += static_cast<u64>(s.ops.count - 1);
  s.frames_ahead = gekko_frames_ahead(s.gekko);
}

void UpdateTimeSync(Core::System& system)
{
  // As NetPlayClient::UpdateTimeSync (Slippi's): nudge the speed of the peer that runs ahead.
  constexpr s64 INTERVAL = 15;
  if (s.opts.synctest > 0 || s.current_frame < 0 || s.current_frame % INTERVAL != 0 ||
      s.current_frame == s.last_timesync_frame)
    return;
  s.last_timesync_frame = s.current_frame;
  const float ahead = s.frames_ahead;
  double factor = 1.0;
  if (ahead > 0.25f)
    factor = 1.0 - std::min(ahead / 2.0f, 1.0f) * 0.02;
  else if (ahead < -0.25f)
    factor = 1.0 + std::min(-ahead / 2.0f, 1.0f) * 0.01;
  s.speed = factor;
}

IOS::HLE::FS::HostFileSystem* HostFS(Core::System& system)
{
  const auto ios = system.GetIOS();
  if (!ios)
    return nullptr;
  return static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get());
}

// After every snapshot: keep its NAND journal mark, and let the journal forget what no snapshot
// in the ring can go back to any more.
void AfterSave(Core::System& system)
{
  auto* fs = HostFS(system);
  if (!fs)
    return;
  s.journal_marks.push_back(fs->LastJournalMark());
  while (s.journal_marks.size() > static_cast<size_t>(Rollback::NUM_SAVE_SLOTS))
  {
    s.journal_marks.pop_front();
    fs->JournalTrim(s.journal_marks.front());
  }
  if (fs->TakeJournalError())
  {
    s.journal_errors++;
    WARN_LOG_FMT(BRAWLBACK, "machine: the NAND journal failed; the NAND may differ");
  }
}

// After a load of `frames_back` snapshots back: the newer snapshots are gone.
void AfterLoad(Core::System& system, int frames_back)
{
  for (int i = 0; i < frames_back && !s.journal_marks.empty(); ++i)
    s.journal_marks.pop_back();
  if (auto* fs = HostFS(system); fs && fs->TakeJournalError())
  {
    s.journal_errors++;
    WARN_LOG_FMT(BRAWLBACK, "machine: undoing NAND changes failed; the NAND may differ");
  }
}

// Sync test diagnostics: the granules each save copied (address -> CRC of the 64 bytes), per
// frame, so that a re-run whose checksum differs from the first run can say where.
struct GranuleMap
{
  s64 frame = -1;
  std::map<u32, u32> granules;
  std::map<u32, std::array<u8, ROLLBACK_PAGE_SIZE>> bytes;
};
std::array<GranuleMap, 16> s_first_runs;
std::vector<std::string> s_diag;
// PPR_MACHINE_DIGESTS: the last frames' granule digests, by frame.
struct FrameDigest
{
  s64 frame = -1;
  u32 checksum = 0;
  std::vector<std::pair<u32, u32>> granules;
};
std::array<FrameDigest, 256> s_digests;
bool DigestsEnabled()
{
  static const bool on = std::getenv("PPR_MACHINE_DIGESTS") != nullptr;
  return on;
}
u64 s_diag_frames = 0;

void DiagnoseSave(s64 frame)
{
  if (s.opts.synctest <= 0 || frame < 0)
    return;
  const auto& rb = Rollback::RollbackManager::Get();
  const auto& slot = rb.m_slots[(rb.m_ring_next + Rollback::NUM_SAVE_SLOTS - 1) % Rollback::NUM_SAVE_SLOTS];
  GranuleMap now;
  now.frame = frame;
  const u8* gpu = Rollback::GPUWrittenBitmap::Get().entries;
  auto add = [&](const Rollback::RegionDelta& d, u32 base) {
    for (u32 i = 0; i < d.page_count; ++i)
    {
      const u32 addr = base + d.page_indices[i] * ROLLBACK_PAGE_SIZE;
      if (gpu[(addr & 0x1FFFFFFF) >> ROLLBACK_PAGE_SHIFT] ||
          (addr >= 0x90000800 && addr < 0x90000800 + 0x12c800))
        continue;
      now.granules[addr] = Common::ComputeCRC32(d.page_data.data() + i * ROLLBACK_PAGE_SIZE,
                                                ROLLBACK_PAGE_SIZE);
      std::memcpy(now.bytes[addr].data(), d.page_data.data() + i * ROLLBACK_PAGE_SIZE,
                  ROLLBACK_PAGE_SIZE);
    }
  };
  add(slot.m_mem1_delta, 0x80000000);
  add(slot.m_mem2_delta, 0x90000000);
  GranuleMap& first = s_first_runs[static_cast<size_t>(frame) % s_first_runs.size()];
  if (first.frame != frame)
  {
    first = std::move(now);
    return;
  }
  // A re-run of `frame`: report what differs from its first run.
  std::vector<std::string> diffs;
  for (const auto& [addr, crc] : first.granules)
  {
    const auto it = now.granules.find(addr);
    if (it == now.granules.end())
      diffs.push_back(fmt::format("{:08x} only first", addr));
    else if (it->second != crc)
    {
      // The differing 4-byte words: offset first->rerun.
      std::string words;
      const auto& a = first.bytes[addr];
      const auto& b = now.bytes[addr];
      for (u32 w = 0; w < ROLLBACK_PAGE_SIZE; w += 4)
      {
        if (std::memcmp(&a[w], &b[w], 4) != 0)
        {
          words += fmt::format(" +{:x}:{:02x}{:02x}{:02x}{:02x}->{:02x}{:02x}{:02x}{:02x}", w, a[w],
                               a[w + 1], a[w + 2], a[w + 3], b[w], b[w + 1], b[w + 2], b[w + 3]);
        }
      }
      diffs.push_back(fmt::format("{:08x} differs [{}]", addr, words));
    }
  }
  for (const auto& [addr, crc] : now.granules)
  {
    if (!first.granules.contains(addr))
      diffs.push_back(fmt::format("{:08x} only rerun", addr));
  }
  if (!diffs.empty())
  {
    s_diag_frames++;
    if (s_diag.size() < 200)
    {
      std::string line = fmt::format("frame {}: {} granules ({} first, {} rerun):", frame,
                                     diffs.size(), first.granules.size(), now.granules.size());
      for (size_t i = 0; i < diffs.size() && i < 12; ++i)
        line += " " + diffs[i];
      s_diag.push_back(std::move(line));
    }
  }
}

void PerformQueuedSave(Core::System& system)
{
  if (!s.queued_save.pending)
    return;
  s.queued_save.pending = false;
  auto& rb = Rollback::RollbackManager::Get();
  rb.SaveFrame(system);
  AfterSave(system);
  s.saves++;
  DiagnoseSave(s.queued_save.frame);
  const u32 sum = rb.LastSaveChecksum();
  if (DigestsEnabled() && s.queued_save.frame >= 0)
  {
    FrameDigest& d = s_digests[static_cast<size_t>(s.queued_save.frame) % s_digests.size()];
    d.frame = s.queued_save.frame;
    d.checksum = sum;
    d.granules = rb.LastSaveDigest();
  }
  if (s.queued_save.checksum)
    *s.queued_save.checksum = sum;
  // Record the frame's inputs with its checksum (the pass that ran it last is the one that counts).
  const s64 frame = s.queued_save.frame;
  HistoryEntry& e = s.history[static_cast<size_t>(frame) % HISTORY_SIZE];
  if (e.frame == frame)
    e.checksum = sum;
}

bool StartBarrier()
{
  // Every peer holds the first frame until all have started GekkoNet; then the lowest slot says
  // go and starts half a round trip later (NetPlayClient::PollStartSync, for N players).
  if (s.opts.synctest > 0)
    return true;
  std::lock_guard lk(s.net_mutex);
  const auto now = Clock::now();
  int lowest = s.opts.local_slot;
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    if ((s.players >> slot) & 1)
    {
      lowest = std::min(lowest, slot);
    }
  }
  if (now - s.last_barrier_send > std::chrono::milliseconds(50))
  {
    s.last_barrier_send = now;
    picojson::object msg;
    msg["type"] = picojson::value("started");
    BroadcastControl(msg);
  }
  if (lowest == s.opts.local_slot)
  {
    if (!s.start_at)
    {
      double rtt = 0;
      for (const PeerLink& p : s.peers)
      {
        if (p.slot < 0)
          continue;
        if (!p.started)
          return false;
        rtt = std::max(rtt, p.rtt_ms);
      }
      picojson::object go;
      go["type"] = picojson::value("go");
      BroadcastControl(go);
      s.start_at = now + std::chrono::microseconds(static_cast<s64>(rtt * 500));
      AddLog(fmt::format("start barrier: everyone started, go in {} ms", rtt / 2));
    }
    return now >= *s.start_at;
  }
  return s.peers[lowest].go;
}

bool PeersConnected()
{
  std::lock_guard lk(s.net_mutex);
  for (const PeerLink& p : s.peers)
  {
    if (p.slot >= 0 && !p.hello)
      return false;
  }
  return true;
}

bool AnyPeerLeft(std::string* who)
{
  std::lock_guard lk(s.net_mutex);
  for (const PeerLink& p : s.peers)
  {
    if (p.slot >= 0 && p.left)
    {
      *who = fmt::format("player {}", p.slot + 1);
      return true;
    }
  }
  return false;
}

enum class Wait
{
  Frames,  // this pass has a frame to run
  Ended,   // the session ended: run the pass as a plain local frame
  Yield,   // the emulation is pausing or stopping: run no game logic this pass
};

// Called at iteration 0 of a pass: runs the connection, the barrier and GekkoNet until this pass
// has a frame to run. Waits on the host, never in the guest: guest code run while waiting would
// move emulated time in a way no other machine (and no re-run) repeats.
Wait WaitForFrames(Core::System& system)
{
  // A peer that sends nothing for this long is gone (GekkoNet drops one after 5 s of silence; a
  // start barrier or handshake that never completes ends the same way).
  constexpr auto STALL_LIMIT = std::chrono::seconds(10);
  constexpr auto CONNECT_LIMIT = std::chrono::seconds(60);
  auto& cpu = system.GetCPU();
  bool waited = false;
  const auto since = Clock::now();
  for (;;)
  {
    const auto waited_for = Clock::now() - since;
    if ((s.phase == Phase::Running && waited_for > STALL_LIMIT) ||
        (s.phase != Phase::Running && waited_for > CONNECT_LIMIT))
    {
      Fail(s.phase == Phase::Running ? "Connection lost" : "Could not connect to the players");
      return Wait::Ended;
    }
    const Phase phase = s.phase;
    if (phase == Phase::Error || phase == Phase::Stopped || phase == Phase::Idle)
      return Wait::Ended;
    // A stop (or a pause, which a session refuses) needs the CPU thread back.
    if (cpu.GetState() != CPU::State::Running)
      return Wait::Yield;
    if (phase == Phase::Connecting)
    {
      if (PeersConnected())
      {
        if (!CreateGekko())
        {
          Fail("GekkoNet could not start");
          return Wait::Ended;
        }
        s.phase = Phase::Starting;
        AddLog("every peer connected; GekkoNet handshake");
      }
    }
    if (s.phase == Phase::Starting)
    {
      if (!s.gekko_started)
      {
        gekko_network_poll(s.gekko);
        int count = 0;
        gekko_update_session(s.gekko, &count);
        ProcessSessionEvents();
      }
      else if (StartBarrier())
      {
        s.phase = Phase::Running;
        AddLog("running");
        OSD::AddMessage("Session started", 3000, OSD::Color::GREEN);
      }
      else
      {
        gekko_network_poll(s.gekko);
      }
    }
    if (s.phase == Phase::Running)
    {
      std::string who;
      if (AnyPeerLeft(&who))
      {
        Fail(who + " left");
        return Wait::Ended;
      }
      CaptureLocalInput();
      Update(system);
      if (s.phase == Phase::Error)
        return Wait::Ended;
      if (s.ops.count > 0)
        break;
    }
    waited = true;
    // The lock is let go while sleeping, so status and control requests get through. OnLoopTop
    // holds it exactly once.
    s.mutex.unlock();
    std::this_thread::sleep_for(std::chrono::microseconds(250));
    s.mutex.lock();
  }
  if (waited)
    system.GetCoreTiming().ResetThrottleToNow();
  return Wait::Frames;
}

void BeginPass(Core::System& system)
{
  // The inputs this pass runs with, for the SI (GetPad) and later the Wii Remotes.
  s.latched = s.ops.inputs[s.iteration];
  s.current_frame = s.ops.frame[s.iteration];
  s.resim = s.ops.count > 1 && s.iteration != s.ops.count - 1;
  RecordHistory(s.current_frame, s.latched, 0);
  auto& core_timing = system.GetCoreTiming();
  core_timing.SetRollbackResimulating(s.resim);
  UpdateTimeSync(system);
  core_timing.SetRollbackSpeedAdjustment(s.speed);
  if (!s.resim)
    Rollback::PresentStats::OnDisplayedFrameStart(s.ops.count > 1);
}

bool Active()
{
  const Phase p = s.phase;
  return p == Phase::Armed || p == Phase::Connecting || p == Phase::Starting ||
         p == Phase::Running;
}

void CheckEnvironment()
{
  if (s.env_checked)
    return;
  s.env_checked = true;
  const char* env = std::getenv("PPR_MACHINE_SESSION");
  if (!env || !*env)
    return;
  picojson::value v;
  const std::string err = picojson::parse(v, env);
  std::string why;
  if (!err.empty())
  {
    ERROR_LOG_FMT(BRAWLBACK, "machine: PPR_MACHINE_SESSION is not JSON: {}", err);
    return;
  }
  const auto opts = ParseOptions(v, &why);
  if (!opts)
  {
    ERROR_LOG_FMT(BRAWLBACK, "machine: PPR_MACHINE_SESSION: {}", why);
    return;
  }
  // Arm takes the lock again (recursive).
  if (!Arm(*opts, &why))
    ERROR_LOG_FMT(BRAWLBACK, "machine: could not arm: {}", why);
}

template <typename T>
T Pinned(const Config::Info<T>& info)
{
  // A setting that changes what the game computes: Dolphin's default, or the game's own value
  // from Dolphin's game settings (Sys/GameSettings), never the player's own choice, so every
  // machine runs with the same.
  const auto& layer = Config::GetLayer(Config::LayerType::GlobalGame);
  if (layer)
  {
    if (const auto v = layer->template Get<T>(info.GetLocation()))
      return *v;
  }
  return info.GetDefaultValue();
}
}  // namespace

GCPadStatus DecodePad(const Input& in)
{
  GCPadStatus pad{};
  if (!(in[0] & INPUT_GC_PRESENT))
  {
    pad.isConnected = true;
    pad.stickX = GCPadStatus::MAIN_STICK_CENTER_X;
    pad.stickY = GCPadStatus::MAIN_STICK_CENTER_Y;
    pad.substickX = GCPadStatus::C_STICK_CENTER_X;
    pad.substickY = GCPadStatus::C_STICK_CENTER_Y;
    return pad;
  }
  pad.button = static_cast<u16>((in[1] << 8) | in[2]);
  pad.stickX = in[3];
  pad.stickY = in[4];
  pad.substickX = in[5];
  pad.substickY = in[6];
  pad.triggerLeft = in[7];
  pad.triggerRight = in[8];
  pad.isConnected = true;
  return pad;
}

void EncodePad(const GCPadStatus& pad, Input* in)
{
  (*in)[0] |= INPUT_GC_PRESENT;
  // PAD_GET_ORIGIN and the other non-button bits stay local.
  const u16 buttons = pad.button & 0x1F7F;
  (*in)[1] = static_cast<u8>(buttons >> 8);
  (*in)[2] = static_cast<u8>(buttons & 0xFF);
  (*in)[3] = pad.stickX;
  (*in)[4] = pad.stickY;
  (*in)[5] = pad.substickX;
  (*in)[6] = pad.substickY;
  (*in)[7] = pad.triggerLeft;
  (*in)[8] = pad.triggerRight;
}

std::optional<Options> ParseOptions(const picojson::value& json, std::string* error)
{
  if (!json.is<picojson::object>())
  {
    *error = "options must be an object";
    return std::nullopt;
  }
  const auto& o = json.get<picojson::object>();
  auto num = [&](const char* key, double def) {
    const auto it = o.find(key);
    return it != o.end() && it->second.is<double>() ? it->second.get<double>() : def;
  };
  Options opts;
  const double slot = num("slot", 0);
  const double port = num("port", 0);
  const double delay = num("delay", 2);
  const double synctest = num("synctest", 0);
  const double pad = num("pad", 0);
  if (slot < 0 || slot >= MAX_PLAYERS || port < 0 || port > 65535 || delay < 0 || delay > 9 ||
      synctest < 0 || synctest > MAX_ROLLBACK_FRAMES || pad < 0 || pad > 3)
  {
    *error = "slot, port, delay, synctest or pad out of range";
    return std::nullopt;
  }
  opts.local_slot = static_cast<int>(slot);
  opts.local_port = static_cast<u16>(port);
  opts.delay = static_cast<int>(delay);
  opts.synctest = static_cast<int>(synctest);
  opts.local_pad = static_cast<int>(pad);
  const double rtc = num("rtc", static_cast<double>(opts.rtc));
  if (rtc < 946684800.0 || rtc > 4102444800.0)
  {
    *error = "rtc must be seconds since 1970 between 2000 and 2100";
    return std::nullopt;
  }
  opts.rtc = static_cast<u64>(rtc);
  if (const auto it = o.find("name"); it != o.end() && it->second.is<std::string>())
    opts.name = it->second.get<std::string>().substr(0, 64);
  auto controller_of = [](const picojson::object& obj, Controller* c) {
    const auto it = obj.find("controller");
    if (it == obj.end())
      return true;
    if (!it->second.is<std::string>())
      return false;
    const std::string& v = it->second.get<std::string>();
    if (v == "gc")
      *c = Controller::GameCube;
    else if (v == "wiimote")
      *c = Controller::WiiRemote;
    else
      return false;
    return true;
  };
  if (!controller_of(o, &opts.controller))
  {
    *error = "controller must be \"gc\" or \"wiimote\"";
    return std::nullopt;
  }
  if (const auto it = o.find("peers"); it != o.end() && it->second.is<picojson::array>())
  {
    for (const auto& pv : it->second.get<picojson::array>())
    {
      if (!pv.is<picojson::object>())
        continue;
      const auto& po = pv.get<picojson::object>();
      Peer p;
      const auto sl = po.find("slot");
      const auto h = po.find("host");
      const auto pt = po.find("port");
      if (sl == po.end() || !sl->second.is<double>() || h == po.end() ||
          !h->second.is<std::string>() || pt == po.end() || !pt->second.is<double>())
      {
        *error = "a peer needs slot, host and port";
        return std::nullopt;
      }
      const double ps = sl->second.get<double>();
      const double pp = pt->second.get<double>();
      if (ps < 0 || ps >= MAX_PLAYERS || ps == slot || pp <= 0 || pp > 65535)
      {
        *error = "a peer's slot or port is out of range";
        return std::nullopt;
      }
      p.slot = static_cast<int>(ps);
      if (!controller_of(po, &p.controller))
      {
        *error = "a peer's controller must be \"gc\" or \"wiimote\"";
        return std::nullopt;
      }
      p.host = h->second.get<std::string>();
      p.port = static_cast<u16>(pp);
      opts.peers.push_back(std::move(p));
    }
  }
  if (opts.synctest > 0 && !opts.peers.empty())
  {
    *error = "a sync test has no peers";
    return std::nullopt;
  }
  return opts;
}

bool Arm(const Options& options, std::string* error)
{
  std::lock_guard lk(s.mutex);
  s.env_checked = true;
  if (Active())
  {
    *error = "a session is already armed";
    return false;
  }
  StopNetwork();
  DestroyGekko();
  s.opts = options;
  s.error.clear();
  s.log.clear();
  s.peers = {};
  s.players = 1u << options.local_slot;
  s.controller = {};
  s.controller[options.local_slot] = options.controller;
  s.wiimote_captures = 0;
  s.local_adapter = Config::Get(Config::GetInfoForSIDevice(options.local_pad)) ==
                    SerialInterface::SIDEVICE_WIIU_ADAPTER;
  for (const Peer& p : options.peers)
  {
    PeerLink& link = s.peers[p.slot];
    link.slot = p.slot;
    link.host = p.host;
    link.port = p.port;
    s.players |= 1u << p.slot;
    s.controller[p.slot] = p.controller;
  }
  s.ops.Clear();
  s.iteration = 0;
  s.resim = false;
  s.queued_save = {};
  s.latched = {};
  s.local_input = {};
  s.current_frame = -1;
  s.speed = 1.0;
  s.last_timesync_frame = -1;
  s.barrier_done = false;
  s.start_at.reset();
  s.rollbacks = s.max_rollback = s.frames_resimulated = s.stall_polls = s.skipped_loads = 0;
  s.desyncs = s.saves = 0;
  s.last_desync_frame = -1;
  for (auto& e : s.history)
    e.frame = -1;
  if (!StartNetwork())
  {
    *error = s.error;
    return false;
  }
  s.phase = Phase::Armed;
  AddLog(fmt::format("armed: slot {}, {} peer(s), port {}, delay {}{}", options.local_slot,
                     options.peers.size(), options.local_port, options.delay,
                     options.synctest ? fmt::format(", sync test {}", options.synctest) : ""));
  return true;
}

void Stop()
{
  if (s.phase == Phase::Idle)
    return;
  // First, without the lock: a CPU thread waiting at a boundary (WaitForFrames, which holds the
  // lock) sees it and lets go.
  if (Active())
    s.phase = Phase::Stopped;
  std::lock_guard lk(s.mutex);
  {
    std::lock_guard nlk(s.net_mutex);
    picojson::object msg;
    msg["type"] = picojson::value("leave");
    if (s.host)
    {
      BroadcastControl(msg);
      enet_host_flush(s.host);
    }
  }
  StopNetwork();
  DestroyGekko();
  Rollback::RollbackManager::Get().m_full_ios_in_snapshots = false;
  if (auto* fs = HostFS(Core::System::GetInstance()))
    fs->JournalStop();
  s.journal_marks.clear();
  s.phase = Phase::Stopped;
  AddLog("stopped");
  OSD::AddMessage("Session ended", 3000, OSD::Color::YELLOW);
}

bool IsArmed()
{
  if (!s.env_checked)
  {
    std::lock_guard lk(s.mutex);
    CheckEnvironment();
  }
  return Active();
}

bool IsRunning()
{
  return s.phase == Phase::Running;
}

u64 GetInitialRTC()
{
  return s.opts.rtc;
}

NetPlay::NetSettings MakeNetSettings()
{
  NetPlay::NetSettings ns{};
  ns.m_RollbackMode = true;
  ns.rollback_delay = s.opts.delay;
  // The CPU thread is the player's choice: with the deterministic GPU thread (forced for rollback
  // by the netplay layer) single and dual core compute the same.
  ns.cpu_thread = Config::Get(Config::MAIN_CPU_THREAD);
  ns.cpu_core = Config::Get(Config::MAIN_CPU_CORE);
  ns.enable_cheats = false;
  ns.selected_language = Pinned(Config::MAIN_GC_LANGUAGE);
  ns.override_region_settings = Pinned(Config::MAIN_OVERRIDE_REGION_SETTINGS);
  ns.dsp_hle = Pinned(Config::MAIN_DSP_HLE);
  ns.dsp_enable_jit = Pinned(Config::MAIN_DSP_JIT);
  ns.ram_override_enable = false;
  ns.mem1_size = Pinned(Config::MAIN_MEM1_SIZE);
  ns.mem2_size = Pinned(Config::MAIN_MEM2_SIZE);
  ns.fallback_region = Pinned(Config::MAIN_FALLBACK_REGION);
  // The SD card is read-only in a session (docs/design/brawl-mods.md §4).
  ns.allow_sd_writes = false;
  ns.oc_enable = false;
  ns.oc_factor = 1.0f;
  ns.vi_oc_enable = false;
  ns.vi_oc_factor = 1.0f;
  for (ExpansionInterface::Slot slot : ExpansionInterface::SLOTS)
    ns.exi_device[slot] = ExpansionInterface::EXIDeviceType::None;
  ns.memcard_size_override = -1;
  for (size_t i = 0; i < Config::SYSCONF_SETTINGS.size(); ++i)
  {
    std::visit(
        [&](auto* info) { ns.sysconf_settings[i] = static_cast<u32>(Pinned(*info)); },
        Config::SYSCONF_SETTINGS[i].config_info);
  }
  ns.efb_access_enable = Pinned(Config::GFX_HACK_EFB_ACCESS_ENABLE);
  ns.bbox_enable = Pinned(Config::GFX_HACK_BBOX_ENABLE);
  ns.force_progressive = Pinned(Config::GFX_HACK_FORCE_PROGRESSIVE);
  ns.efb_to_texture_enable = Pinned(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
  ns.xfb_to_texture_enable = Pinned(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM);
  ns.disable_copy_to_vram = Pinned(Config::GFX_HACK_DISABLE_COPY_TO_VRAM);
  ns.immediate_xfb_enable = Config::Get(Config::GFX_HACK_IMMEDIATE_XFB);
  ns.efb_emulate_format_changes = Pinned(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES);
  ns.safe_texture_cache_color_samples = Pinned(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES);
  ns.perf_queries_enable = Pinned(Config::GFX_PERF_QUERIES_ENABLE);
  ns.float_exceptions = Pinned(Config::MAIN_FLOAT_EXCEPTIONS);
  ns.divide_by_zero_exceptions = Pinned(Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS);
  ns.fprf = Pinned(Config::MAIN_FPRF);
  ns.accurate_nans = Pinned(Config::MAIN_ACCURATE_NANS);
  ns.accurate_fmadds = Pinned(Config::MAIN_ACCURATE_FMADDS);
  ns.disable_icache = Pinned(Config::MAIN_DISABLE_ICACHE);
  ns.sync_on_skip_idle = Pinned(Config::MAIN_SYNC_ON_SKIP_IDLE);
  ns.sync_gpu = Pinned(Config::MAIN_SYNC_GPU);
  ns.sync_gpu_max_distance = Pinned(Config::MAIN_SYNC_GPU_MAX_DISTANCE);
  ns.sync_gpu_min_distance = Pinned(Config::MAIN_SYNC_GPU_MIN_DISTANCE);
  ns.sync_gpu_overclock = Pinned(Config::MAIN_SYNC_GPU_OVERCLOCK);
  ns.jit_follow_branch = Pinned(Config::MAIN_JIT_FOLLOW_BRANCH);
  ns.fast_disc_speed = false;
  ns.mmu = Pinned(Config::MAIN_MMU);
  ns.fastmem = Config::Get(Config::MAIN_FASTMEM);
  ns.skip_ipl = true;
  ns.load_ipl_dump = false;
  ns.defer_efb_copies = Pinned(Config::GFX_HACK_DEFER_EFB_COPIES);
  ns.efb_access_tile_size = Pinned(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE);
  ns.efb_access_defer_invalidation = Pinned(Config::GFX_HACK_EFB_DEFER_INVALIDATION);
  ns.use_fma = Config::Get(Config::SESSION_USE_FMA);
  ns.savedata_load = true;
  // The session runs on a temporary NAND (WiiRoot), thrown away at the end: the game may write
  // its save there (Brawl asks to create one, and asks again if the write fails).
  ns.savedata_write = true;
  ns.savedata_sync_all_wii = false;
  ns.strict_settings_sync = false;
  ns.golf_mode = false;
  ns.hide_remote_gbas = false;
  // Player ids are slot + 1. Every player's port is a standard controller; empty ports have none
  // (BootManager sets the session's SI devices after this layer).
  ns.pad_map.fill(0);
  ns.wiimote_map.fill(0);
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    if (!((s.players >> slot) & 1))
      continue;
    if (s.controller[slot] == Controller::WiiRemote)
      ns.wiimote_map[slot] = static_cast<NetPlay::PlayerId>(slot + 1);
    else
      ns.pad_map[slot] = static_cast<NetPlay::PlayerId>(slot + 1);
  }
  ns.local_player_id = static_cast<NetPlay::PlayerId>(s.opts.local_slot + 1);
  return ns;
}

bool IsResimulationPass()
{
  return Active() && s.resim;
}

bool GetPad(int port, GCPadStatus* status)
{
  if (!IsArmed())
    return false;
  if (port < 0 || port >= MAX_PLAYERS)
    return false;
  if (!((s.players >> port) & 1) || s.controller[port] != Controller::GameCube)
  {
    *status = GCPadStatus{};
    status->isConnected = false;
    return true;
  }
  // Before the session runs (boot) every player's controller is neutral on every machine.
  *status = DecodePad(s.latched[port]);
  return true;
}

// A Wii Remote at rest, no extension (the struct has no assignment).
void ResetWiimoteState(WiimoteEmu::DesiredWiimoteState* state)
{
  state->buttons = {};
  state->acceleration = WiimoteEmu::DesiredWiimoteState::DEFAULT_ACCELERATION;
  state->camera_points = WiimoteEmu::DesiredWiimoteState::DEFAULT_CAMERA;
  state->motion_plus.reset();
  state->extension.data = std::monostate{};
}

bool GetWiimote(int index, WiimoteEmu::DesiredWiimoteState* state)
{
  if (!IsArmed() || index < 0 || index >= MAX_PLAYERS)
    return false;
  if (!((s.players >> index) & 1) || s.controller[index] != Controller::WiiRemote)
    return false;
  const Input& in = s.latched[index];
  ResetWiimoteState(state);
  // Before the session runs (boot), or a player who sent nothing yet: a Wii Remote at rest.
  if (!(in[0] & INPUT_WIIMOTE_CONNECTED))
    return true;
  WiimoteEmu::SerializedWiimoteState ser{};
  ser.length = std::min<u8>(in[9], static_cast<u8>(ser.data.size()));
  std::memcpy(ser.data.data(), in.data() + 10, ser.length);
  // The bytes came from a peer: anything that does not parse is a Wii Remote at rest.
  if (!WiimoteEmu::DeserializeDesiredState(state, ser))
    ResetWiimoteState(state);
  return true;
}

void SetWiimoteOverride(std::optional<WiimoteOverride> value)
{
  std::lock_guard lk(s.mutex);
  s.wiimote_override = value;
}

bool OnLoopTop(const Core::CPUThreadGuard& guard)
{
  if (!IsArmed())
    return false;
  std::lock_guard lk(s.mutex);
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  auto run_original = [&] {
    ppc.gpr[25] = 1;
    ppc.npc = BRAWL_LOOP_TOP + 4;
  };

  // The hook fires only for main.dol's loop; until the game reaches scBoot it runs freely, the
  // same on every machine (no input, the session's pinned settings and clock).
  if (s.phase == Phase::Armed)
  {
    if (system.GetMemory().Read_U32(BRAWL_LOOP_TOP) != BRAWL_LOOP_TOP_OPCODE ||
        !HLE_Misc::IsBootScene(guard, ppc.gpr[23]))
    {
      run_original();
      return true;
    }
    auto& rb = Rollback::RollbackManager::Get();
    rb.SetBrawlExclusions(true);
    rb.m_full_ios_in_snapshots = true;
    s.journal_marks.clear();
    if (!rb.m_frame_save_enabled.load(std::memory_order_relaxed))
      rb.ToggleFrameSave();
    s.phase = Phase::Connecting;
    AddLog("first boundary (scBoot): waiting for the players");
  }
  if (s.phase == Phase::Error || s.phase == Phase::Stopped)
  {
    system.GetCoreTiming().SetRollbackResimulating(false);
    system.GetCoreTiming().SetRollbackSpeedAdjustment(1.0);
    run_original();
    return true;
  }

  PerformQueuedSave(system);
  if (s.iteration == 0 || s.iteration >= s.ops.count)
  {
    s.iteration = 0;
    switch (WaitForFrames(system))
    {
    case Wait::Frames:
      break;
    case Wait::Ended:
      // The session ended: from here the game runs on its own (as a local game).
      system.GetCoreTiming().SetRollbackResimulating(false);
      system.GetCoreTiming().SetRollbackSpeedAdjustment(1.0);
      run_original();
      return true;
    case Wait::Yield:
      // No game logic this pass; the next loop top waits again.
      ppc.gpr[25] = 1;
      ppc.npc = BRAWL_LOOP_END;
      s.ops.count = 0;
      return true;
    }
  }
  BeginPass(system);
  run_original();
  return true;
}

bool OnFrameEnd(const Core::CPUThreadGuard& guard)
{
  if (!IsArmed())
    return false;
  std::lock_guard lk(s.mutex);
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  // The original instruction first (stw r0,0x100(r23)), so the save at the next loop top holds
  // this pass's frame counter (HLE_Misc::BrawlbackGekkoNetFrameEnd).
  PowerPC::MMU::HostWrite<u32>(guard, ppc.gpr[0], ppc.gpr[23] + 0x100);
  ppc.npc = BRAWL_LOOP_END;
  if (s.phase == Phase::Running && s.iteration < s.ops.count && s.ops.save_after[s.iteration])
  {
    s.queued_save.pending = true;
    s.queued_save.frame = s.ops.frame[s.iteration];
    s.queued_save.checksum = s.ops.checksum[s.iteration];
  }
  return true;
}

bool OnLoopEnd(const Core::CPUThreadGuard& guard)
{
  if (!IsArmed())
    return false;
  std::lock_guard lk(s.mutex);
  auto& ppc = guard.GetSystem().GetPPCState();
  if (s.phase == Phase::Running && s.ops.count > 0)
  {
    s.iteration++;
    if (s.iteration >= s.ops.count)
      s.iteration = 0;
  }
  ppc.npc = BRAWL_LOOP_TOP;
  return true;
}

picojson::value Status()
{
  std::unique_lock lk(s.mutex, std::defer_lock);
  (void)lk.try_lock_for(std::chrono::milliseconds(200));
  picojson::object o;
  o["phase"] = picojson::value(PhaseName(s.phase));
  o["armed"] = picojson::value(Active());
  if (!lk.owns_lock())
  {
    o["busy"] = picojson::value(true);
    return picojson::value(o);
  }
  o["error"] = picojson::value(s.error);
  o["slot"] = picojson::value(static_cast<double>(s.opts.local_slot));
  o["players"] = picojson::value(static_cast<double>(s.players));
  o["frame"] = picojson::value(static_cast<double>(s.current_frame));
  o["rollbacks"] = picojson::value(static_cast<double>(s.rollbacks));
  o["max_rollback"] = picojson::value(static_cast<double>(s.max_rollback));
  o["frames_resimulated"] = picojson::value(static_cast<double>(s.frames_resimulated));
  o["stall_polls"] = picojson::value(static_cast<double>(s.stall_polls));
  o["skipped_loads"] = picojson::value(static_cast<double>(s.skipped_loads));
  o["saves"] = picojson::value(static_cast<double>(s.saves));
  o["desyncs"] = picojson::value(static_cast<double>(s.desyncs));
  o["journal_errors"] = picojson::value(static_cast<double>(s.journal_errors));
  o["last_desync_frame"] = picojson::value(static_cast<double>(s.last_desync_frame));
  o["frames_ahead"] = picojson::value(static_cast<double>(s.frames_ahead));
  o["speed"] = picojson::value(s.speed);
  o["synctest"] = picojson::value(static_cast<double>(s.opts.synctest));
  picojson::array peers;
  {
    std::lock_guard nlk(s.net_mutex);
    for (const PeerLink& p : s.peers)
    {
      if (p.slot < 0)
        continue;
      picojson::object po;
      po["slot"] = picojson::value(static_cast<double>(p.slot));
      po["connected"] = picojson::value(p.hello);
      po["started"] = picojson::value(p.started);
      po["left"] = picojson::value(p.left);
      po["rtt_ms"] = picojson::value(p.rtt_ms);
      peers.emplace_back(po);
    }
  }
  o["peers"] = picojson::value(peers);
  picojson::array log;
  for (const auto& l : s.log)
    log.emplace_back(l);
  o["log"] = picojson::value(log);
  picojson::array diag;
  for (const auto& l : s_diag)
    diag.emplace_back(l);
  o["diag"] = picojson::value(diag);
  o["diag_frames"] = picojson::value(static_cast<double>(s_diag_frames));
  const auto& rb = Rollback::RollbackManager::Get();
  const auto t = rb.GetTimingStats();
  o["save_us_avg"] = picojson::value(t.save_count ? double(t.save_us_total) / t.save_count : 0.0);
  o["load_us_avg"] = picojson::value(t.load_count ? double(t.load_us_total) / t.load_count : 0.0);
  o["save_us_max"] = picojson::value(static_cast<double>(t.save_us_max));
  o["load_us_max"] = picojson::value(static_cast<double>(t.load_us_max));
  return picojson::value(o);
}

picojson::value Digest(s64 frame)
{
  std::lock_guard lk(s.mutex);
  picojson::object o;
  if (frame < 0)
    return picojson::value(o);
  const FrameDigest& d = s_digests[static_cast<size_t>(frame) % s_digests.size()];
  if (d.frame != frame)
    return picojson::value(o);
  o["frame"] = picojson::value(static_cast<double>(frame));
  o["checksum"] = picojson::value(static_cast<double>(d.checksum));
  std::string text;
  for (const auto& [addr, crc] : d.granules)
    text += fmt::format("{:08x}:{:08x} ", addr, crc);
  o["granules"] = picojson::value(text);
  return picojson::value(o);
}

picojson::value History(s64 since)
{
  std::lock_guard lk(s.mutex);
  picojson::object frames;
  const s64 newest = s.current_frame;
  // Frames GekkoNet may still roll back are not final yet.
  const s64 upto = newest - MAX_ROLLBACK_FRAMES - 1;
  for (s64 f = std::max<s64>(since, upto - static_cast<s64>(HISTORY_SIZE) + 1); f <= upto; ++f)
  {
    if (f < 0)
      continue;
    const HistoryEntry& e = s.history[static_cast<size_t>(f) % HISTORY_SIZE];
    if (e.frame != f)
      continue;
    picojson::array row;
    for (const Input& in : e.inputs)
    {
      std::string hex;
      const int n = (in[0] & INPUT_WIIMOTE_CONNECTED) ? 10 + std::min<int>(in[9], 31) : 9;
      for (int i = 0; i < n; ++i)
        hex += fmt::format("{:02x}", in[i]);
      row.emplace_back(hex);
    }
    row.emplace_back(static_cast<double>(e.checksum));
    frames[std::to_string(f)] = picojson::value(row);
  }
  picojson::object o;
  o["frames"] = picojson::value(frames);
  return picojson::value(o);
}
}  // namespace Machine
