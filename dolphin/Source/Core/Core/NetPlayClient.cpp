// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlayClient.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <xxhash.h>

#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/ENet.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/QoSSession.h"
#include "Common/SFMLHelper.h"
#include "Common/Timer.h"
#include "Common/Version.h"

#include "Core/ActionReplay.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DVD/DVDThread.h"
#include "Core/GeckoCode.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SI/SI_DeviceAMBaseboard.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "Core/HW/Sram.h"
#include "Core/HW/WiiSave.h"
#include "Core/HW/WiiSaveStructs.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/Uids.h"
#include "Core/Movie.h"
#include "Core/NetPlayCommon.h"
#include "Core/Online/Timeouts.h"
#include "Core/SyncIdentifier.h"
#include "Core/System.h"
#include "DiscIO/Blob.h"

#include "InputCommon/GCAdapter.h"
#include "UICommon/GameFile.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoEvents.h"

#include "Rollback/RollbackManager.h"
#include "Core/Harness/Harness.h"

// Brawlback constants
#include "Brawlback/include/brawlback-common/BrawlbackConstants.h"

// GekkoNet integration
#include "Brawlback/include/gekkonet/GekkoLib/include/gekkonet.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "InputCommon/GCPadStatus.h"

namespace NetPlay
{
using namespace WiimoteCommon;

static std::mutex crit_netplay_client;
static NetPlayClient* netplay_client = nullptr;
static bool s_si_poll_batching = false;
static std::atomic<bool> s_gekko_cpu_stalled{false};

// GekkoNet adapter static instance
static NetPlayClient* s_gekko_client_instance = nullptr;

// GekkoNet adapter callbacks
void GekkoNetAdapter_SendData(GekkoNetAddress* address, const char* data, int length)
{
  if (!s_gekko_client_instance || !s_gekko_client_instance->m_server)
    return;

  // Called from the CPU thread (inside gekko_network_poll/gekko_update_session). ENet is not
  // thread-safe and the netplay thread is inside enet_host_service on the same host, so never
  // touch ENet here: queue the packet and wake the netplay thread, which sends and flushes it
  // right away (otherwise the packet also sits in ENet's queue until the next received event).
  {
    std::lock_guard<std::mutex> lock(s_gekko_client_instance->m_gekko_send_mutex);
    s_gekko_client_instance->m_gekko_send_queue.emplace_back(data, static_cast<size_t>(length));
  }
  Common::ENet::WakeupThread(s_gekko_client_instance->m_client);
}

// called from ---NETPLAY--- thread
void NetPlayClient::FlushGekkoSendQueue()
{
  std::vector<GekkoNetPacket> packets;
  {
    std::lock_guard<std::mutex> lock(m_gekko_send_mutex);
    packets.swap(m_gekko_send_queue);
  }
  if (packets.empty() || !m_server)
    return;

  for (const GekkoNetPacket& pkt : packets)
  {
    ENetPacket* packet =
        enet_packet_create(pkt.data.data(), pkt.data.size(), ENET_PACKET_FLAG_UNSEQUENCED);
    if (!packet)
      continue;
    if (enet_peer_send(m_server, GEKKONET_CHANNEL, packet) < 0)
      enet_packet_destroy(packet);
  }
  enet_host_flush(m_client);
}

GekkoNetResult** GekkoNetAdapter_ReceiveData(int* length)
{
  if (!s_gekko_client_instance)
  {
    if (length != nullptr)
      *length = 0;
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(s_gekko_client_instance->m_gekko_packet_mutex);

  s_gekko_client_instance->m_gekko_packet_ptrs.clear();

  // Check if length parameter is valid before dereferencing
  if (length == nullptr)
  {
    return reinterpret_cast<GekkoNetResult**>(s_gekko_client_instance->m_gekko_packet_ptrs.data());
  }

  if (s_gekko_client_instance->m_gekko_received_packets.empty())
  {
    *length = 0;
    return reinterpret_cast<GekkoNetResult**>(s_gekko_client_instance->m_gekko_packet_ptrs.data());
  }

  s_gekko_client_instance->m_gekko_packet_ptrs.reserve(s_gekko_client_instance->m_gekko_received_packets.size());

  for (auto& pkt : s_gekko_client_instance->m_gekko_received_packets)
  {
    // GekkoNet's internal MessageSystem::HandleData() calls free_data() on
    // result->addr.data, result->data, and result itself for every packet it
    // processes. All memory handed to GekkoNet must therefore be allocated
    // (and later freed) using the same allocator so the free_data callback
    // can safely release it - mirroring the reference asio_receive/asio_free
    // implementation, which uses malloc/free throughout.
    auto* result = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    if (result == nullptr)
    {
      break;
    }

    result->data_len = static_cast<unsigned int>(pkt.data.size());
    result->data = std::malloc(pkt.data.size());

    // Provide the remote address (as an owned copy) so GekkoNet can match
    // packets to the remote actor during handshake. The address buffer must
    // be independently allocated since GekkoNet will free_data() it.
    const std::string& remote_addr = s_gekko_client_instance->m_gekko_remote_addr;
    if (!remote_addr.empty())
    {
      result->addr.data = std::malloc(remote_addr.size());
      result->addr.size = static_cast<unsigned int>(remote_addr.size());
    }
    else
    {
      result->addr.data = nullptr;
      result->addr.size = 0;
    }

    // Check if any allocation failed and cleanup if so
    if (result->data == nullptr || (!remote_addr.empty() && result->addr.data == nullptr))
    {
      std::free(result->addr.data);
      std::free(result->data);
      std::free(result);
      break;
    }

    std::memcpy(result->data, pkt.data.data(), pkt.data.size());
    if (!remote_addr.empty())
    {
      std::memcpy(result->addr.data, remote_addr.data(), remote_addr.size());
    }

    s_gekko_client_instance->m_gekko_packet_ptrs.push_back(result);
  }

  s_gekko_client_instance->m_gekko_received_packets.clear();

  *length = static_cast<int>(s_gekko_client_instance->m_gekko_packet_ptrs.size());
  return reinterpret_cast<GekkoNetResult**>(s_gekko_client_instance->m_gekko_packet_ptrs.data());
}

void GekkoNetAdapter_FreeData(void* data)
{
  if (!data)
    return;

  // Guard against callback being invoked when netplay session is not active.
  // This can happen if GekkoNet tries to clean up after the session is destroyed.
  if (!s_gekko_client_instance)
    return;

  // Must match the allocator used in GekkoNetAdapter_ReceiveData (malloc).
  std::free(data);
}

// called from ---GUI--- thread
NetPlayClient::~NetPlayClient()
{
  // not perfect
  if (m_is_running.IsSet())
    StopGame();

  if (m_is_connected)
  {
    m_should_compute_game_digest = false;
    m_dialog->AbortGameDigest();
    if (m_game_digest_thread.joinable())
      m_game_digest_thread.join();
    m_do_loop.Clear();
    m_thread.join();

    m_chunked_data_receive_queue.clear();
    m_dialog->HideChunkedProgressDialog();
  }

  if (m_server)
  {
    Disconnect();
  }

  // Clean up GekkoNet session
  DestroyGekkoSession();

  if (Common::g_MainNetHost.get() == m_client)
  {
    Common::g_MainNetHost.release();
  }
  if (m_client)
  {
    enet_host_destroy(m_client);
    m_client = nullptr;
  }

  if (m_traversal_client)
  {
    Common::ReleaseTraversalClient();
  }
}

// called from ---GUI--- thread
NetPlayClient::NetPlayClient(const std::string& address, const u16 port, NetPlayUI* dialog,
                             std::string name, const NetTraversalConfig& traversal_config,
                             u16 local_port)
    : m_dialog(dialog), m_player_name(std::move(name))
{
  ClearBuffers();

  if (!traversal_config.use_traversal)
  {
    // Direct Connection
    ENetAddress local_addr{};
    local_addr.host = ENET_HOST_ANY;
    local_addr.port = local_port;
    m_client = enet_host_create(local_port ? &local_addr : nullptr, 1, CHANNEL_COUNT, 0, 0);

    if (m_client == nullptr)
    {
      m_dialog->OnConnectionError(_trans("Could not create client."));
      return;
    }

    m_client->mtu = std::min(m_client->mtu, NetPlay::MAX_ENET_MTU);

    ENetAddress addr;
    enet_address_set_host(&addr, address.c_str());
    addr.port = port;

    m_server = enet_host_connect(m_client, &addr, CHANNEL_COUNT, 0);

    if (m_server == nullptr)
    {
      m_dialog->OnConnectionError(_trans("Could not create peer."));
      return;
    }

    // Update time in milliseconds of no acknowledgment of
    // sent packets before a connection is deemed disconnected
    enet_peer_timeout(m_server, 0, PEER_TIMEOUT.count(), PEER_TIMEOUT.count());

    ENetEvent netEvent;
    int net = enet_host_service(m_client, &netEvent, 5000);
    if (net > 0 && netEvent.type == ENET_EVENT_TYPE_CONNECT)
    {
      if (Connect())
      {
        m_client->intercept = Common::ENet::InterceptCallback;
        m_thread = std::thread(&NetPlayClient::ThreadFunc, this);
      }
    }
    else
    {
      m_dialog->OnConnectionError(_trans("Could not communicate with host."));
    }
  }
  else
  {
    if (address.size() > Common::NETPLAY_CODE_SIZE)
    {
      m_dialog->OnConnectionError(
          _trans("The host code is too long.\nPlease recheck that you have the correct code."));
      return;
    }

    if (!Common::EnsureTraversalClient(traversal_config.traversal_host,
                                       traversal_config.traversal_port))
    {
      return;
    }
    m_client = Common::g_MainNetHost.get();

    m_traversal_client = Common::g_TraversalClient.get();

    // If we were disconnected in the background, reconnect.
    if (m_traversal_client->HasFailed())
      m_traversal_client->ReconnectToServer();
    m_traversal_client->m_Client = this;
    m_host_spec = address;
    m_connection_state = ConnectionState::WaitingForTraversalClientConnection;
    OnTraversalStateChanged();
    m_connecting = true;

    Common::Timer connect_timer;
    connect_timer.Start();

    while (m_connecting)
    {
      ENetEvent netEvent;
      if (m_traversal_client)
        m_traversal_client->HandleResends();

      while (enet_host_service(m_client, &netEvent, 4) > 0)
      {
        sf::Packet rpac;
        switch (netEvent.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
          m_server = netEvent.peer;

          // Update time in milliseconds of no acknowledgment of
          // sent packets before a connection is deemed disconnected
          enet_peer_timeout(m_server, 0, PEER_TIMEOUT.count(), PEER_TIMEOUT.count());

          if (Connect())
          {
            m_connection_state = ConnectionState::Connected;
            m_thread = std::thread(&NetPlayClient::ThreadFunc, this);
          }
          return;
        default:
          break;
        }
      }
      if (connect_timer.ElapsedMs() > 5000)
        break;
    }
    m_dialog->OnConnectionError(_trans("Could not communicate with host."));
  }
}

bool NetPlayClient::Connect()
{
  INFO_LOG_FMT(NETPLAY, "Connecting to server.");

  // send connect message
  sf::Packet packet;
  packet << Common::GetScmRevGitStr();
  packet << Common::GetNetplayDolphinVer();
  packet << m_player_name;
  Send(packet);
  enet_host_flush(m_client);
  sf::Packet rpac;
  // TODO: make this not hang
  ENetEvent netEvent;
  int net;
  while ((net = enet_host_service(m_client, &netEvent, 5000)) > 0 &&
         static_cast<int>(netEvent.type) == Common::ENet::SKIPPABLE_EVENT)
  {
    // ignore packets from traversal server
  }
  if (net > 0 && netEvent.type == ENET_EVENT_TYPE_RECEIVE)
  {
    rpac.append(netEvent.packet->data, netEvent.packet->dataLength);
    enet_packet_destroy(netEvent.packet);
  }
  else
  {
    return false;
  }

  ConnectionError error;
  rpac >> error;

  // got error message
  if (error != ConnectionError::NoError)
  {
    switch (error)
    {
    case ConnectionError::ServerFull:
      m_dialog->OnConnectionError(_trans("The server is full."));
      break;
    case ConnectionError::VersionMismatch:
      m_dialog->OnConnectionError(
          _trans("The server and client's NetPlay versions are incompatible."));
      break;
    case ConnectionError::GameRunning:
      m_dialog->OnConnectionError(_trans("The game is currently running."));
      break;
    case ConnectionError::NameTooLong:
      m_dialog->OnConnectionError(_trans("Nickname is too long."));
      break;
    default:
      m_dialog->OnConnectionError(_trans("The server sent an unknown error message."));
      break;
    }

    Disconnect();
    return false;
  }
  else
  {
    rpac >> m_pid;

    Player player;
    player.name = m_player_name;
    player.pid = m_pid;
    player.revision = Common::GetNetplayDolphinVer();
    player.buffer = 0;

    // add self to player list
    m_players[m_pid] = player;
    m_local_player = &m_players[m_pid];

    m_dialog->Update();

    m_is_connected = true;

    return true;
  }
}

static void ReceiveSyncIdentifier(sf::Packet& spac, SyncIdentifier& sync_identifier)
{
  // We use a temporary variable here due to a potential long vs long long mismatch
  u64 dol_elf_size;
  spac >> dol_elf_size;
  sync_identifier.dol_elf_size = dol_elf_size;

  spac >> sync_identifier.game_id;
  spac >> sync_identifier.revision;
  spac >> sync_identifier.disc_number;
  spac >> sync_identifier.is_datel;

  for (u8& x : sync_identifier.sync_hash)
    spac >> x;
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnData(sf::Packet& packet)
{
  MessageID mid;
  packet >> mid;

  INFO_LOG_FMT(NETPLAY, "Got server message: {:x}", static_cast<u8>(mid));

  switch (mid)
  {
  case MessageID::PlayerJoin:
    OnPlayerJoin(packet);
    break;

  case MessageID::PlayerLeave:
    OnPlayerLeave(packet);
    break;

  case MessageID::ChatMessage:
    OnChatMessage(packet);
    break;

  case MessageID::ChunkedDataStart:
    OnChunkedDataStart(packet);
    break;

  case MessageID::ChunkedDataEnd:
    OnChunkedDataEnd(packet);
    break;

  case MessageID::ChunkedDataPayload:
    OnChunkedDataPayload(packet);
    break;

  case MessageID::ChunkedDataAbort:
    OnChunkedDataAbort(packet);
    break;

  case MessageID::PadMapping:
    OnPadMapping(packet);
    break;

  case MessageID::GBAConfig:
    OnGBAConfig(packet);
    break;

  case MessageID::WiimoteMapping:
    OnWiimoteMapping(packet);
    break;

  case MessageID::PadData:
    OnPadData(packet);
    break;

  case MessageID::PadHostData:
    OnPadHostData(packet);
    break;

  case MessageID::WiimoteData:
    OnWiimoteData(packet);
    break;

  case MessageID::PadBufferMinimum:
    OnPadBufferMinimum(packet);
    break;

  case MessageID::PadBufferPlayer:
    OnPadBufferPlayer(packet);
    break;

  case MessageID::HostInputAuthority:
    OnHostInputAuthority(packet);
    break;

  case MessageID::RollbackDelay:
    OnRollbackDelay(packet);
    break;

  case MessageID::GolfSwitch:
    OnGolfSwitch(packet);
    break;

  case MessageID::GolfPrepare:
    OnGolfPrepare(packet);
    break;

  case MessageID::ChangeGame:
    OnChangeGame(packet);
    break;

  case MessageID::GameStatus:
    OnGameStatus(packet);
    break;

  case MessageID::StartGame:
    OnStartGame(packet);
    break;

  case MessageID::StopGame:
  case MessageID::DisableGame:
    OnStopGame(packet);
    break;

  case MessageID::PowerButton:
    OnPowerButton();
    break;

  case MessageID::Ping:
    OnPing(packet);
    break;

  case MessageID::PlayerPingData:
    OnPlayerPingData(packet);
    break;

  case MessageID::DesyncDetected:
    OnDesyncDetected(packet);
    break;

  case MessageID::SyncSaveData:
    OnSyncSaveData(packet);
    break;

  case MessageID::SyncCodes:
    OnSyncCodes(packet);
    break;

  case MessageID::ComputeGameDigest:
    OnComputeGameDigest(packet);
    break;

  case MessageID::GameDigestProgress:
    OnGameDigestProgress(packet);
    break;

  case MessageID::GameDigestResult:
    OnGameDigestResult(packet);
    break;

  case MessageID::GameDigestError:
    OnGameDigestError(packet);
    break;

  case MessageID::GameDigestAbort:
    OnGameDigestAbort();
    break;

  default:
    PanicAlertFmtT("Unknown message received with id : {0}", static_cast<u8>(mid));
    break;
  }
}

void NetPlayClient::OnPlayerJoin(sf::Packet& packet)
{
  Player player{};
  packet >> player.pid;
  packet >> player.name;
  packet >> player.revision;

  INFO_LOG_FMT(NETPLAY, "Player {} ({}) using {} joined", player.name, player.pid, player.revision);

  {
    std::lock_guard lkp(m_crit.players);
    m_players[player.pid] = player;
  }

  m_dialog->OnPlayerConnect(player.name);

  m_dialog->Update();
}

void NetPlayClient::OnPlayerLeave(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lkp(m_crit.players);
    const auto it = m_players.find(pid);
    if (it == m_players.end())
      return;

    const auto& player = it->second;
    INFO_LOG_FMT(NETPLAY, "Player {} ({}) left", player.name, pid);
    m_dialog->OnPlayerDisconnect(player.name);
    m_players.erase(it);
  }

  m_dialog->Update();
}

void NetPlayClient::OnChatMessage(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;
  std::string msg;
  packet >> msg;

  // don't need lock to read in this thread
  const Player& player = m_players[pid];

  INFO_LOG_FMT(NETPLAY, "Player {} ({}) wrote: {}", player.name, player.pid, msg);

  // add to gui
  m_dialog->AppendChat(fmt::format("{}[{}]: {}", player.name, pid, msg));
}

void NetPlayClient::OnChunkedDataStart(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;
  std::string title;
  packet >> title;
  const u64 data_size = Common::PacketReadU64(packet);

  INFO_LOG_FMT(NETPLAY, "Starting data chunk {}.", cid);

  m_chunked_data_receive_queue.emplace(cid, sf::Packet{});

  std::vector<int> players;
  players.push_back(m_local_player->pid);
  m_dialog->ShowChunkedProgressDialog(title, data_size, players);
}

void NetPlayClient::OnChunkedDataEnd(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;

  const auto data_packet_iter = m_chunked_data_receive_queue.find(cid);
  if (data_packet_iter == m_chunked_data_receive_queue.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", cid);
    return;
  }

  INFO_LOG_FMT(NETPLAY, "Ending data chunk {}.", cid);

  auto& data_packet = data_packet_iter->second;
  OnData(data_packet);
  m_chunked_data_receive_queue.erase(data_packet_iter);
  m_dialog->HideChunkedProgressDialog();

  sf::Packet complete_packet;
  complete_packet << MessageID::ChunkedDataComplete;
  complete_packet << cid;
  Send(complete_packet, CHUNKED_DATA_CHANNEL);
}

void NetPlayClient::OnChunkedDataPayload(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;

  const auto data_packet_iter = m_chunked_data_receive_queue.find(cid);
  if (data_packet_iter == m_chunked_data_receive_queue.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", cid);
    return;
  }

  auto& data_packet = data_packet_iter->second;
  while (!packet.endOfPacket())
  {
    u8 byte;
    packet >> byte;
    data_packet << byte;
  }

  INFO_LOG_FMT(NETPLAY, "Received {} bytes of data chunk {}.", data_packet.getDataSize(), cid);

  m_dialog->SetChunkedProgress(m_local_player->pid, data_packet.getDataSize());

  sf::Packet progress_packet;
  progress_packet << MessageID::ChunkedDataProgress;
  progress_packet << cid;
  progress_packet << u64{data_packet.getDataSize()};
  Send(progress_packet, CHUNKED_DATA_CHANNEL);
}

void NetPlayClient::OnChunkedDataAbort(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;

  const auto iter = m_chunked_data_receive_queue.find(cid);
  if (iter == m_chunked_data_receive_queue.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", cid);
    return;
  }

  INFO_LOG_FMT(NETPLAY, "Aborting data chunk {}.", cid);

  m_chunked_data_receive_queue.erase(iter);
  m_dialog->HideChunkedProgressDialog();
}

void NetPlayClient::OnPadMapping(sf::Packet& packet)
{
  for (PlayerId& mapping : m_net_settings.pad_map)
    packet >> mapping;

  m_dialog->Update();
}

void NetPlayClient::OnWiimoteMapping(sf::Packet& packet)
{
  for (PlayerId& mapping : m_net_settings.wiimote_map)
    packet >> mapping;

  m_dialog->Update();
}

void NetPlayClient::OnGBAConfig(sf::Packet& packet)
{
  for (size_t i = 0; i < m_net_settings.gba_config.size(); ++i)
  {
    auto& config = m_net_settings.gba_config[i];
    const auto old_config = config;

    packet >> config.enabled >> config.has_rom >> config.title;
    for (auto& data : config.hash)
      packet >> data;

    if (std::tie(config.has_rom, config.title, config.hash) !=
        std::tie(old_config.has_rom, old_config.title, old_config.hash))
    {
      m_dialog->OnMsgChangeGBARom(static_cast<int>(i), config);
      m_net_settings.gba_rom_paths[i] =
          config.has_rom ?
              m_dialog->FindGBARomPath(config.hash, config.title, static_cast<int>(i)) :
              "";
    }
  }

  SendGameStatus();

  m_dialog->Update();
}

void NetPlayClient::OnPadData(sf::Packet& packet)
{
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    GCPadStatus pad;
    packet >> pad.button;
    if (static_cast<size_t>(map) < m_net_settings.gba_config.size() &&
        !m_net_settings.gba_config.at(map).enabled)
    {
      packet >> pad.analogA >> pad.analogB >> pad.stickX >> pad.stickY >> pad.substickX >>
          pad.substickY >> pad.triggerLeft >> pad.triggerRight >> pad.isConnected;
    }
    if (m_net_settings.m_RollbackMode)
    {
      // Rollback mode should only be used with GekkoNet
      // GekkoNet does NOT use PadData packets - it receives inputs via GekkoNet session
      // advance events (processed in HandleGekkoFrame) and stores them in
      // m_gekko_pending_ops.adv_pads for injection during OnResimFrame.

      // If we get here, something is wrong with the network protocol
      static int s_rollback_paddata_warning_counter = 0;
      if (++s_rollback_paddata_warning_counter % 60 == 0)
      {
        WARN_LOG_FMT(NETPLAY, 
          "OnPadData called in rollback mode - this should not happen! (map={})", map);
      }
      return; // Ignore the packet
    }
    else if (static_cast<size_t>(map) < m_pad_buffer.size())
    {
      m_pad_buffer.at(map).Push(pad);
      m_gc_pad_event.Set();
    }
  }
}

void NetPlayClient::OnPadHostData(sf::Packet& packet)
{
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    GCPadStatus pad;
    packet >> pad.button;
    if (static_cast<size_t>(map) < m_net_settings.gba_config.size() &&
        !m_net_settings.gba_config.at(map).enabled)
    {
      packet >> pad.analogA >> pad.analogB >> pad.stickX >> pad.stickY >> pad.substickX >>
          pad.substickY >> pad.triggerLeft >> pad.triggerRight >> pad.isConnected;
    }

    if (static_cast<size_t>(map) < m_last_pad_status.size())
      m_last_pad_status[map] = pad;

    if (static_cast<size_t>(map) < m_first_pad_status_received.size())
    {
      if (!m_first_pad_status_received[map])
      {
        m_first_pad_status_received[map] = true;
        m_first_pad_status_received_event.Set();
      }
    }
  }
}

void NetPlayClient::OnWiimoteData(sf::Packet& packet)
{
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    WiimoteEmu::SerializedWiimoteState pad;
    packet >> pad.length;
    ASSERT(pad.length <= pad.data.size());
    if (pad.length <= pad.data.size())
    {
      for (size_t i = 0; i < pad.length; ++i)
        packet >> pad.data[i];
    }
    else
    {
      pad.length = 0;
    }

    if (static_cast<size_t>(map) < m_wiimote_buffer.size())
    {
      m_wiimote_buffer.at(map).Push(pad);
      m_wii_pad_event.Set();
    }
  }
}

void NetPlayClient::OnHostInputAuthority(sf::Packet& packet)
{
  packet >> m_host_input_authority;
  m_dialog->OnHostInputAuthorityChanged(m_host_input_authority);
}

void NetPlayClient::OnRollbackDelay(sf::Packet& packet)
{
  u32 delay = ROLLBACK_DELAY_DEFAULT;
  packet >> delay;
  m_announced_rollback_delay = std::clamp(delay, ROLLBACK_DELAY_MIN, ROLLBACK_DELAY_MAX);
  m_dialog->OnRollbackDelayChanged(m_announced_rollback_delay);
}

void NetPlayClient::OnGolfSwitch(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  const PlayerId previous_golfer = m_current_golfer;
  m_current_golfer = pid;
  m_dialog->OnGolferChanged(m_local_player->pid == pid, pid != 0 ? m_players[pid].name : "");

  if (m_local_player->pid == previous_golfer)
  {
    sf::Packet spac;
    spac << MessageID::GolfRelease;
    Send(spac);
  }
  else if (m_local_player->pid == pid)
  {
    sf::Packet spac;
    spac << MessageID::GolfAcquire;
    Send(spac);

    // Pads are already calibrated so we can just ignore this
    m_first_pad_status_received.fill(true);

    m_wait_on_input = false;
    m_wait_on_input_event.Set();
  }
}

void NetPlayClient::OnGolfPrepare(sf::Packet& packet)
{
  m_wait_on_input_received = true;
  m_wait_on_input = true;
}

void NetPlayClient::OnChangeGame(sf::Packet& packet)
{
  std::string netplay_name;
  {
    std::lock_guard lkg(m_crit.game);
    ReceiveSyncIdentifier(packet, m_selected_game);
    packet >> netplay_name;
  }

  INFO_LOG_FMT(NETPLAY, "Game changed to {}", netplay_name);

  // update gui
  m_dialog->OnMsgChangeGame(m_selected_game, netplay_name);

  SendGameStatus();

  sf::Packet client_capabilities_packet;
  client_capabilities_packet << MessageID::ClientCapabilities;
  client_capabilities_packet << ExpansionInterface::CEXIIPL::HasIPLDump();
  client_capabilities_packet << Config::Get(Config::SESSION_USE_FMA);
  Send(client_capabilities_packet);
}

void NetPlayClient::OnGameStatus(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lkp(m_crit.players);
    packet >> m_players[pid].game_status;
  }

  m_dialog->Update();
}

void NetPlayClient::OnStartGame(sf::Packet& packet)
{
  {
    std::lock_guard lkg(m_crit.game);

    INFO_LOG_FMT(NETPLAY, "Start of game {}", m_selected_game.game_id);

    packet >> m_current_game;
    packet >> m_net_settings.cpu_thread;
    packet >> m_net_settings.cpu_core;
    packet >> m_net_settings.enable_cheats;
    packet >> m_net_settings.enable_hardcore;
    packet >> m_net_settings.selected_language;
    packet >> m_net_settings.override_region_settings;
    packet >> m_net_settings.dsp_enable_jit;
    packet >> m_net_settings.dsp_hle;
    packet >> m_net_settings.ram_override_enable;
    packet >> m_net_settings.mem1_size;
    packet >> m_net_settings.mem2_size;
    packet >> m_net_settings.fallback_region;
    packet >> m_net_settings.allow_sd_writes;
    packet >> m_net_settings.oc_enable;
    packet >> m_net_settings.oc_factor;
    packet >> m_net_settings.vi_oc_enable;
    packet >> m_net_settings.vi_oc_factor;
    packet >> m_net_settings.spectator_mode;

    for (auto slot : ExpansionInterface::SLOTS)
      packet >> m_net_settings.exi_device[slot];

    packet >> m_net_settings.memcard_size_override;

    for (u32& value : m_net_settings.sysconf_settings)
      packet >> value;

    packet >> m_net_settings.efb_access_enable;
    packet >> m_net_settings.bbox_enable;
    packet >> m_net_settings.force_progressive;
    packet >> m_net_settings.efb_to_texture_enable;
    packet >> m_net_settings.xfb_to_texture_enable;
    packet >> m_net_settings.disable_copy_to_vram;
    packet >> m_net_settings.immediate_xfb_enable;
    packet >> m_net_settings.efb_emulate_format_changes;
    packet >> m_net_settings.safe_texture_cache_color_samples;
    packet >> m_net_settings.perf_queries_enable;
    packet >> m_net_settings.float_exceptions;
    packet >> m_net_settings.divide_by_zero_exceptions;
    packet >> m_net_settings.fprf;
    packet >> m_net_settings.accurate_nans;
    packet >> m_net_settings.disable_icache;
    packet >> m_net_settings.sync_on_skip_idle;
    packet >> m_net_settings.sync_gpu;
    packet >> m_net_settings.sync_gpu_max_distance;
    packet >> m_net_settings.sync_gpu_min_distance;
    packet >> m_net_settings.sync_gpu_overclock;
    packet >> m_net_settings.jit_follow_branch;
    packet >> m_net_settings.fast_disc_speed;
    packet >> m_net_settings.mmu;
    packet >> m_net_settings.fastmem;
    packet >> m_net_settings.skip_ipl;
    packet >> m_net_settings.load_ipl_dump;
    packet >> m_net_settings.vertex_rounding;
    packet >> m_net_settings.internal_resolution;
    packet >> m_net_settings.efb_scaled_copy;
    packet >> m_net_settings.fast_depth_calc;
    packet >> m_net_settings.enable_pixel_lighting;
    packet >> m_net_settings.widescreen_hack;
    packet >> m_net_settings.force_texture_filtering;
    packet >> m_net_settings.max_anisotropy;
    packet >> m_net_settings.force_true_color;
    packet >> m_net_settings.disable_copy_filter;
    packet >> m_net_settings.disable_fog;
    packet >> m_net_settings.arbitrary_mipmap_detection;
    packet >> m_net_settings.arbitrary_mipmap_detection_threshold;
    packet >> m_net_settings.enable_gpu_texture_decoding;
    packet >> m_net_settings.defer_efb_copies;
    packet >> m_net_settings.efb_access_tile_size;
    packet >> m_net_settings.efb_access_defer_invalidation;
    packet >> m_net_settings.savedata_load;
    packet >> m_net_settings.savedata_write;
    packet >> m_net_settings.savedata_sync_all_wii;
    if (!m_net_settings.savedata_load)
    {
      m_net_settings.savedata_write = false;
      m_net_settings.savedata_sync_all_wii = false;
    }
    packet >> m_net_settings.strict_settings_sync;

    m_initial_rtc = Common::PacketReadU64(packet);

    packet >> m_net_settings.save_data_region;
    packet >> m_net_settings.sync_codes;
    packet >> m_net_settings.golf_mode;
    packet >> m_net_settings.use_fma;
    packet >> m_net_settings.hide_remote_gbas;
    packet >> m_net_settings.m_RollbackMode;
    packet >> m_net_settings.rollback_delay;
    m_net_settings.rollback_delay =
        std::clamp(m_net_settings.rollback_delay, ROLLBACK_DELAY_MIN, ROLLBACK_DELAY_MAX);

    for (size_t i = 0; i < sizeof(m_net_settings.sram); ++i)
      packet >> m_net_settings.sram[i];

    m_net_settings.is_hosting = m_local_player->IsHost();
  }

  // Original rollback mode variables removed - GekkoNet handles input storage internally
  current_frame = 0;

  // Initialize GekkoNet for rollback mode
  if (m_net_settings.m_RollbackMode &&
      (m_players.size() == 2 || (m_players.size() == 1 && SyncTestDistance() > 0)) &&
      m_local_player && m_is_connected)
  {
    // In rollback mode with 2 players, use GekkoNet for networking
    // The "remote address" is just an identifier since we're using a custom adapter
    // that tunnels through the existing ENet connection

    // Differentiate between host and guest for proper P2P session setup
    const bool is_host = m_local_player->IsHost();
    std::string remote_addr;
    unsigned short local_port;

    if (m_server && m_client)
    {
      // Get remote peer's IP address and port from m_server (ENetPeer)
      char remote_ip[256];
      if (enet_address_get_host_ip(&m_server->address, remote_ip, sizeof(remote_ip)) == 0)
      {
        std::string remote_ip_str(remote_ip);
        remote_addr = fmt::format("{}:{}", remote_ip_str, m_server->address.port);
      }
      else
      {
        remote_addr = is_host ? "gekkonet://guest:12346" : "gekkonet://host:12345";
      }

      // Get local port from m_client (ENetHost)
      // For local testing, ensure we're using the actual bound port
      local_port = m_client->address.port;

      // If port is 0, ENet chose a random port - try to get the actual bound port
      if (local_port == 0 && m_client->socket != -1)
      {
        ENetAddress bound_addr;
        if (enet_socket_get_address(m_client->socket, &bound_addr) == 0)
        {
          local_port = bound_addr.port;
        }
      }

      }
    else
    {
        // Fallback to placeholder values if connection info not available
      if (is_host)
      {
        remote_addr = "gekkonet://guest:12346";
        local_port = 12345;
      }
      else
      {
        remote_addr = "gekkonet://host:12345";
        local_port = 12346;
      }
    }

    InitGekkoSession(remote_addr, local_port, is_host);
  }

  m_dialog->OnMsgStartGame();
}

void NetPlayClient::OnStopGame(sf::Packet& packet)
{
  INFO_LOG_FMT(NETPLAY, "Game stopped");

  StopGame();
  m_dialog->OnMsgStopGame();
}

void NetPlayClient::OnPowerButton()
{
  InvokeStop();
  m_dialog->OnMsgPowerButton();
}

void NetPlayClient::OnPing(sf::Packet& packet)
{
  u32 ping_key = 0;
  packet >> ping_key;

  sf::Packet response_packet;
  response_packet << MessageID::Pong;
  response_packet << ping_key;

  Send(response_packet);
}

void NetPlayClient::OnPlayerPingData(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lkp(m_crit.players);
    Player& player = m_players[pid];
    packet >> player.ping;
  }

  DisplayPlayersPing();
  m_dialog->Update();
}

void NetPlayClient::OnDesyncDetected(sf::Packet& packet)
{
  int pid_to_blame;
  u32 frame;
  if (!m_net_settings.m_RollbackMode)
  {
    packet >> pid_to_blame;
    packet >> frame;

    std::string player = "??";
    std::lock_guard lkp(m_crit.players);
    {
      const auto it = m_players.find(pid_to_blame);
      if (it != m_players.end())
        player = it->second.name;
    }

    INFO_LOG_FMT(NETPLAY, "Player {} ({}) desynced!", player, pid_to_blame);

    m_dialog->OnDesync(frame, player);
  }
}

void NetPlayClient::OnSyncSaveData(sf::Packet& packet)
{
  SyncSaveDataID sub_id;
  packet >> sub_id;

  if (m_local_player->IsHost())
    return;

  INFO_LOG_FMT(NETPLAY, "Processing OnSyncSaveData sub id: {}", static_cast<u8>(sub_id));

  switch (sub_id)
  {
  case SyncSaveDataID::Notify:
    OnSyncSaveDataNotify(packet);
    break;

  case SyncSaveDataID::RawData:
    OnSyncSaveDataRaw(packet);
    break;

  case SyncSaveDataID::GCIData:
    OnSyncSaveDataGCI(packet);
    break;

  case SyncSaveDataID::WiiData:
    OnSyncSaveDataWii(packet);
    break;

  case SyncSaveDataID::GBAData:
    OnSyncSaveDataGBA(packet);
    break;

  default:
    PanicAlertFmtT("Unknown SYNC_SAVE_DATA message received with id: {0}", static_cast<u8>(sub_id));
    break;
  }
}

void NetPlayClient::InitGekkoSession(const std::string& remote_addr, unsigned short local_port,
                                     bool is_host)
{
  DestroyGekkoSession();

  // Sync test (PPR_SYNCTEST=N, a host playing alone): a GekkoNet stress session that loads the
  // state from N frames back and resimulates on every frame, so every frame runs at least twice
  // with the same inputs. Any difference between the runs is a save/load or determinism bug, with
  // no network involved (GGPO's SyncTest).
  const int synctest_distance = m_players.size() == 1 ? SyncTestDistance() : 0;
  m_gekko_num_players = synctest_distance > 0 ? 1 : 2;

  if (!gekko_create(&m_gekko_session, synctest_distance > 0 ? GekkoStressSession : GekkoGameSession))
  {
    return;
  }

  GekkoConfig cfg{};
  cfg.num_players = static_cast<unsigned char>(m_gekko_num_players);
  cfg.check_distance = static_cast<unsigned int>(synctest_distance);
  cfg.input_size = sizeof(gfPadStatus);
  cfg.state_size = sizeof(u32);
  cfg.input_prediction_window = MAX_ROLLBACK_FRAMES;  // MAX_ROLLBACK_FRAMES
  cfg.max_spectators = 0;
  cfg.desync_detection = true;
  cfg.limited_saving = false;

  gekko_start(m_gekko_session, &cfg);

  // Set up custom ENet adapter for GekkoNet
  s_gekko_client_instance = this;

  if (!m_gekko_adapter)
  {
    m_gekko_adapter = new GekkoNetAdapter();
    m_gekko_adapter->send_data = GekkoNetAdapter_SendData;
    m_gekko_adapter->receive_data = GekkoNetAdapter_ReceiveData;
    m_gekko_adapter->free_data = GekkoNetAdapter_FreeData;
  }

  gekko_net_adapter_set(m_gekko_session, m_gekko_adapter);

  // Add players
  GekkoNetAddress addr{const_cast<char*>(remote_addr.data()),
                       static_cast<unsigned int>(remote_addr.size())};

  if (synctest_distance > 0)
  {
    m_gekko_local_handle = gekko_add_actor(m_gekko_session, GekkoLocalPlayer, nullptr);
    m_gekko_remote_handle = -1;
  }
  else if (is_host)
  {
    m_gekko_local_handle = gekko_add_actor(m_gekko_session, GekkoLocalPlayer, nullptr);
    m_gekko_remote_handle = gekko_add_actor(m_gekko_session, GekkoRemotePlayer, &addr);
  }
  else
  {
    m_gekko_remote_handle = gekko_add_actor(m_gekko_session, GekkoRemotePlayer, &addr);
    m_gekko_local_handle = gekko_add_actor(m_gekko_session, GekkoLocalPlayer, nullptr);
  }

  // Local input delay, chosen by the host (NetSettings::rollback_delay, 1-9 frames from reading the
  // controller to the game using it, like Slippi's delay frames; default 2). The local sample is
  // captured during frame F-1's pad read and submitted at the start of frame F, so it already
  // lands one frame late; GekkoNet's delay comes on top of that. Both peers use the same value.
  const int gekko_input_delay = static_cast<int>(m_net_settings.rollback_delay) - 1;
  gekko_set_local_delay(m_gekko_session, m_gekko_local_handle,
                        static_cast<unsigned char>(gekko_input_delay));
  INFO_LOG_FMT(BRAWLBACK, "GekkoNet: rollback delay {} (GekkoNet local delay {})",
               m_net_settings.rollback_delay, gekko_input_delay);

  // Drop a silent peer when Slippi would: after its 7 s stall (Online/Timeouts.h), not after
  // GekkoNet's default 5 s.
  const u32 disconnect_timeout_ms =
      Online::PeerSilenceTimeoutMs(static_cast<int>(m_net_settings.rollback_delay));
  gekko_set_disconnect_timeout(m_gekko_session, disconnect_timeout_ms);
  INFO_LOG_FMT(BRAWLBACK, "GekkoNet: disconnect timeout {} ms", disconnect_timeout_ms);

  m_gekko_remote_addr = remote_addr;
  m_gekko_next_ping_display_frame = PING_DISPLAY_INTERVAL;
  m_use_gekko_netplay = true;
  m_stat_gekko_session = true;
  m_stat_session_started = false;
  m_stat_current_frame = 0;
  m_stat_rollbacks = 0;
  m_stat_max_rollback_frames = 0;
  m_stat_frames_resimulated = 0;
  m_stat_desyncs_detected = 0;
  m_stat_last_desync_frame = -1;
  m_stat_stall_polls = 0;
  m_stat_peer_disconnects = 0;
  m_gekko_disconnect_stop_requested = false;
  m_stat_skipped_loads = 0;
  m_stat_dropped_advances = 0;
  m_stat_stall_fallbacks = 0;
  m_stat_synctest_mismatches = 0;
  for (auto& depth : m_stat_depth)
  {
    depth.count = 0;
    depth.load_us_total = 0;
    depth.load_us_max = 0;
    depth.resim_us_total = 0;
    depth.resim_us_max = 0;
  }
  m_resim_depth = 0;
  m_queued_save = QueuedFrameSave{};
  m_gekko_session_started = false;
  // PPR_ROLLBACK_CHUNK_HASHES=1 (64 KiB chunks) or =<chunk size in bytes>; always on in a sync test.
  m_chunk_hash_size = CHUNK_HASH_SIZE;
  if (const char* env = std::getenv("PPR_ROLLBACK_CHUNK_HASHES"))
  {
    const unsigned long size = std::strtoul(env, nullptr, 0);
    if (size >= 64)
      m_chunk_hash_size = static_cast<u32>(size);
  }
  m_chunk_hashes_enabled =
      std::getenv("PPR_ROLLBACK_CHUNK_HASHES") != nullptr || synctest_distance > 0;
  {
    std::lock_guard lk(m_chunk_hash_mutex);
    for (auto& entry : m_chunk_hashes)
      entry = ChunkHashEntry{};
  }
  m_start_sync_waiting = false;
  m_start_sync_remote_ready = false;
  m_start_sync_go_sent = false;
  m_start_sync_go_received = false;
  m_start_sync_start_at.reset();
  m_synctest = synctest_distance > 0;
  if (m_synctest)
  {
    // No peer to handshake with: a stress session advances right away.
    m_gekko_session_started = true;
    m_stat_session_started = true;
    time_synced = true;
  }
  m_time_sync_speed_factor = 1.0;
  m_time_sync_last_update_frame = UINT32_MAX;
  {
    std::lock_guard lk(m_pad_history_mutex);
    m_pad_history.fill(PadHistoryEntry{});
  }
  m_gekko_last_synced_pads.fill(gfPadStatus{});
  m_gekko_has_last_synced_pad.fill(false);
}

void NetPlayClient::DestroyGekkoSession()
{
  if (m_gekko_session)
  {
    gekko_destroy(&m_gekko_session);
    m_gekko_session = nullptr;
    m_gekko_session_started = false;
    m_gekko_frame_save_initialized = false;
    time_synced = false;
  }

  // Cleanup custom ENet adapter
  if (m_gekko_adapter)
  {
    delete m_gekko_adapter;
    m_gekko_adapter = nullptr;
  }

  // Clear packet queues
  {
    std::lock_guard<std::mutex> lock(m_gekko_packet_mutex);
    m_gekko_received_packets.clear();
    m_gekko_packet_ptrs.clear();
  }
  {
    std::lock_guard<std::mutex> lock(m_gekko_send_mutex);
    m_gekko_send_queue.clear();
  }

  s_gekko_client_instance = nullptr;

  m_use_gekko_netplay = false;
  m_stat_gekko_session = false;
  m_stat_session_started = false;
  m_gekko_local_handle = -1;
  m_gekko_remote_handle = -1;
}

bool NetPlayClient::ProcessGekkoEvents()
{
  if (!m_gekko_session)
    return false;

  int session_event_count = 0;
  auto** session_events = gekko_session_events(m_gekko_session, &session_event_count);

  for (int i = 0; i < session_event_count; i++)
  {
    switch (session_events[i]->type)
    {
    case GekkoPlayerSyncing:
      INFO_LOG_FMT(BRAWLBACK, "GekkoNet: Player {} syncing {}/{}",
                   session_events[i]->data.syncing.handle,
                   session_events[i]->data.syncing.current,
                   session_events[i]->data.syncing.max);
      break;

    case GekkoPlayerConnected:
      INFO_LOG_FMT(BRAWLBACK, "GekkoNet: Player {} connected",
                   session_events[i]->data.connected.handle);
      break;

    case GekkoSessionStarted:
      m_gekko_session_started = true;
      // Each peer gets this when its own side of the handshake completes, which can be a round
      // trip apart. Hold the first frame until both have started (PollStartSync()).
      m_start_sync_waiting = true;
      m_stat_session_started = true;
      INFO_LOG_FMT(BRAWLBACK, "GekkoNet: Session started");
      break;

    case GekkoPlayerDisconnected:
      // GekkoNet keeps the session in agreement on its own: both sides settle on the frame from
      // which the dropped player's input is neutral (disconnect claims), and the game events of
      // this update already account for it. Dropping this update's events instead (as before)
      // left the game one or more frames behind GekkoNet's frame numbering on this side only: a
      // permanent desync (seen after multi-second stalls: the frame counters differed by one).
      m_stat_peer_disconnects++;
      WARN_LOG_FMT(BRAWLBACK, "GekkoNet: Player {} disconnected",
                   session_events[i]->data.disconnected.handle);
      // GekkoNet drops a peer it has not heard from for its disconnect timeout and plays that
      // player with neutral input from an agreed frame on. With two players that cannot stay in
      // agreement: each side drops the other (at different frames) and keeps its own player
      // moving, so the match silently desyncs (seen whenever one side stalled for 5+ s). End the
      // game on both sides instead (the server relays the stop).
      if (m_gekko_num_players <= 2 && !m_gekko_disconnect_stop_requested)
      {
        m_gekko_disconnect_stop_requested = true;
        // Slippi shows "DISCONNECTED" in red in the game's HUD and ends the match (no OSD
        // message); until our game-side UI exists (docs/backend-design.md 5.6) the OSD carries
        // the same text.
        OSD::AddMessage("DISCONNECTED", 10000, OSD::Color::RED);
        RequestStopGame();
      }
      break;

    case GekkoDesyncDetected:
      m_stat_desyncs_detected++;
      m_stat_last_desync_frame = session_events[i]->data.desynced.frame;
#ifdef BRAWLBACK_DESYNC_DETECTION
      WARN_LOG_FMT(BRAWLBACK, "GekkoNet: Desync detected at frame {} (remote={}, local_checksum={:08x}, remote_checksum={:08x})",
                   session_events[i]->data.desynced.frame,
                   session_events[i]->data.desynced.remote_handle,
                   session_events[i]->data.desynced.local_checksum,
                   session_events[i]->data.desynced.remote_checksum);
#endif
      break;

    default:
      break;
    }
  }
  return true;
}

void NetPlayClient::HandleGekkoFrame()
{
  if (!m_gekko_session || !m_use_gekko_netplay)
    return;

  m_gekko_pending_ops.Clear();

  {
    std::lock_guard<std::mutex> lock(m_gekko_poll_mutex);
    gekko_network_poll(m_gekko_session);
  }

  gekko_add_local_input(m_gekko_session, m_gekko_local_handle, &m_gekko_last_local_input);

  int game_event_count = 0;
  GekkoGameEvent** game_events = gekko_update_session(m_gekko_session, &game_event_count);

  if (!ProcessGekkoEvents())
  {
    m_gekko_pending_ops.adv_count = 0;
    return;
  }

  int num_adv = 0;
  bool pending_load = false;
  int pending_load_frame = 0;
  bool pending_initial_save = false;  // Track SAVE events before any ADVANCE (e.g., frame -1)

  for (int i = 0; i < game_event_count; i++)
  {
    auto& evt = *game_events[i];
    switch (evt.type)
    {
    case GekkoSaveEvent:
      INFO_LOG_FMT(BRAWLBACK, "GekkoNet: SAVE event at frame {}", evt.data.save.frame);
      if (evt.data.save.state_len)
        *evt.data.save.state_len = sizeof(u32);
      if (num_adv > 0)
      {
        m_gekko_pending_ops.save_after[num_adv - 1] = true;
        m_gekko_pending_ops.save_checksum_ptrs[num_adv - 1] = evt.data.save.checksum;
      }
      else
      {
        if (evt.data.save.checksum)
          *evt.data.save.checksum = Rollback::CalculateBrawlbackDesyncChecksum(Core::System::GetInstance());
        pending_initial_save = true;
      }
      break;

    case GekkoLoadEvent:
      INFO_LOG_FMT(BRAWLBACK, "GekkoNet: LOAD event at frame {}", evt.data.load.frame);
      pending_load = true;
      pending_load_frame = evt.data.load.frame;
      break;

    case GekkoAdvanceEvent:
      INFO_LOG_FMT(BRAWLBACK, "GekkoNet: ADVANCE event at frame {}", evt.data.adv.frame);
      if (num_adv >= GekkonetPendingOps::MAX_ADVANCE)
      {
        // Never expected (a rollback is at most MAX_ROLLBACK_FRAMES deep, plus the new frame),
        // but a dropped advance would put this side behind GekkoNet's frame numbering for good.
        m_stat_dropped_advances++;
        ERROR_LOG_FMT(BRAWLBACK, "GekkoNet: more than {} advances in one update; frame {} dropped",
                      GekkonetPendingOps::MAX_ADVANCE, evt.data.adv.frame);
      }
      else
      {
        m_gekko_pending_ops.adv_frames[num_adv] = static_cast<u32>(evt.data.adv.frame);
        m_gekko_pending_ops.adv_rollback[num_adv] = evt.data.adv.rolling_back;

        if (pending_load)
        {
          m_gekko_pending_ops.load_before[num_adv] = true;
          m_gekko_pending_ops.load_before_frame[num_adv] = pending_load_frame;
          pending_load = false;
        }

        for (int p = 0; p < 4; p++)
          m_gekko_pending_ops.adv_pads[p][num_adv] = gfPadStatus();

        if (evt.data.adv.inputs)
        {
          int adv_local_port = -1;
          int adv_remote_port = -1;
          const auto& adv_pad_mapping = GetPadMapping();

          // Find local player's port
          for (int port_idx = 0; port_idx < static_cast<int>(adv_pad_mapping.size()); port_idx++)
          {
            if (adv_pad_mapping.at(port_idx) == m_local_player->pid)
            {
              adv_local_port = port_idx;
              break;
            }
          }

          for (int port_idx = 0; port_idx < static_cast<int>(adv_pad_mapping.size()); port_idx++)
          {
            PlayerId mapped_pid = adv_pad_mapping.at(port_idx);
            if (mapped_pid > 0 && mapped_pid != m_local_player->pid)
            {
              adv_remote_port = port_idx;
              break;
            }
          }

          for (int player_idx = 0; player_idx < m_gekko_num_players; player_idx++)
          {
            const gfPadStatus* pad_ptr = reinterpret_cast<const gfPadStatus*>(
              static_cast<const void*>(evt.data.adv.inputs + player_idx * sizeof(gfPadStatus)));

            // Map GekkoNet player index to in-game port
            int port = (player_idx == m_gekko_local_handle) ? adv_local_port : adv_remote_port;

            if (port >= 0 && port < 4)
            {
              m_gekko_pending_ops.adv_pads[port][num_adv] = *pad_ptr;
              m_gekko_last_synced_pads[port] = *pad_ptr;
              m_gekko_has_last_synced_pad[port] = true;
            }
          }
        }

        num_adv++;
      }
      break;

    default:
      break;
    }
  }

  int load_idx = -1;
  for (int i = 0; i < num_adv; i++)
  {
    if (m_gekko_pending_ops.load_before[i])
    {
      load_idx = i;
      break;
    }
  }

  if (load_idx >= 0)
  {
    // GekkoNet's load event names the frame whose *end* state to restore: its SAVE event for frame
    // F is emitted right after the ADVANCE event for F (GameSession::UpdateSession and
    // HandleRollback, StressSession alike), and it then resimulates from F + 1. Our ring gets one
    // slot per SAVE in the same order (plus the initial save of frame -1), so its most recent
    // slot holds the end of the frame before this update's normal advance, `normal_frame - 1`.
    // The slot to restore is therefore (normal_frame - 1) - snapshot_frame slots back. This used
    // to be normal_frame - snapshot_frame, one slot too old: every rollback silently dropped one
    // frame of game progress on the peer that rolled back (a sync test with distance 1 never
    // got past scBoot, replaying the same frame forever). 0 means the state is already current.
    const int snapshot_frame = m_gekko_pending_ops.load_before_frame[load_idx];
    const int target_frame = static_cast<int>(m_gekko_pending_ops.adv_frames[load_idx]);
    const int normal_frame = static_cast<int>(m_gekko_pending_ops.adv_frames[num_adv - 1]);
    const int frames_back = (normal_frame - 1) - snapshot_frame;

    auto& rbMgr = Rollback::RollbackManager::Get();

    INFO_LOG_FMT(BRAWLBACK, "GekkoNet: Rollback - load_idx={} last_adv_frame={} target_frame={} snapshot_frame={} frames_back={} num_adv={} ring_count={}",
           load_idx, normal_frame, target_frame, snapshot_frame, frames_back, num_adv,
           rbMgr.m_ring_count);

    if (frames_back >= 1 && frames_back <= MAX_ROLLBACK_FRAMES &&
        rbMgr.m_ring_count >= 2 && frames_back < rbMgr.m_ring_count)
    {
      const auto load_start = std::chrono::steady_clock::now();
      rbMgr.LoadFrame(Core::System::GetInstance(), frames_back);
      m_resim_start = std::chrono::steady_clock::now();
      if (frames_back < RollbackStats::MAX_DEPTH_STATS)
      {
        const u64 us = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(m_resim_start - load_start)
                .count());
        auto& depth = m_stat_depth[frames_back];
        depth.count++;
        depth.load_us_total += us;
        if (us > depth.load_us_max)
          depth.load_us_max = us;
        m_resim_depth = frames_back;
      }
      m_stat_rollbacks++;
      if (static_cast<u64>(frames_back) > m_stat_max_rollback_frames)
        m_stat_max_rollback_frames = static_cast<u64>(frames_back);

      // Reset iteration counter to 0 - all following advances will be resimulations
      SetGekkoCurrentIteration(0);
    }
    else if (frames_back != 0)
    {
      // The resimulation then runs on top of the current state instead of the requested one.
      m_stat_skipped_loads++;
      ERROR_LOG_FMT(BRAWLBACK,
                    "GekkoNet: cannot load {} frame(s) back (ring holds {}); rollback skipped",
                    frames_back, rbMgr.m_ring_count);
    }
  }

  if (pending_initial_save)
  {
    Rollback::RollbackManager::Get().SaveFrame(Core::System::GetInstance());
  }

  m_gekko_pending_ops.adv_count = num_adv;
  if (num_adv == 0)
    m_stat_stall_polls++;
  if (num_adv > 1)
    m_stat_frames_resimulated += static_cast<u64>(num_adv - 1);

  // The final iteration is presented even when GekkoNet labels the whole burst as a rollback.
  // InjectPadsForIteration sets this true only for throwaway resimulation iterations.
  m_is_rolling_back = false;
  CheckForLocalAdvantage();
}

void NetPlayClient::CheckForLocalAdvantage()
{
  // Kept for the stats only. Upstream showed it on screen every frame; Slippi shows the ping there.
  m_stat_frames_ahead = gekko_frames_ahead(m_gekko_session);
}

int NetPlayClient::SyncTestDistance()
{
  const char* env = std::getenv("PPR_SYNCTEST");
  if (!env)
    return 0;
  return std::clamp(std::atoi(env), 0, MAX_ROLLBACK_FRAMES);
}

void NetPlayClient::SendStartSyncMsg(StartSyncMsg msg)
{
  const char packet[2] = {static_cast<char>(GEKKO_CONTROL_MAGIC), static_cast<char>(msg)};
  {
    std::lock_guard<std::mutex> lock(m_gekko_send_mutex);
    m_gekko_send_queue.emplace_back(packet, sizeof(packet));
  }
  Common::ENet::WakeupThread(m_client);
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnGekkoControlPacket(const u8* data, size_t size)
{
  switch (static_cast<StartSyncMsg>(data[1]))
  {
  case StartSyncMsg::Ready:
    m_start_sync_remote_ready = true;
    // The joiner repeats Ready until it sees Go; answer again if our Go was lost.
    if (m_start_sync_go_sent)
      SendStartSyncMsg(StartSyncMsg::Go);
    break;
  case StartSyncMsg::Go:
    m_start_sync_go_received = true;
    break;
  default:
    WARN_LOG_FMT(BRAWLBACK, "Unknown GekkoNet control packet {:#x} ({} bytes)", data[1], size);
    break;
  }
}

// Returns true once this peer may simulate the first frame. The joiner tells the host when its
// session has started (repeating, the channel is unreliable); once the host has started too it
// answers Go and starts itself half a round trip later, which is when Go reaches the joiner.
// Starting each side on its own GekkoSessionStarted put the host about RTT/2 ahead from frame 0.
bool NetPlayClient::PollStartSync()
{
  const auto now = std::chrono::steady_clock::now();
  if (!m_local_player->IsHost())
  {
    if (m_start_sync_go_received)
      return true;
    if (now - m_start_sync_last_ready >= std::chrono::milliseconds(50))
    {
      SendStartSyncMsg(StartSyncMsg::Ready);
      m_start_sync_last_ready = now;
    }
    return false;
  }

  if (!m_start_sync_start_at && m_start_sync_remote_ready)
  {
    u32 ping_ms;
    {
      std::lock_guard lkp(m_crit.players);
      ping_ms = GetPlayersMaxPing();
    }
    m_start_sync_go_sent = true;
    SendStartSyncMsg(StartSyncMsg::Go);
    m_start_sync_start_at = now + std::chrono::microseconds(ping_ms * 1000 / 2);
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet: both peers ready, host starts in {} us", ping_ms * 1000 / 2);
  }
  return m_start_sync_start_at && now >= *m_start_sync_start_at;
}

void NetPlayClient::UpdateTimeSync()
{
  // Slippi-style time sync: rather than sleeping (which Dolphin's absolute-timeline throttle
  // simply repays on the next frames), nudge the emulation speed of the peer that runs ahead
  // (and, less, of the one behind) until both reach the same GekkoNet frame at the same time.
  // gekko_frames_ahead() is already halved, i.e. this peer's share of the gap.
  constexpr u32 UPDATE_INTERVAL_FRAMES = 15;
  constexpr float DEAD_ZONE_FRAMES = 0.25f;
  constexpr float FULL_CORRECTION_FRAMES = 2.0f;
  constexpr double MAX_SLOW_DOWN = 0.02;
  constexpr double MAX_SPEED_UP = 0.01;

  const u32 frame = static_cast<u32>(current_frame);
  if (frame % UPDATE_INTERVAL_FRAMES != 0 || frame == m_time_sync_last_update_frame)
    return;
  m_time_sync_last_update_frame = frame;

  const float ahead = gekko_frames_ahead(m_gekko_session);
  double factor = 1.0;
  if (ahead > DEAD_ZONE_FRAMES)
    factor = 1.0 - std::min(ahead / FULL_CORRECTION_FRAMES, 1.0f) * MAX_SLOW_DOWN;
  else if (ahead < -DEAD_ZONE_FRAMES)
    factor = 1.0 + std::min(-ahead / FULL_CORRECTION_FRAMES, 1.0f) * MAX_SPEED_UP;
  m_time_sync_speed_factor = factor;
}

// Guest memory is big-endian; every multi-byte field must be swapped before the raw memcpy below.
static gfPadStatus ToBigEndian(gfPadStatus status)
{
  status.m_buttonsCurrentFrame.bits = Common::swap32(status.m_buttonsCurrentFrame.bits);
  status.m_buttonsCurrentFrame2.bits = Common::swap32(status.m_buttonsCurrentFrame2.bits);
  status.m_buttonsHeld.bits = Common::swap32(status.m_buttonsHeld.bits);
  status.m_buttonsPressedThisFrame.bits = Common::swap32(status.m_buttonsPressedThisFrame.bits);
  status.m_buttonsReleasedThisFrame.bits = Common::swap32(status.m_buttonsReleasedThisFrame.bits);
  status.m_buttonsPressedThisFrame2.bits = Common::swap32(status.m_buttonsPressedThisFrame2.bits);
  // The motion floats are always 0 for a GC pad, so their bit pattern is swap-invariant.
  status.m_controllerType =
      static_cast<gfPadType::PadType>(Common::swap32(static_cast<u32>(status.m_controllerType)));
  return status;
}

static gfPadStatus FromBigEndian(gfPadStatus status)
{
  return ToBigEndian(status);
}

static GCPadStatus ToGCPadStatus(const gfPadStatus& status)
{
  GCPadStatus pad{};
  pad.button = static_cast<u16>(status.m_buttonsCurrentFrame2.bits);
  pad.stickX = static_cast<u8>(status.m_stickX + GCPadStatus::MAIN_STICK_CENTER_X);
  pad.stickY = static_cast<u8>(status.m_stickY + GCPadStatus::MAIN_STICK_CENTER_Y);
  pad.substickX = static_cast<u8>(status.m_subStickX + GCPadStatus::C_STICK_CENTER_X);
  pad.substickY = static_cast<u8>(status.m_subStickY + GCPadStatus::C_STICK_CENTER_Y);
  pad.triggerLeft = static_cast<u8>(status.m_lTriggerAnalog);
  pad.triggerRight = static_cast<u8>(status.m_rTriggerAnalog);
  pad.isConnected = status.m_error == gfPadError::NONE;
  return pad;
}

void NetPlayClient::InjectPads(const std::array<gfPadStatus, 4>& pads, Core::System& system)
{
  auto& mem = system.GetMemory();
  const auto& pad_mapping = GetPadMapping();

  for (int i = 0; i < 4; i++)
  {
    s_override_pads[i] = pads[i];

    // Only ports driven by a player carry GekkoNet input. Leave the others as the game's own
    // pad read left them (disconnected): writing a zeroed gfPadStatus there would make them read
    // as connected (m_error == NONE) on resimulated frames only.
    if (pad_mapping[i] <= 0)
      continue;

    const u32 base = BRAWL_PAD_RAW_BASE + static_cast<u32>(i) * BRAWL_PAD_STRIDE;
    const gfPadStatus be_status = ToBigEndian(pads[i]);
    mem.CopyToEmu(base, &be_status, sizeof(gfPadStatus));
  }

  // Enable SI override flag
  s_override_active.store(true, std::memory_order_release);
}

void NetPlayClient::OnFrameStart(std::unique_lock<std::mutex>& lock)
{
  if (m_use_gekko_netplay && m_gekko_session)
  {
    if (!m_gekko_frame_save_initialized)
    {
      auto& rbMgr = Rollback::RollbackManager::Get();
      if (!rbMgr.m_frame_save_enabled.load(std::memory_order_relaxed))
      {
        rbMgr.ToggleFrameSave();
      }
      m_gekko_frame_save_initialized = true;
    }

    if (m_start_sync_waiting)
    {
      // GekkoNet already handed out the first frame when the session started; keep those pending
      // ops for when both peers start, and only keep the connection serviced meanwhile.
      {
        std::lock_guard<std::mutex> poll_lock(m_gekko_poll_mutex);
        gekko_network_poll(m_gekko_session);
      }
      if (!PollStartSync())
        return;
      m_start_sync_waiting = false;
      time_synced = true;
      return;
    }

    HandleGekkoFrame();
    if (m_gekko_pending_ops.adv_count > 0)
    {
      const u32 last_adv_frame = m_gekko_pending_ops.adv_frames[m_gekko_pending_ops.adv_count - 1];
      current_frame = last_adv_frame;
      m_stat_current_frame = last_adv_frame;
      DisplayGekkoPing(last_adv_frame);
      UpdateTimeSync();
    }
  }
}

void NetPlayClient::InjectPadsForIteration(int iteration_index)
{
  if (!m_use_gekko_netplay || !m_gekko_session)
    return;

  if (iteration_index < 0 || iteration_index >= m_gekko_pending_ops.adv_count)
  {
    return;
  }

  // Log which frame's inputs we're injecting
  INFO_LOG_FMT(BRAWLBACK, "GekkoNet: InjectPadsForIteration - iteration={} target_frame={} is_rollback={}",
               iteration_index, m_gekko_pending_ops.adv_frames[iteration_index],
               m_gekko_pending_ops.adv_rollback[iteration_index]);

  std::array<gfPadStatus, 4> pads_for_iteration;
  for (int port = 0; port < 4; port++)
  {
    pads_for_iteration[port] = m_gekko_pending_ops.adv_pads[port][iteration_index];
  }

  InjectPads(pads_for_iteration, Core::System::GetInstance());
}

void NetPlayClient::SetGekkoResimulationPass(bool is_resimulation_pass)
{
  m_is_rolling_back = is_resimulation_pass;
}

void NetPlayClient::CaptureGekkoPadInput(Core::System& system)
{
  if (!m_use_gekko_netplay || !m_gekko_session || !m_local_player)
    return;

  const auto& pad_mapping = GetPadMapping();
  for (int port = 0; port < static_cast<int>(pad_mapping.size()); ++port)
  {
    if (pad_mapping[port] != m_local_player->pid)
      continue;

    gfPadStatus status{};
    const u32 address = BRAWL_PAD_RAW_BASE + static_cast<u32>(port) * BRAWL_PAD_STRIDE;
    system.GetMemory().CopyFromEmu(&status, address, sizeof(status));
    m_gekko_last_local_input = FromBigEndian(status);
    return;
  }
}

void NetPlayClient::RecordPadHistory(Core::System& system, int iteration_index)
{
  if (!m_use_gekko_netplay || iteration_index < 0 ||
      iteration_index >= m_gekko_pending_ops.adv_count)
    return;

  std::array<u8, 4 * BRAWL_PAD_STRIDE> raw;
  system.GetMemory().CopyFromEmu(raw.data(), BRAWL_PAD_RAW_BASE, raw.size());

  PadHistoryEntry entry;
  entry.frame = m_gekko_pending_ops.adv_frames[iteration_index];
  for (size_t port = 0; port < 4; ++port)
  {
    u32 buttons;
    std::memcpy(&buttons, raw.data() + port * BRAWL_PAD_STRIDE, sizeof(buttons));
    entry.buttons[port] = Common::swap32(buttons);
  }
  entry.crc = Common::ComputeCRC32(raw.data(), static_cast<u32>(raw.size()));
  static_assert(sizeof(entry.raw) == sizeof(raw));
  std::memcpy(entry.raw.data(), raw.data(), raw.size());

  std::lock_guard lk(m_pad_history_mutex);
  m_pad_history[static_cast<size_t>(entry.frame) % PAD_HISTORY_SIZE] = entry;
}

void NetPlayClient::QueueFrameSave(int iteration_index, u32 logic_steps)
{
  if (!m_use_gekko_netplay || iteration_index < 0 ||
      iteration_index >= m_gekko_pending_ops.adv_count ||
      !m_gekko_pending_ops.save_after[iteration_index])
    return;

  m_queued_save.pending = true;
  m_queued_save.frame = m_gekko_pending_ops.adv_frames[iteration_index];
  m_queued_save.checksum = m_gekko_pending_ops.save_checksum_ptrs[iteration_index];
  m_queued_save.logic_steps = logic_steps;
}

void NetPlayClient::PerformQueuedFrameSave(Core::System& system)
{
  if (!m_queued_save.pending)
    return;
  m_queued_save.pending = false;

  Rollback::RollbackManager::Get().SaveFrame(system);
  const Rollback::DesyncChecksums sums = Rollback::CalculateDesyncChecksums(system);
  if (m_queued_save.checksum)
    *m_queued_save.checksum = sums.combined;
  RecordStateHistory(m_queued_save.frame, {sums.combined, sums.legacy, sums.frame_counter,
                                           sums.fighters, m_queued_save.logic_steps});
  RecordChunkHashes(system, m_queued_save.frame);
}

void NetPlayClient::RecordStateHistory(s64 frame, const std::array<u32, 5>& state)
{
  if (!m_use_gekko_netplay)
    return;

  std::lock_guard lk(m_pad_history_mutex);
  PadHistoryEntry& entry = m_pad_history[static_cast<size_t>(frame) % PAD_HISTORY_SIZE];
  if (entry.frame == frame)
    entry.state = state;
}

void NetPlayClient::RecordChunkHashes(Core::System& system, s64 frame)
{
  if (!m_chunk_hashes_enabled || !m_use_gekko_netplay)
    return;

  auto& memory = system.GetMemory();
  const std::array<std::pair<const u8*, size_t>, 2> regions{
      {{memory.GetRAM(), memory.GetRamSize()}, {memory.GetEXRAM(), memory.GetExRamSize()}}};
  std::vector<u64> hashes;
  for (const auto& [base, size] : regions)
  {
    if (!base)
      continue;
    for (size_t offset = 0; offset < size; offset += m_chunk_hash_size)
      hashes.push_back(XXH3_64bits(base + offset, std::min<size_t>(m_chunk_hash_size, size - offset)));
  }

  std::lock_guard lk(m_chunk_hash_mutex);
  ChunkHashEntry& entry = m_chunk_hashes[static_cast<size_t>(frame) % m_chunk_hashes.size()];
  if (m_synctest && entry.frame == frame && entry.hashes.size() == hashes.size())
  {
    // Sync test: compare this run of the frame with its first run, and keep the first.
    u32 differing = 0;
    std::string first;
    for (size_t i = 0; i < hashes.size(); ++i)
    {
      if (hashes[i] == entry.hashes[i])
        continue;
      if (differing < 48)
        first += fmt::format(" {:08x}", ChunkAddress(memory, i));
      ++differing;
    }
    if (differing)
    {
      // Word-level detail from the full copy of the first run, if we still have it.
      const SyncTestRamCopy& copy = m_synctest_ram[static_cast<size_t>(frame) % m_synctest_ram.size()];
      if (copy.frame == frame)
      {
        std::string words;
        u32 shown = 0;
        size_t offset = 0;
        for (const auto& [base, size] : regions)
        {
          for (size_t i = 0; base && i + 4 <= size && shown < 32; i += 4)
          {
            u32 now_v, then_v;
            std::memcpy(&now_v, base + i, 4);
            std::memcpy(&then_v, copy.ram.data() + offset + i, 4);
            if (now_v == then_v)
              continue;
            const u32 addr = (base == memory.GetRAM() ? 0x80000000u : 0x90000000u) + static_cast<u32>(i);
            words += fmt::format(" {:08x}:{:08x}->{:08x}", addr, Common::swap32(then_v),
                                 Common::swap32(now_v));
            ++shown;
          }
          offset += size;
        }
        WARN_LOG_FMT(BRAWLBACK, "SYNCTEST: frame {} words (addr:first->now):{}", frame, words);
      }
      m_stat_synctest_mismatches++;
      WARN_LOG_FMT(BRAWLBACK, "SYNCTEST: frame {} differs from its first run in {} chunk(s) of {:#x} bytes:{}",
                   frame, differing, m_chunk_hash_size, first);
    }
    return;
  }
  entry.frame = frame;
  entry.hashes = std::move(hashes);
  if (m_synctest)
  {
    SyncTestRamCopy& copy = m_synctest_ram[static_cast<size_t>(frame) % m_synctest_ram.size()];
    copy.frame = frame;
    copy.ram.resize(memory.GetRamSize() + memory.GetExRamSize());
    std::memcpy(copy.ram.data(), memory.GetRAM(), memory.GetRamSize());
    if (memory.GetEXRAM())
      std::memcpy(copy.ram.data() + memory.GetRamSize(), memory.GetEXRAM(), memory.GetExRamSize());
  }
}

u32 NetPlayClient::ChunkAddress(const Memory::MemoryManager& memory, size_t index) const
{
  const size_t mem1_chunks = (memory.GetRamSize() + m_chunk_hash_size - 1) / m_chunk_hash_size;
  if (index < mem1_chunks)
    return 0x80000000u + static_cast<u32>(index * m_chunk_hash_size);
  return 0x90000000u + static_cast<u32>((index - mem1_chunks) * m_chunk_hash_size);
}

std::optional<std::vector<u64>> NetPlayClient::GetChunkHashes(s64 frame) const
{
  std::lock_guard lk(m_chunk_hash_mutex);
  const ChunkHashEntry& entry = m_chunk_hashes[static_cast<size_t>(frame) % m_chunk_hashes.size()];
  if (entry.frame != frame)
    return std::nullopt;
  return entry.hashes;
}

std::vector<NetPlayClient::PadHistoryEntry> NetPlayClient::GetPadHistory(s64 since_frame) const
{
  std::vector<PadHistoryEntry> out;
  std::lock_guard lk(m_pad_history_mutex);
  for (const PadHistoryEntry& entry : m_pad_history)
  {
    if (entry.frame >= since_frame)
      out.push_back(entry);
  }
  std::sort(out.begin(), out.end(),
            [](const PadHistoryEntry& a, const PadHistoryEntry& b) { return a.frame < b.frame; });
  return out;
}

NetPlayClient::RollbackStats NetPlayClient::GetRollbackStats() const
{
  RollbackStats stats;
  stats.gekko_session = m_stat_gekko_session;
  stats.session_started = m_stat_session_started;
  stats.current_frame = m_stat_current_frame;
  stats.rollbacks = m_stat_rollbacks;
  stats.max_rollback_frames = m_stat_max_rollback_frames;
  stats.frames_resimulated = m_stat_frames_resimulated;
  stats.desyncs_detected = m_stat_desyncs_detected;
  stats.last_desync_frame = m_stat_last_desync_frame;
  stats.frames_ahead = m_stat_frames_ahead;
  stats.time_sync_speed = m_time_sync_speed_factor;
  stats.stall_polls = m_stat_stall_polls;
  stats.peer_disconnects = m_stat_peer_disconnects;
  stats.skipped_loads = m_stat_skipped_loads;
  stats.dropped_advances = m_stat_dropped_advances;
  stats.stall_fallbacks = m_stat_stall_fallbacks;
  stats.synctest = m_synctest;
  stats.synctest_mismatches = m_stat_synctest_mismatches;
  const auto timing = Rollback::RollbackManager::Get().GetTimingStats();
  stats.save_count = timing.save_count;
  stats.save_us_total = timing.save_us_total;
  stats.save_us_max = timing.save_us_max;
  stats.load_count = timing.load_count;
  stats.load_us_total = timing.load_us_total;
  stats.load_us_max = timing.load_us_max;
  stats.save_sync_count = timing.save_sync_count;
  stats.save_sync_us_total = timing.save_sync_us_total;
  stats.save_sync_us_max = timing.save_sync_us_max;
  for (int i = 0; i < RollbackStats::MAX_DEPTH_STATS; ++i)
  {
    const auto& src = m_stat_depth[i];
    auto& dst = stats.by_depth[i];
    dst.count = src.count;
    dst.load_us_total = src.load_us_total;
    dst.load_us_max = src.load_us_max;
    dst.resim_us_total = src.resim_us_total;
    dst.resim_us_max = src.resim_us_max;
  }
  return stats;
}

void NetPlayClient::OnDisplayedFrameStart()
{
  if (m_resim_depth == 0)
    return;
  const u64 us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - m_resim_start)
                                      .count());
  auto& depth = m_stat_depth[m_resim_depth];
  depth.resim_us_total += us;
  if (us > depth.resim_us_max)
    depth.resim_us_max = us;
  m_resim_depth = 0;
}

bool NetPlayClient::IsRollingBack()
{
  return m_is_rolling_back;
}

bool NetPlayClient::IsInRollbackMode()
{
  return m_net_settings.m_RollbackMode;
}

int NetPlayClient::GetFramesToAdvance()
{
  if (m_use_gekko_netplay && m_gekko_session)
  {
    return m_gekko_pending_ops.adv_count;
  }

  return 1;
}

bool NetPlayClient::IsTimeSynced()
{
  // No Gekko session for the match exists yet (still booting/in menus) - nothing to wait on.
  if (!m_use_gekko_netplay || !m_gekko_session)
    return true;

  return time_synced;
}

void NetPlayClient::OnSyncSaveDataNotify(sf::Packet& packet)
{
  packet >> m_sync_save_data_count;
  m_sync_save_data_success_count = 0;

  INFO_LOG_FMT(NETPLAY, "Initializing wait for {} savegame chunks.", m_sync_save_data_count);

  if (m_sync_save_data_count == 0)
    SyncSaveDataResponse(true);
  else
    m_dialog->AppendChat(Common::GetStringT("Synchronizing save data..."));
}

void NetPlayClient::OnSyncSaveDataRaw(sf::Packet& packet)
{
  bool is_slot_a;
  std::string region;
  int size_override;
  packet >> is_slot_a >> region >> size_override;
  INFO_LOG_FMT(NETPLAY, "Received raw memcard data for slot {}: region {}, size override {}.",
               is_slot_a ? 'A' : 'B', region, size_override);

  // This check is mainly intended to filter out characters which have special meanings in paths
  if (region != JAP_DIR && region != USA_DIR && region != EUR_DIR)
  {
    WARN_LOG_FMT(NETPLAY, "Received invalid raw memory card region.");
    SyncSaveDataResponse(false);
    return;
  }

  std::string size_suffix;
  if (size_override >= 0 && size_override <= 4)
  {
    size_suffix = fmt::format(
        ".{}", Memcard::MbitToFreeBlocks(Memcard::MBIT_SIZE_MEMORY_CARD_59 << size_override));
  }

  const std::string path = File::GetUserPath(D_GCUSER_IDX) + GC_MEMCARD_NETPLAY +
                           (is_slot_a ? "A." : "B.") + region + size_suffix + ".raw";
  if (File::Exists(path) && !File::Delete(path))
  {
    PanicAlertFmtT("Failed to delete NetPlay memory card. Verify your write permissions.");
    SyncSaveDataResponse(false);
    return;
  }

  const bool success = DecompressPacketIntoFile(packet, path);
  SyncSaveDataResponse(success);
}

void NetPlayClient::OnSyncSaveDataGCI(sf::Packet& packet)
{
  bool is_slot_a;
  u8 file_count;
  packet >> is_slot_a >> file_count;

  const std::string path = File::GetUserPath(D_GCUSER_IDX) + GC_MEMCARD_NETPLAY DIR_SEP +
                           fmt::format("Card {}", is_slot_a ? 'A' : 'B');

  INFO_LOG_FMT(NETPLAY, "Received GCI memcard data for slot {}: {}, {} files.",
               is_slot_a ? 'A' : 'B', path, file_count);

  if ((File::Exists(path) && !File::DeleteDirRecursively(path + DIR_SEP)) ||
      !File::CreateFullPath(path + DIR_SEP))
  {
    PanicAlertFmtT("Failed to reset NetPlay GCI folder. Verify your write permissions.");
    SyncSaveDataResponse(false);
    return;
  }

  for (u8 i = 0; i < file_count; i++)
  {
    std::string file_name;
    packet >> file_name;

    INFO_LOG_FMT(NETPLAY, "Received GCI: {}", file_name);

    if (!Common::IsFileNameSafe(file_name) ||
        !DecompressPacketIntoFile(packet, path + DIR_SEP + file_name))
    {
      WARN_LOG_FMT(NETPLAY, "Received invalid GCI.");
      SyncSaveDataResponse(false);
      return;
    }
  }

  SyncSaveDataResponse(true);
}

void NetPlayClient::OnSyncSaveDataWii(sf::Packet& packet)
{
  const std::string path = File::GetUserPath(D_USER_IDX) + "Wii" GC_MEMCARD_NETPLAY DIR_SEP;
  std::string redirect_path = File::GetUserPath(D_USER_IDX) + "Redirect" GC_MEMCARD_NETPLAY DIR_SEP;

  if (File::Exists(path) && !File::DeleteDirRecursively(path))
  {
    PanicAlertFmtT("Failed to reset NetPlay NAND folder. Verify your write permissions.");
    SyncSaveDataResponse(false);
    return;
  }
  if (File::Exists(redirect_path) && !File::DeleteDirRecursively(redirect_path))
  {
    PanicAlertFmtT("Failed to reset NetPlay redirect folder. Verify your write permissions.");
    SyncSaveDataResponse(false);
    return;
  }

  auto temp_fs = std::make_unique<IOS::HLE::FS::HostFileSystem>(path);
  std::vector<u64> titles;

  constexpr IOS::HLE::FS::Modes fs_modes{
      IOS::HLE::FS::Mode::ReadWrite,
      IOS::HLE::FS::Mode::ReadWrite,
      IOS::HLE::FS::Mode::ReadWrite,
  };

  // Read the Mii data
  bool mii_data;
  packet >> mii_data;
  if (mii_data)
  {
    INFO_LOG_FMT(NETPLAY, "Received Mii data.");

    auto buffer = DecompressPacketIntoBuffer(packet);

    temp_fs->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL, "/shared2/menu/FaceLib/", 0,
                            fs_modes);
    auto file = temp_fs->CreateAndOpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL,
                                           Common::GetMiiDatabasePath(), fs_modes);

    if (!buffer || !file || !file->Write(buffer->data(), buffer->size()))
    {
      PanicAlertFmtT("Failed to write Mii data.");
      SyncSaveDataResponse(false);
      return;
    }
  }

  // Read the saves
  u32 save_count;
  packet >> save_count;
  INFO_LOG_FMT(NETPLAY, "Received data for {} Wii saves.", save_count);
  for (u32 n = 0; n < save_count; n++)
  {
    u64 title_id = Common::PacketReadU64(packet);
    titles.push_back(title_id);
    temp_fs->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL,
                            Common::GetTitleDataPath(title_id) + '/', 0, fs_modes);
    auto save = WiiSave::MakeNandStorage(temp_fs.get(), title_id);

    bool exists;
    packet >> exists;
    if (!exists)
    {
      INFO_LOG_FMT(NETPLAY, "No data for Wii save of title {:016x}.", title_id);
      continue;
    }

    INFO_LOG_FMT(NETPLAY, "Received Wii save of title {:016x}.", title_id);

    // Header
    WiiSave::Header header;
    packet >> header.tid;
    packet >> header.banner_size;
    packet >> header.permissions;
    packet >> header.unk1;
    for (u8& byte : header.md5)
      packet >> byte;
    packet >> header.unk2;
    for (size_t i = 0; i < std::min<size_t>(header.banner_size, sizeof(header.banner)); i++)
      packet >> header.banner[i];

    // BkHeader
    WiiSave::BkHeader bk_header;
    packet >> bk_header.size;
    packet >> bk_header.magic;
    packet >> bk_header.ngid;
    packet >> bk_header.number_of_files;
    packet >> bk_header.size_of_files;
    packet >> bk_header.unk1;
    packet >> bk_header.unk2;
    packet >> bk_header.total_size;
    for (u8& byte : bk_header.unk3)
      packet >> byte;
    packet >> bk_header.tid;
    for (u8& byte : bk_header.mac_address)
      packet >> byte;

    // Files
    std::vector<WiiSave::Storage::SaveFile> files;
    for (u32 i = 0; i < bk_header.number_of_files; i++)
    {
      WiiSave::Storage::SaveFile file;
      packet >> file.mode >> file.attributes;
      packet >> file.type;
      packet >> file.path;

      INFO_LOG_FMT(NETPLAY, "Received Wii save data of type {} at {}", static_cast<u8>(file.type),
                   file.path);

      if (file.type == WiiSave::Storage::SaveFile::Type::File)
      {
        auto buffer = DecompressPacketIntoBuffer(packet);
        if (!buffer)
        {
          SyncSaveDataResponse(false);
          return;
        }

        file.data = std::move(*buffer);
      }

      files.push_back(std::move(file));
    }

    if (!save->WriteHeader(header) || !save->WriteBkHeader(bk_header) || !save->WriteFiles(files))
    {
      PanicAlertFmtT("Failed to write Wii save.");
      SyncSaveDataResponse(false);
      return;
    }
  }

  bool has_redirected_save;
  packet >> has_redirected_save;
  if (has_redirected_save)
  {
    INFO_LOG_FMT(NETPLAY, "Received redirected save.");
    if (!DecompressPacketIntoFolder(packet, redirect_path))
    {
      PanicAlertFmtT("Failed to write redirected save.");
      SyncSaveDataResponse(false);
      return;
    }
  }

  SetWiiSyncData(std::move(temp_fs), std::move(titles), std::move(redirect_path));
  SyncSaveDataResponse(true);
}

void NetPlayClient::OnSyncSaveDataGBA(sf::Packet& packet)
{
  u8 slot;
  packet >> slot;

  INFO_LOG_FMT(NETPLAY, "Received GBA save for slot {}.", slot);

  const std::string path =
      fmt::format("{}{}{}.sav", File::GetUserPath(D_GBAUSER_IDX), GBA_SAVE_NETPLAY, slot + 1);
  if (File::Exists(path) && !File::Delete(path))
  {
    PanicAlertFmtT("Failed to delete NetPlay GBA{0} save file. Verify your write permissions.",
                   slot + 1);
    SyncSaveDataResponse(false);
    return;
  }

  const bool success = DecompressPacketIntoFile(packet, path);
  SyncSaveDataResponse(success);
}

void NetPlayClient::OnSyncCodes(sf::Packet& packet)
{
  // Receive Data Packet
  SyncCodeID sub_id;
  packet >> sub_id;

  INFO_LOG_FMT(NETPLAY, "Processing OnSyncCodes sub id: {}", static_cast<u8>(sub_id));

  // Check Which Operation to Perform with This Packet
  switch (sub_id)
  {
  case SyncCodeID::Notify:
    OnSyncCodesNotify();
    break;

  case SyncCodeID::NotifyGecko:
    OnSyncCodesNotifyGecko(packet);
    break;

  case SyncCodeID::GeckoData:
    OnSyncCodesDataGecko(packet);
    break;

  case SyncCodeID::NotifyAR:
    OnSyncCodesNotifyAR(packet);
    break;

  case SyncCodeID::ARData:
    OnSyncCodesDataAR(packet);
    break;

  default:
    PanicAlertFmtT("Unknown SYNC_CODES message received with id: {0}", static_cast<u8>(sub_id));
    break;
  }
}

void NetPlayClient::OnSyncCodesNotify()
{
  // Set both codes as unsynced
  m_sync_gecko_codes_complete = false;
  m_sync_ar_codes_complete = false;
}

void NetPlayClient::OnSyncCodesNotifyGecko(sf::Packet& packet)
{
  // Return if this is the host
  if (m_local_player->IsHost())
    return;

  // Receive Number of Codelines to Receive
  packet >> m_sync_gecko_codes_count;

  m_sync_gecko_codes_success_count = 0;

  INFO_LOG_FMT(NETPLAY, "Receiving {} Gecko codelines", m_sync_gecko_codes_count);

  // Check if no codes to sync, if so return as finished
  if (m_sync_gecko_codes_count == 0)
  {
    m_sync_gecko_codes_complete = true;
    SyncCodeResponse(true);
  }
  else
  {
    m_dialog->AppendChat(Common::GetStringT("Synchronizing Gecko codes..."));
  }
}

void NetPlayClient::OnSyncCodesDataGecko(sf::Packet& packet)
{
  // Return if this is the host
  if (m_local_player->IsHost())
    return;

  std::vector<Gecko::GeckoCode> synced_codes;
  synced_codes.reserve(m_sync_gecko_codes_count);

  Gecko::GeckoCode gcode{};
  gcode.name = "Synced Codes";
  gcode.enabled = true;

  // Receive code contents from packet
  for (u32 i = 0; i < m_sync_gecko_codes_count; i++)
  {
    Gecko::GeckoCode::Code new_code;
    packet >> new_code.address;
    packet >> new_code.data;

    INFO_LOG_FMT(NETPLAY, "Received {:08x} {:08x}", new_code.address, new_code.data);

    gcode.codes.push_back(std::move(new_code));

    if (++m_sync_gecko_codes_success_count >= m_sync_gecko_codes_count)
    {
      m_sync_gecko_codes_complete = true;
      SyncCodeResponse(true);
    }
  }

  // Add gcode containing all codes to Gecko Code vector
  synced_codes.push_back(std::move(gcode));

  // Clear Vector if received 0 codes (match host's end when using no codes)
  if (m_sync_gecko_codes_count == 0)
    synced_codes.clear();

  // Copy this to the vector located in GeckoCode.cpp
  Gecko::UpdateSyncedCodes(synced_codes);
}

void NetPlayClient::OnSyncCodesNotifyAR(sf::Packet& packet)
{
  // Return if this is the host
  if (m_local_player->IsHost())
    return;

  // Receive Number of Codelines to Receive
  packet >> m_sync_ar_codes_count;

  m_sync_ar_codes_success_count = 0;

  INFO_LOG_FMT(NETPLAY, "Receiving {} AR codelines", m_sync_ar_codes_count);

  // Check if no codes to sync, if so return as finished
  if (m_sync_ar_codes_count == 0)
  {
    m_sync_ar_codes_complete = true;
    SyncCodeResponse(true);
  }
  else
  {
    m_dialog->AppendChat(Common::GetStringT("Synchronizing AR codes..."));
  }
}

void NetPlayClient::OnSyncCodesDataAR(sf::Packet& packet)
{
  // Return if this is the host
  if (m_local_player->IsHost())
    return;

  std::vector<ActionReplay::ARCode> synced_codes;
  synced_codes.reserve(m_sync_ar_codes_count);

  ActionReplay::ARCode arcode{};
  arcode.name = "Synced Codes";
  arcode.enabled = true;

  // Receive code contents from packet
  for (u32 i = 0; i < m_sync_ar_codes_count; i++)
  {
    ActionReplay::AREntry new_code;
    packet >> new_code.cmd_addr;
    packet >> new_code.value;

    INFO_LOG_FMT(NETPLAY, "Received {:08x} {:08x}", new_code.cmd_addr, new_code.value);
    arcode.ops.push_back(new_code);

    if (++m_sync_ar_codes_success_count >= m_sync_ar_codes_count)
    {
      m_sync_ar_codes_complete = true;
      SyncCodeResponse(true);
    }
  }

  // Add arcode containing all codes to AR Code vector
  synced_codes.push_back(std::move(arcode));

  // Clear Vector if received 0 codes (match host's end when using no codes)
  if (m_sync_ar_codes_count == 0)
    synced_codes.clear();

  // Copy this to the vector located in ActionReplay.cpp
  ActionReplay::UpdateSyncedCodes(synced_codes);
}

void NetPlayClient::OnComputeGameDigest(sf::Packet& packet)
{
  SyncIdentifier sync_identifier;
  ReceiveSyncIdentifier(packet, sync_identifier);

  ComputeGameDigest(sync_identifier);
}

void NetPlayClient::OnGameDigestProgress(sf::Packet& packet)
{
  PlayerId pid;
  int progress;
  packet >> pid;
  packet >> progress;

  m_dialog->SetGameDigestProgress(pid, progress);
}

void NetPlayClient::OnGameDigestResult(sf::Packet& packet)
{
  PlayerId pid;
  std::string result;
  packet >> pid;
  packet >> result;

  m_dialog->SetGameDigestResult(pid, result);
}

void NetPlayClient::OnGameDigestError(sf::Packet& packet)
{
  PlayerId pid;
  std::string error;
  packet >> pid;
  packet >> error;

  m_dialog->SetGameDigestResult(pid, error);
}

void NetPlayClient::OnGameDigestAbort()
{
  m_should_compute_game_digest = false;
  m_dialog->AbortGameDigest();
}

void NetPlayClient::Send(const sf::Packet& packet, const u8 channel_id)
{
  Common::ENet::SendPacket(m_server, packet, channel_id);
}

void NetPlayClient::DisplayPlayersPing()
{
  if (!Config::Get(Config::GFX_SHOW_NETPLAY_PING))
    return;

  OSD::AddTypedMessage(OSD::MessageType::NetPlayPing, fmt::format("Ping: {}", GetPlayersMaxPing()),
                       OSD::Duration::SHORT, OSD::Color::CYAN);
}

// Slippi's ping line (SlippiNetplayClient, ack packets): the last round trip to the opponent in ms,
// cyan, refreshed every PING_DISPLAY_INTERVAL frames and left up for Duration::NORMAL, so it stays
// on screen through the match and fades out after it.
void NetPlayClient::DisplayGekkoPing(u32 frame)
{
  if (frame < m_gekko_next_ping_display_frame)
    return;
  m_gekko_next_ping_display_frame = frame + PING_DISPLAY_INTERVAL;

  if (!Config::Get(Config::GFX_SHOW_NETPLAY_PING) || m_gekko_remote_handle < 0)
    return;

  GekkoNetworkStats stats{};
  gekko_network_stats(m_gekko_session, m_gekko_remote_handle, &stats);
  OSD::AddTypedMessage(OSD::MessageType::NetPlayPing, fmt::format("Ping: {}", stats.last_ping),
                       OSD::Duration::NORMAL, OSD::Color::CYAN);
}

u32 NetPlayClient::GetPlayersMaxPing() const
{
  return std::ranges::max_element(m_players, {}, [](const auto& kv) { return kv.second.ping; })
      ->second.ping;
}

void NetPlayClient::Disconnect()
{
  ENetEvent netEvent;
  m_connecting = false;
  m_connection_state = ConnectionState::Failure;
  if (m_server)
    enet_peer_disconnect(m_server, 0);
  else
    return;

  while (enet_host_service(m_client, &netEvent, 3000) > 0)
  {
    switch (netEvent.type)
    {
    case ENET_EVENT_TYPE_RECEIVE:
      enet_packet_destroy(netEvent.packet);
      break;
    case ENET_EVENT_TYPE_DISCONNECT:
      m_server = nullptr;
      return;
    default:
      break;
    }
  }
  // didn't disconnect gracefully force disconnect
  enet_peer_reset(m_server);
  m_server = nullptr;
}

void NetPlayClient::SendAsync(sf::Packet&& packet, const u8 channel_id)
{
  {
    std::lock_guard lkq(m_crit.async_queue_write);
    m_async_queue.Push(AsyncQueueEntry{std::move(packet), channel_id});
  }
  Common::ENet::WakeupThread(m_client);
}

// called from ---NETPLAY--- thread
void NetPlayClient::ThreadFunc()
{
  INFO_LOG_FMT(NETPLAY, "NetPlayClient starting.");

  Common::QoSSession qos_session;
  if (Config::Get(Config::NETPLAY_ENABLE_QOS))
  {
    qos_session = Common::QoSSession(m_server);

    if (qos_session.Successful())
    {
      m_dialog->AppendChat(
          Common::GetStringT("Quality of Service (QoS) was successfully enabled."));
    }
    else
    {
      m_dialog->AppendChat(Common::GetStringT("Quality of Service (QoS) couldn't be enabled."));
    }
  }

  while (m_do_loop.IsSet())
  {
    ENetEvent netEvent;
    int net;
    if (m_traversal_client)
      m_traversal_client->HandleResends();
    net = enet_host_service(m_client, &netEvent, 250);
    while (!m_async_queue.Empty())
    {
      INFO_LOG_FMT(NETPLAY, "Processing async queue event.");
      {
        auto& e = m_async_queue.Front();
        Send(e.packet, e.channel_id);
      }
      INFO_LOG_FMT(NETPLAY, "Processing async queue event done.");
      m_async_queue.Pop();
    }
    FlushGekkoSendQueue();

    if (net > 0)
    {
      sf::Packet rpac;
      switch (netEvent.type)
      {
      case ENET_EVENT_TYPE_CONNECT:
        INFO_LOG_FMT(NETPLAY, "enet_host_service: connect event");
        break;
      case ENET_EVENT_TYPE_RECEIVE:
        INFO_LOG_FMT(NETPLAY, "enet_host_service: receive event");

        // Check if this is a GekkoNet packet
        if (netEvent.channelID == GEKKONET_CHANNEL && netEvent.packet->dataLength >= 2 &&
            netEvent.packet->data[0] == GEKKO_CONTROL_MAGIC)
        {
          OnGekkoControlPacket(netEvent.packet->data, netEvent.packet->dataLength);
        }
        else if (netEvent.channelID == GEKKONET_CHANNEL)
        {
          // Queue packet for GekkoNet adapter
          std::lock_guard<std::mutex> lock(m_gekko_packet_mutex);
          m_gekko_received_packets.emplace_back(
            reinterpret_cast<const char*>(netEvent.packet->data),
            netEvent.packet->dataLength);
        }
        else
        {
          // Normal NetPlay packet
          rpac.append(netEvent.packet->data, netEvent.packet->dataLength);
          OnData(rpac);
        }

        enet_packet_destroy(netEvent.packet);
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        INFO_LOG_FMT(NETPLAY, "enet_host_service: disconnect event");

        m_dialog->OnConnectionLost();

        if (m_is_running.IsSet())
          StopGame();

        break;
      default:
        // not a valid switch case due to not technically being part of the enum
        if (static_cast<int>(netEvent.type) == Common::ENet::SKIPPABLE_EVENT)
          INFO_LOG_FMT(NETPLAY, "enet_host_service: skippable packet event");
        else
          ERROR_LOG_FMT(NETPLAY, "enet_host_service: unknown event type: {}",
                        static_cast<int>(netEvent.type));
        break;
      }
    }
    else if (net == 0)
    {
      INFO_LOG_FMT(NETPLAY, "enet_host_service: no event occurred");
    }
    else
    {
      ERROR_LOG_FMT(NETPLAY, "enet_host_service error: {}", net);
    }
  }

  INFO_LOG_FMT(NETPLAY, "NetPlayClient shutting down.");

  Disconnect();
  return;
}

// called from ---GUI--- thread
std::vector<const Player*> NetPlayClient::GetPlayers()
{
  std::lock_guard lkp(m_crit.players);
  std::vector<const Player*> players;

  for (const auto& pair : m_players)
    players.push_back(&pair.second);

  return players;
}

const NetSettings& NetPlayClient::GetNetSettings() const
{
  return m_net_settings;
}

// called from ---GUI--- thread
void NetPlayClient::SendChatMessage(const std::string& msg)
{
  sf::Packet packet;
  packet << MessageID::ChatMessage;
  packet << msg;

  SendAsync(std::move(packet));
}

// called from ---CPU--- thread
void NetPlayClient::AddPadStateToPacket(const int in_game_pad, const GCPadStatus& pad,
                                        sf::Packet& packet)
{
  packet << static_cast<PadIndex>(in_game_pad);
  packet << pad.button;
  if (!m_net_settings.gba_config[in_game_pad].enabled)
  {
    packet << pad.analogA << pad.analogB << pad.stickX << pad.stickY << pad.substickX
           << pad.substickY << pad.triggerLeft << pad.triggerRight << pad.isConnected;
  }
}

// called from ---CPU--- thread
void NetPlayClient::AddWiimoteStateToPacket(int in_game_pad,
                                            const WiimoteEmu::SerializedWiimoteState& state,
                                            sf::Packet& packet)
{
  packet << static_cast<PadIndex>(in_game_pad);
  packet << state.length;
  for (size_t i = 0; i < state.length; ++i)
    packet << state.data[i];
}

// called from ---GUI--- thread
void NetPlayClient::SendStartGamePacket()
{
  sf::Packet packet;
  packet << MessageID::StartGame;
  packet << m_current_game;

  SendAsync(std::move(packet));
}

// called from ---GUI--- thread
void NetPlayClient::SendStopGamePacket()
{
  sf::Packet packet;
  packet << MessageID::StopGame;

  SendAsync(std::move(packet));
}

// called from ---GUI--- thread
bool NetPlayClient::StartGame(const std::string& path)
{
  std::lock_guard lkg(m_crit.game);
  SendStartGamePacket();

  if (m_is_running.IsSet())
  {
    PanicAlertFmtT("Game is already running!");
    return false;
  }

  m_timebase_frame = 0;
  m_current_golfer = 1;
  m_wait_on_input = false;

  m_is_running.Set();
  NetPlay_Enable(this);

  ClearBuffers();

  m_first_pad_status_received.fill(false);

  if (m_dialog->IsRecording())
  {
    auto& movie = Core::System::GetInstance().GetMovie();
    if (movie.IsReadOnly())
      movie.SetReadOnly(false);

    Movie::ControllerTypeArray controllers{};
    Movie::WiimoteEnabledArray wiimotes{};
    for (unsigned int i = 0; i < 4; ++i)
    {
      if (m_net_settings.pad_map[i] > 0 && m_net_settings.gba_config[i].enabled)
        controllers[i] = Movie::ControllerType::GBA;
      else if (m_net_settings.pad_map[i] > 0)
        controllers[i] = Movie::ControllerType::GC;
      else
        controllers[i] = Movie::ControllerType::None;
      wiimotes[i] = m_net_settings.wiimote_map[i] > 0;
    }
    movie.BeginRecordingInput(controllers, wiimotes);
  }

  // boot game
  auto boot_session_data = std::make_unique<BootSessionData>();

  INFO_LOG_FMT(NETPLAY,
               "Setting Wii sync data: has FS {}, sync_titles = {:016x}, redirect folder = {}",
               !!m_wii_sync_fs, fmt::join(m_wii_sync_titles, ", "), m_wii_sync_redirect_folder);

  boot_session_data->SetWiiSyncData(std::move(m_wii_sync_fs), std::move(m_wii_sync_titles),
                                    std::move(m_wii_sync_redirect_folder), [] {
                                      // on emulation end clean up the Wii save sync directory --
                                      // see OnSyncSaveDataWii()
                                      const std::string wii_path = File::GetUserPath(D_USER_IDX) +
                                                                   "Wii" GC_MEMCARD_NETPLAY DIR_SEP;
                                      if (File::Exists(wii_path))
                                        File::DeleteDirRecursively(wii_path);
                                      const std::string redirect_path =
                                          File::GetUserPath(D_USER_IDX) +
                                          "Redirect" GC_MEMCARD_NETPLAY DIR_SEP;
                                      if (File::Exists(redirect_path))
                                        File::DeleteDirRecursively(redirect_path);
                                    });

  m_net_settings.local_player_id = m_local_player->pid;
  boot_session_data->SetNetplaySettings(std::make_unique<NetPlay::NetSettings>(m_net_settings));

  m_dialog->BootGame(path, std::move(boot_session_data));

  return true;
}

void NetPlayClient::SyncSaveDataResponse(const bool success)
{
  m_dialog->AppendChat(success ? Common::GetStringT("Data received!") :
                                 Common::GetStringT("Error processing data."));

  if (success)
  {
    if (++m_sync_save_data_success_count >= m_sync_save_data_count)
    {
      sf::Packet response_packet;
      response_packet << MessageID::SyncSaveData;
      response_packet << SyncSaveDataID::Success;
      Send(response_packet);
    }
  }
  else
  {
    sf::Packet response_packet;
    response_packet << MessageID::SyncSaveData;
    response_packet << SyncSaveDataID::Failure;
    Send(response_packet);
  }
}

void NetPlayClient::SyncCodeResponse(const bool success)
{
  // If something failed, immediately report back that code sync failed
  if (!success)
  {
    m_dialog->AppendChat(Common::GetStringT("Error processing codes."));

    sf::Packet response_packet;
    response_packet << MessageID::SyncCodes;
    response_packet << SyncCodeID::Failure;

    Send(response_packet);
    return;
  }

  // If both gecko and AR codes have completely finished transferring, report back as successful
  if (m_sync_gecko_codes_complete && m_sync_ar_codes_complete)
  {
    m_dialog->AppendChat(Common::GetStringT("Codes received!"));

    sf::Packet response_packet;
    response_packet << MessageID::SyncCodes;
    response_packet << SyncCodeID::Success;

    Send(response_packet);
  }
}

// called from ---GUI--- thread
bool NetPlayClient::ChangeGame(const std::string&)
{
  return true;
}

// called from ---NETPLAY--- thread
void NetPlayClient::ClearBuffers()
{
  // clear pad buffers, Clear method isn't thread safe
  for (unsigned int i = 0; i < 4; ++i)
  {
    while (m_pad_buffer[i].Size())
      m_pad_buffer[i].Pop();

    while (m_wiimote_buffer[i].Size())
      m_wiimote_buffer[i].Pop();
  }

  // Clear SI override state for GekkoNet (EXI-style injection)
  s_override_active.store(false, std::memory_order_release);
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnTraversalStateChanged()
{
  const Common::TraversalClient::State state = m_traversal_client->GetState();

  if (m_connection_state == ConnectionState::WaitingForTraversalClientConnection &&
      state == Common::TraversalClient::State::Connected)
  {
    m_connection_state = ConnectionState::WaitingForTraversalClientConnectReady;
    m_traversal_client->ConnectToClient(m_host_spec);
  }
  else if (m_connection_state != ConnectionState::Failure &&
           state == Common::TraversalClient::State::Failure)
  {
    Disconnect();
    m_dialog->OnTraversalError(m_traversal_client->GetFailureReason());
  }
  m_dialog->OnTraversalStateChanged(state);
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnConnectReady(ENetAddress addr)
{
  if (m_connection_state == ConnectionState::WaitingForTraversalClientConnectReady)
  {
    m_connection_state = ConnectionState::Connecting;
    enet_host_connect(m_client, &addr, CHANNEL_COUNT, 0);
  }
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnConnectFailed(Common::TraversalConnectFailedReason reason)
{
  m_connecting = false;
  m_connection_state = ConnectionState::Failure;
  switch (reason)
  {
  case Common::TraversalConnectFailedReason::ClientDidntRespond:
    PanicAlertFmtT("Traversal server timed out connecting to the host");
    break;
  case Common::TraversalConnectFailedReason::ClientFailure:
    PanicAlertFmtT("Server rejected traversal attempt");
    break;
  case Common::TraversalConnectFailedReason::NoSuchClient:
    PanicAlertFmtT("Invalid host");
    break;
  default:
    PanicAlertFmtT("Unknown error {0:x}", static_cast<int>(reason));
    break;
  }
}

bool NetPlayClient::GetOverrideInput(int pad_num, gfPadStatus* status)
{
  if (!s_override_active || pad_num < 0 || pad_num >= MAX_NUM_PLAYERS)
    return false;
  *status = s_override_pads[pad_num];
  return true;
}

// called from ---CPU--- thread
bool NetPlayClient::GetNetPads(const int pad_nb, const bool batching, GCPadStatus* pad_status)
{
  if (m_net_settings.m_RollbackMode)
  {
    if (m_use_gekko_netplay && m_gekko_session)
    {
      if (!m_gekko_session_started)
      {
        const int local_pad = InGamePadToLocalPad(pad_nb);
        if (local_pad < 4)
        {
          *pad_status = Pad::GetStatus(local_pad);
        }
        else
        {
          *pad_status = GCPadStatus();
        }
        return true;
      }

        // On normal passes, updateLowGC receives local input through its usual PADRead/GetNetPads
        // path. The post-updateLowGC hook captures that sample for GekkoNet's later submission.
        // Remote input comes from the current GekkoNet advance event through this same path.
      const auto& pad_mapping = GetPadMapping();
      // The local port always reads the live controller, resimulated passes included: an SI poll
      // during a resimulated iteration can latch the data the presented pass's pad read then
      // returns, and that read is the sample GekkoNet gets. The post-read hook replaces every
      // player's slot with GekkoNet's input anyway.
      if (m_local_player && pad_nb >= 0 && pad_nb < static_cast<int>(pad_mapping.size()) &&
          pad_mapping[pad_nb] == m_local_player->pid)
      {
        const int local_pad = InGamePadToLocalPad(pad_nb);
        if (local_pad >= 0 && local_pad < 4)
        {
            if (Config::Get(Config::GetInfoForSIDevice(pad_nb)) ==
                SerialInterface::SIDEVICE_WIIU_ADAPTER)
            {
              *pad_status = GCAdapter::Input(local_pad);
              Harness::ApplyPadOverride(local_pad, pad_status);
            }
            else
            {
              *pad_status = Pad::GetStatus(local_pad);
            }
            return true;
        }
      }

      const int iteration_index = m_gekko_current_iteration;
      if (pad_nb >= 0 && pad_nb < MAX_NUM_PLAYERS && iteration_index >= 0 &&
          iteration_index < m_gekko_pending_ops.adv_count)
      {
        *pad_status = ToGCPadStatus(m_gekko_pending_ops.adv_pads[pad_nb][iteration_index]);
        return true;
      }

      if (pad_nb >= 0 && pad_nb < MAX_NUM_PLAYERS && m_gekko_has_last_synced_pad[pad_nb])
      {
        *pad_status = ToGCPadStatus(m_gekko_last_synced_pads[pad_nb]);
        return true;
      }

      *pad_status = GCPadStatus();
      return true;
    }
    *pad_status = GCPadStatus();
    return false;
  }
  else
  {
    // The interface for this is extremely silly.
    //
    // Imagine a physical device that links three GameCubes together
    // and emulates NetPlay that way. Which GameCube controls which
    // in-game controllers can be configured on the device (m_pad_map)
    // but which sockets on each individual GameCube should be used
    // to control which players? The solution that Dolphin uses is
    // that we hardcode the knowledge that they go in order, so if
    // you have a 3P game with three GameCubes, then every single
    // controller should be plugged into slot 1.
    //
    // If you have a 4P game, then one of the GameCubes will have
    // a controller plugged into slot 1, and another in slot 2.
    //
    // The slot number is the "local" pad number, and what player
    // it actually means is the "in-game" pad number.

    // When the 1st in-game pad is polled and batching is set, the
    // others will be polled as well. To reduce latency, we poll all
    // local controllers at once and then send the status to the other
    // clients.
    //
    // Batching is enabled when polled from VI. If batching is not
    // enabled, the poll is probably from MMIO, which can poll any
    // specific pad arbitrarily. In this case, we poll just that pad
    // and send it.

    // When here when told to so we don't deadlock in certain situations
    while (m_wait_on_input)
    {
      if (!m_is_running.IsSet())
      {
        return false;
      }

      if (m_wait_on_input_received)
      {
        // Tell the server we've acknowledged the message
        sf::Packet spac;
        spac << MessageID::GolfPrepare;
        Send(spac);

        m_wait_on_input_received = false;
      }

      m_wait_on_input_event.Wait();
    }

    if (IsFirstInGamePad(pad_nb) && batching)
    {
      sf::Packet packet;
      packet << MessageID::PadData;

      bool send_packet = false;
      const int num_local_pads = NumLocalPads();
      for (int local_pad = 0; local_pad < num_local_pads; local_pad++)
      {
        send_packet = PollLocalPad(local_pad, packet) || send_packet;
      }

      if (send_packet)
        SendAsync(std::move(packet));

      if (m_host_input_authority)
        SendPadHostPoll(-1);
    }

    if (!batching)
    {
      const int local_pad = InGamePadToLocalPad(pad_nb);
      if (local_pad < 4)
      {
        sf::Packet packet;
        packet << MessageID::PadData;
        if (PollLocalPad(local_pad, packet))
          SendAsync(std::move(packet));
      }

      if (m_host_input_authority)
        SendPadHostPoll(pad_nb);
    }

    if (m_host_input_authority)
    {
      if (m_local_player->pid != m_current_golfer)
      {
        // CoreTiming acts funny and causes what looks like frame skip if
        // we toggle the emulation speed too quickly, so to prevent this
        // we wait until the buffer has been over for at least 1 second.

        const bool buffer_over_target = m_pad_buffer[pad_nb].Size() > m_target_buffer_size + 1;
        if (!buffer_over_target)
          m_buffer_under_target_last = std::chrono::steady_clock::now();

        std::chrono::duration<double> time_diff =
            std::chrono::steady_clock::now() - m_buffer_under_target_last;
        if (time_diff.count() >= 1.0 || !buffer_over_target)
        {
          // run fast if the buffer is overfilled, otherwise run normal speed
          Config::SetCurrent(Config::MAIN_EMULATION_SPEED, buffer_over_target ? 0.0f : 1.0f);
        }
      }
      else
      {
        // Set normal speed when we're the host, otherwise it can get stuck at unlimited
        Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 1.0f);
      }
    }

    // Now, we either use the data pushed earlier, or wait for the
    // other clients to send it to us
    while (m_pad_buffer[pad_nb].Size() == 0)
    {
      if (!m_is_running.IsSet())
      {
        return false;
      }

      m_gc_pad_event.Wait();
    }

    m_pad_buffer[pad_nb].Pop(*pad_status);
  }
  auto& movie = Core::System::GetInstance().GetMovie();
  if (movie.IsRecordingInput())
  {
    movie.RecordInput(pad_status, pad_nb);
    movie.InputUpdate();
  }
  else
  {
    movie.CheckPadStatus(pad_status, pad_nb);
  }

  return true;
}

u64 NetPlayClient::GetInitialRTCValue() const
{
  return m_initial_rtc;
}

// called from ---CPU--- thread
bool NetPlayClient::WiimoteUpdate(const std::span<WiimoteDataBatchEntry>& entries)
{
  for (const WiimoteDataBatchEntry& entry : entries)
  {
    const int local_wiimote = InGameWiimoteToLocalWiimote(entry.wiimote);
    DEBUG_LOG_FMT(NETPLAY,
                  "Entering WiimoteUpdate() with wiimote {}, local_wiimote {}, state [{:02x}]",
                  entry.wiimote, local_wiimote,
                  fmt::join(std::span(entry.state->data.data(), entry.state->length), ", "));
    if (local_wiimote < 4)
    {
      sf::Packet packet;
      packet << MessageID::WiimoteData;
      if (AddLocalWiimoteToBuffer(local_wiimote, *entry.state, packet))
        SendAsync(std::move(packet));
    }

    // Now, we either use the data pushed earlier, or wait for the
    // other clients to send it to us
    while (m_wiimote_buffer[entry.wiimote].Size() == 0)
    {
      if (!m_is_running.IsSet())
      {
        return false;
      }

      m_wii_pad_event.Wait();
    }

    m_wiimote_buffer[entry.wiimote].Pop(*entry.state);

    DEBUG_LOG_FMT(NETPLAY, "Exiting WiimoteUpdate() with wiimote {}, state [{:02x}]", entry.wiimote,
                  fmt::join(std::span(entry.state->data.data(), entry.state->length), ", "));
  }

  return true;
}

bool NetPlayClient::PollLocalPad(const int local_pad, sf::Packet& packet)
{
  const int ingame_pad = LocalPadToInGamePad(local_pad);
  bool data_added = false;
  GCPadStatus pad_status;

  if (m_net_settings.gba_config[ingame_pad].enabled)
  {
    pad_status = Pad::GetGBAStatus(local_pad);
  }
  else if (Config::Get(Config::GetInfoForSIDevice(ingame_pad)) ==
           SerialInterface::SIDEVICE_WIIU_ADAPTER)
  {
    pad_status = GCAdapter::Input(local_pad);
    Harness::ApplyPadOverride(local_pad, &pad_status);
  }
  else
  {
    pad_status = Pad::GetStatus(local_pad);
  }
  if (m_net_settings.m_RollbackMode)
  {
    ERROR_LOG_FMT(NETPLAY, "PollLocalPad called in rollback mode - this should not happen!");
    return false;
  }
  else
  {
    if (m_host_input_authority)
    {
      if (m_local_player->pid != m_current_golfer)
      {
        // add to packet
        AddPadStateToPacket(ingame_pad, pad_status, packet);
        data_added = true;
      }
      else
      {
        // set locally
        m_last_pad_status[ingame_pad] = pad_status;
        m_first_pad_status_received[ingame_pad] = true;
      }
    }
    else
    {
      // adjust the buffer either up or down
      // inserting multiple padstates or dropping states
      while (m_pad_buffer[ingame_pad].Size() <= m_target_buffer_size)
      {
        // add to buffer
        m_pad_buffer[ingame_pad].Push(pad_status);

        // add to packet
        AddPadStateToPacket(ingame_pad, pad_status, packet);
        data_added = true;
      }
    }
  }
  
  return data_added;
}

bool NetPlayClient::AddLocalWiimoteToBuffer(const int local_wiimote,
                                            const WiimoteEmu::SerializedWiimoteState& state,
                                            sf::Packet& packet)
{
  const int ingame_pad = LocalWiimoteToInGameWiimote(local_wiimote);
  bool data_added = false;

  // adjust the buffer either up or down
  // inserting multiple padstates or dropping states
  while (m_wiimote_buffer[ingame_pad].Size() <= m_target_buffer_size)
  {
    // add to buffer
    m_wiimote_buffer[ingame_pad].Push(state);

    // add to packet
    AddWiimoteStateToPacket(ingame_pad, state, packet);
    data_added = true;
  }

  return data_added;
}

void NetPlayClient::SendPadHostPoll(const PadIndex pad_num)
{
  // Here we handle polling for the Host Input Authority and Golf modes. Pad data is "polled" from
  // the most recent data received for the given pad. Passing pad_num < 0 will poll all assigned
  // pads (used for batched polls), while 0..3 will poll the respective pad (used for MMIO polls).
  // See GetNetPads for more details.
  //
  // If the local buffer is non-empty, we skip actually buffering and sending new pad data, this way
  // don't end up with permanent local latency. It does create a period of time where no inputs are
  // accepted, but under typical circumstances this is not noticeable.
  //
  // Additionally, we wait until some actual pad data has been received before buffering and sending
  // it, otherwise controllers get calibrated wrongly with the default values of GCPadStatus.

  if (m_local_player->pid != m_current_golfer)
    return;

  sf::Packet packet;
  packet << MessageID::PadHostData;

  if (pad_num < 0)
  {
    for (size_t i = 0; i < m_net_settings.pad_map.size(); i++)
    {
      if (m_net_settings.pad_map[i] <= 0)
        continue;

      while (!m_first_pad_status_received[i])
      {
        if (!m_is_running.IsSet())
          return;

        m_first_pad_status_received_event.Wait();
      }
    }

    for (size_t i = 0; i < m_net_settings.pad_map.size(); i++)
    {
      if (m_net_settings.pad_map[i] == 0 || m_pad_buffer[i].Size() > 0)
        continue;

      const GCPadStatus& pad_status = m_last_pad_status[i];
      m_pad_buffer[i].Push(pad_status);
      AddPadStateToPacket(static_cast<int>(i), pad_status, packet);
    }
  }
  else if (m_net_settings.pad_map[pad_num] != 0)
  {
    while (!m_first_pad_status_received[pad_num])
    {
      if (!m_is_running.IsSet())
        return;

      m_first_pad_status_received_event.Wait();
    }

    if (m_pad_buffer[pad_num].Size() == 0)
    {
      const GCPadStatus& pad_status = m_last_pad_status[pad_num];
      m_pad_buffer[pad_num].Push(pad_status);
      AddPadStateToPacket(pad_num, pad_status, packet);
    }
  }

  SendAsync(std::move(packet));
}

void NetPlayClient::InvokeStop()
{
  m_is_running.Clear();

  // stop waiting for input
  m_gc_pad_event.Set();
  m_wii_pad_event.Set();
  m_first_pad_status_received_event.Set();
  m_wait_on_input_event.Set();
}

// called from ---GUI--- thread and ---NETPLAY--- thread (client side)
bool NetPlayClient::StopGame()
{
  InvokeStop();

  // The GekkoNet session is not destroyed here but by the next game's InitGekkoSession or the
  // destructor. A stop arrives on the netplay thread (OnStopGame) while the CPU thread is still
  // in the middle of a frame that uses the session: a queued save holds a checksum pointer into
  // GekkoNet's memory and writes it at the end of the frame. Destroying the session here crashed
  // the stopping peer with an access violation (every time a dropped peer ended the game).

  NetPlay_Disable();

  // stop game
  m_dialog->StopGame();

  return true;
}

// called from ---GUI--- thread
void NetPlayClient::Stop()
{
  if (!m_is_running.IsSet())
    return;

  InvokeStop();

  // Tell the server to stop if we have a pad mapped in game.
  if (LocalPlayerHasControllerMapped())
    SendStopGamePacket();
  else
    StopGame();
}

void NetPlayClient::RequestStopGame()
{
  // Tell the server to stop if we have a pad mapped in game.
  if (LocalPlayerHasControllerMapped())
    SendStopGamePacket();
}

void NetPlayClient::SendPowerButtonEvent()
{
  sf::Packet packet;
  packet << MessageID::PowerButton;
  SendAsync(std::move(packet));
}

void NetPlayClient::RequestGolfControl(const PlayerId pid)
{
  if (!m_host_input_authority || !m_net_settings.golf_mode)
    return;

  sf::Packet packet;
  packet << MessageID::GolfRequest;
  packet << pid;
  SendAsync(std::move(packet));
}

void NetPlayClient::RequestGolfControl()
{
  RequestGolfControl(m_local_player->pid);
}

// called from ---GUI--- thread
std::string NetPlayClient::GetCurrentGolfer()
{
  std::lock_guard lkp(m_crit.players);
  if (const auto it = m_players.find(m_current_golfer); it != m_players.end())
    return it->second.name;
  return "";
}

// called from ---GUI--- thread
bool NetPlayClient::LocalPlayerHasControllerMapped() const
{
  return PlayerHasControllerMapped(m_local_player->pid);
}

bool NetPlayClient::IsFirstInGamePad(int ingame_pad) const
{
  return std::none_of(m_net_settings.pad_map.begin(), m_net_settings.pad_map.begin() + ingame_pad,
                      [](auto mapping) { return mapping > 0; });
}

int NetPlayClient::NumLocalPads() const
{
  return std::ranges::count(m_net_settings.pad_map, m_local_player->pid);
}

int NetPlayClient::NumLocalWiimotes() const
{
  return std::ranges::count(m_net_settings.wiimote_map, m_local_player->pid);
}

static int InGameToLocal(int ingame_pad, const PadMappingArray& pad_map, PlayerId local_player_pid)
{
  // not our pad
  if (pad_map[ingame_pad] != local_player_pid)
    return 4;

  int local_pad = 0;
  int pad = 0;

  for (; pad < ingame_pad; ++pad)
  {
    if (pad_map[pad] == local_player_pid)
      local_pad++;
  }

  return local_pad;
}

static int LocalToInGame(int local_pad, const PadMappingArray& pad_map, PlayerId local_player_pid)
{
  // Figure out which in-game pad maps to which local pad.
  // The logic we have here is that the local slots always
  // go in order.
  int local_pad_count = -1;
  int ingame_pad = 0;
  for (; ingame_pad < 4; ingame_pad++)
  {
    if (pad_map[ingame_pad] == local_player_pid)
      local_pad_count++;

    if (local_pad_count == local_pad)
      break;
  }

  return ingame_pad;
}

int NetPlayClient::InGamePadToLocalPad(int ingame_pad) const
{
  return InGameToLocal(ingame_pad, m_net_settings.pad_map, m_local_player->pid);
}

int NetPlayClient::LocalPadToInGamePad(int local_pad) const
{
  return LocalToInGame(local_pad, m_net_settings.pad_map, m_local_player->pid);
}

int NetPlayClient::InGameWiimoteToLocalWiimote(int ingame_wiimote) const
{
  return InGameToLocal(ingame_wiimote, m_net_settings.wiimote_map, m_local_player->pid);
}

int NetPlayClient::LocalWiimoteToInGameWiimote(int local_wiimote) const
{
  return LocalToInGame(local_wiimote, m_net_settings.wiimote_map, m_local_player->pid);
}

bool NetPlayClient::PlayerHasControllerMapped(const PlayerId pid) const
{
  const auto mapping_matches_player_id = [pid](const PlayerId& mapping) { return mapping == pid; };

  return std::ranges::any_of(m_net_settings.pad_map, mapping_matches_player_id) ||
         std::ranges::any_of(m_net_settings.wiimote_map, mapping_matches_player_id);
}

bool NetPlayClient::IsLocalPlayer(const PlayerId pid) const
{
  return pid == m_local_player->pid;
}

const PlayerId& NetPlayClient::GetLocalPlayerId() const
{
  return m_local_player->pid;
}

void NetPlayClient::SendGameStatus()
{
  sf::Packet packet;
  packet << MessageID::GameStatus;

  SyncIdentifierComparison result;
  m_dialog->FindGameFile(m_selected_game, &result);
  for (size_t i = 0; i < 4; ++i)
  {
    if (m_net_settings.gba_config[i].enabled && m_net_settings.gba_config[i].has_rom &&
        m_net_settings.gba_rom_paths[i].empty())
    {
      result = SyncIdentifierComparison::DifferentGame;
    }
  }

  packet << result;
  Send(packet);
}

void NetPlayClient::SendTimeBase()
{
  std::lock_guard lk(crit_netplay_client);

  if (!netplay_client)
    return;

  if (netplay_client->m_timebase_frame % 60 == 0)
  {
    const u64 timebase = Core::System::GetInstance().GetSystemTimers().GetFakeTimeBase();

    sf::Packet packet;
    packet << MessageID::TimeBase;
    packet << timebase;
    packet << netplay_client->m_timebase_frame;

    netplay_client->SendAsync(std::move(packet));
  }

  netplay_client->m_timebase_frame++;
}

bool NetPlayClient::DoAllPlayersHaveGame()
{
  std::lock_guard lkp(m_crit.players);

  return std::ranges::all_of(m_players, [](const auto& entry) {
    return entry.second.game_status == SyncIdentifierComparison::SameGame;
  });
}

static std::string SHA1Sum(const std::string& file_path,
                           const std::function<bool(int)>& report_progress)
{
  std::vector<u8> data(8 * 1024 * 1024);
  u64 read_offset = 0;

  std::unique_ptr<DiscIO::BlobReader> file(DiscIO::CreateBlobReader(file_path));
  u64 game_size = file->GetDataSize();

  auto ctx = Common::SHA1::CreateContext();

  while (read_offset < game_size)
  {
    size_t read_size = std::min(static_cast<u64>(data.size()), game_size - read_offset);
    if (!file->Read(read_offset, read_size, data.data()))
      return "";

    ctx->Update(data.data(), read_size);
    read_offset += read_size;

    int progress =
        static_cast<int>(static_cast<float>(read_offset) / static_cast<float>(game_size) * 100);
    if (!report_progress(progress))
      return "";
  }

  // Convert to hex
  return fmt::format("{:02x}", fmt::join(ctx->Finish(), ""));
}
SyncIdentifier NetPlayClient::GetBrawlFileIdentifier()
{
  return SyncIdentifier{{}, "Save", {}, {}, {}, {}};
}
void NetPlayClient::ComputeGameDigest(const SyncIdentifier& sync_identifier)
{
  if (m_should_compute_game_digest)
    return;

  m_dialog->ShowGameDigestDialog(sync_identifier.game_id);
  m_should_compute_game_digest = true;

  std::string file;
  if (sync_identifier == GetSDCardIdentifier())
    file = File::GetUserPath(F_WIISDCARDIMAGE_IDX);
  else if (auto game = m_dialog->FindGameFile(sync_identifier))
    file = game->GetFilePath();
  else if (sync_identifier == GetBrawlFileIdentifier())
    file = File::GetSysDirectory() + "Wii" + DIR_SEP + "title" + DIR_SEP + "00010000" + DIR_SEP +
           "52534245" + DIR_SEP + "data" + DIR_SEP + BRAWL_SAVE_FILE;

  if (file.empty() || !File::Exists(file))
  {
    sf::Packet packet;
    packet << MessageID::GameDigestError;
    packet << "file not found";
    Send(packet);
    return;
  }

  if (m_game_digest_thread.joinable())
    m_game_digest_thread.join();
  m_game_digest_thread = std::thread([this, file] {
    std::string sum = SHA1Sum(file, [&](int progress) {
      sf::Packet packet;
      packet << MessageID::GameDigestProgress;
      packet << progress;
      SendAsync(std::move(packet));

      return m_should_compute_game_digest;
    });

    sf::Packet packet;
    packet << MessageID::GameDigestResult;
    packet << sum;
    SendAsync(std::move(packet));
  });
}

const PadMappingArray& NetPlayClient::GetPadMapping() const
{
  return m_net_settings.pad_map;
}

const GBAConfigArray& NetPlayClient::GetGBAConfig() const
{
  return m_net_settings.gba_config;
}

const PadMappingArray& NetPlayClient::GetWiimoteMapping() const
{
  return m_net_settings.wiimote_map;
}

// called from ---GUI--- and ---NETPLAY--- thread
void NetPlayClient::AdjustPlayerPadBufferSize(u32 buffer)
{
  std::lock_guard<std::recursive_mutex> lkp(m_crit.players);

  m_local_player->buffer = buffer;
  if (m_local_player->buffer < m_minimum_buffer_size)
    m_local_player->buffer = m_minimum_buffer_size;

  // not needed on clients with host input authority
  if (!m_host_input_authority)
  {
    // tell clients to change buffer size
    sf::Packet spac;
    spac << MessageID::PadBufferPlayer;
    spac << m_local_player->buffer;

    SendAsync(std::move(spac));
  }

  m_dialog->OnPlayerPadBufferChanged(m_local_player->buffer);
}

void NetPlayClient::OnPadBufferMinimum(sf::Packet& packet)
{
  u32 size = 0;
  packet >> size;

  m_minimum_buffer_size = size;
  m_dialog->OnMinimumPadBufferChanged(size);

  if (m_local_player->buffer < m_minimum_buffer_size)
    AdjustPlayerPadBufferSize(m_minimum_buffer_size);
}

void NetPlayClient::OnPadBufferPlayer(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard<std::recursive_mutex> lkp(m_crit.players);
    packet >> m_players[pid].buffer;
  }
}

void NetPlayClient::AdjustMinimumPadBufferSize(const unsigned int size)
{
  m_minimum_buffer_size = size;
  m_dialog->OnMinimumPadBufferChanged(size);
}

void NetPlayClient::SetWiiSyncData(std::unique_ptr<IOS::HLE::FS::FileSystem> fs,
                                   std::vector<u64> titles, std::string redirect_folder)
{
  m_wii_sync_fs = std::move(fs);
  m_wii_sync_titles = std::move(titles);
  m_wii_sync_redirect_folder = std::move(redirect_folder);
}

SyncIdentifier NetPlayClient::GetSDCardIdentifier()
{
  return SyncIdentifier{{}, "sd", {}, {}, {}, {}};
}

std::string GetPlayerMappingString(PlayerId pid, const PadMappingArray& pad_map,
                                   const GBAConfigArray& gba_config,
                                   const PadMappingArray& wiimote_map)
{
  std::vector<size_t> gc_slots, gba_slots, wiimote_slots;
  for (size_t i = 0; i < pad_map.size(); ++i)
  {
    if (pad_map[i] == pid && !gba_config[i].enabled)
      gc_slots.push_back(i + 1);
    if (pad_map[i] == pid && gba_config[i].enabled)
      gba_slots.push_back(i + 1);
    if (wiimote_map[i] == pid)
      wiimote_slots.push_back(i + 1);
  }
  std::vector<std::string> groups;
  std::array<std::pair<std::string, std::vector<size_t>*>, 3> slot_groups = {
      {{"GC", &gc_slots}, {"GBA", &gba_slots}, {"Wii", &wiimote_slots}}};

  for (const auto& [group_name, slots] : slot_groups)
  {
    if (!slots->empty())
      groups.emplace_back(fmt::format("{}{}", group_name, fmt::join(*slots, ",")));
  }
  std::string res = fmt::format("{}", fmt::join(groups, "|"));
  return res.empty() ? "None" : res;
}

bool IsNetPlayRunning()
{
  return netplay_client != nullptr;
}

void SetSIPollBatching(bool state)
{
  s_si_poll_batching = state;
}

void SendPowerButtonEvent()
{
  ASSERT(IsNetPlayRunning());
  netplay_client->SendPowerButtonEvent();
}

std::string GetGBASavePath(int pad_num)
{
  std::lock_guard lk(crit_netplay_client);

  if (!netplay_client || netplay_client->GetNetSettings().is_hosting)
  {
#ifdef HAS_LIBMGBA
    std::string rom_path = Config::Get(Config::MAIN_GBA_ROM_PATHS[pad_num]);
    return HW::GBA::Core::GetSavePath(rom_path, pad_num);
#else
    return {};
#endif
  }

  if (!netplay_client->GetNetSettings().savedata_load)
    return {};

  return fmt::format("{}{}{}.sav", File::GetUserPath(D_GBAUSER_IDX), GBA_SAVE_NETPLAY, pad_num + 1);
}

PadDetails GetPadDetails(int pad_num)
{
  std::lock_guard lk(crit_netplay_client);

  PadDetails res{};
  res.local_pad = 4;
  if (!netplay_client)
    return res;

  auto pad_map = netplay_client->GetPadMapping();
  if (pad_map[pad_num] <= 0)
    return res;

  for (auto player : netplay_client->GetPlayers())
  {
    if (player->pid == pad_map[pad_num])
      res.player_name = player->name;
  }

  int local_pad = 0;
  int non_local_pad = 0;
  for (int i = 0; i < pad_num; ++i)
  {
    if (netplay_client->IsLocalPlayer(pad_map[i]))
      ++local_pad;
    else
      ++non_local_pad;
  }
  res.is_local = netplay_client->IsLocalPlayer(pad_map[pad_num]);
  res.local_pad = res.is_local ? local_pad : netplay_client->NumLocalPads() + non_local_pad;
  res.hide_gba = !res.is_local && netplay_client->GetNetSettings().hide_remote_gbas &&
                 netplay_client->LocalPlayerHasControllerMapped();
  return res;
}

int NumLocalWiimotes()
{
  std::lock_guard lk(crit_netplay_client);
  if (netplay_client)
    return netplay_client->NumLocalWiimotes();
  return 0;
}

void NetPlay_Enable(NetPlayClient* const np)
{
  std::lock_guard lk(crit_netplay_client);
  netplay_client = np;
}

void NetPlay_Disable()
{
  std::lock_guard lk(crit_netplay_client);
  netplay_client = nullptr;
}

void OnFrameStart()
{
  if (IsNetPlayRunning() && IsInRollbackMode())
  {
    std::unique_lock lock(crit_netplay_client);
    netplay_client->OnFrameStart(lock);
  }
}

bool IsTimeSynced()
{
  if (IsNetPlayRunning() && IsInRollbackMode())
  {
    return netplay_client->IsTimeSynced();
  }
  return true;
}

bool IsRollingBack()
{
  if (!netplay_client)
    return false;

  return netplay_client->IsRollingBack();
}

bool IsInRollbackMode()
{
  if (!netplay_client)
    return false;

  return netplay_client->IsInRollbackMode();
}

u64 GetInitialRTCValue()
{
  if (!netplay_client)
    return 0;

  return netplay_client->GetInitialRTCValue();
}

bool IsGekkoCpuStalled()
{
  return s_gekko_cpu_stalled.load(std::memory_order_acquire);
}

void SetGekkoCpuStalled(bool stalled)
{
  s_gekko_cpu_stalled.store(stalled, std::memory_order_release);
  s_gekko_cpu_stalled.notify_all();
}

void NoteGekkoStallFallback()
{
  if (netplay_client)
    netplay_client->NoteStallFallback();
}

GekkoSession* GetGekkoSession()
{
  if (!netplay_client)
    return nullptr;

  return netplay_client->GetGekkoSession();
}

double GetTimeSyncSpeedFactor()
{
  if (!netplay_client)
    return 1.0;

  return netplay_client->GetTimeSyncSpeedFactor();
}

int GetFramesToAdvance()
{
  if (!netplay_client)
    return 1;

  return netplay_client->GetFramesToAdvance();
}

bool HasPendingSave()
{
  if (!netplay_client)
    return false;

  return netplay_client->GetGekkoPendingFinalSave();
}

void ClearPendingSave()
{
  if (netplay_client)
    netplay_client->SetGekkoPendingFinalSave(false);
}

bool ShouldSaveAfterIteration(int iteration_index)
{
  if (!netplay_client)
    return false;

  return netplay_client->GetShouldSaveAfterIteration(iteration_index);
}

void WriteGekkoChecksumForIteration(int iteration_index, u32 checksum)
{
  if (netplay_client)
    netplay_client->WriteGekkoChecksumForIteration(iteration_index, checksum);
}

int GetCurrentIteration()
{
  if (!netplay_client)
    return 0;

  return netplay_client->GetGekkoCurrentIteration();
}

void SetCurrentIteration(int iteration)
{
  if (netplay_client)
    netplay_client->SetGekkoCurrentIteration(iteration);
}

void InjectPadsForIteration(int iteration_index)
{
  if (netplay_client)
    netplay_client->InjectPadsForIteration(iteration_index);
}

void SetGekkoResimulationPass(bool is_resimulation_pass)
{
  if (netplay_client)
    netplay_client->SetGekkoResimulationPass(is_resimulation_pass);
}

void CaptureGekkoPadInput(Core::System& system)
{
  if (netplay_client)
    netplay_client->CaptureGekkoPadInput(system);
}

void RecordPadHistory(int iteration_index)
{
  if (netplay_client)
    netplay_client->RecordPadHistory(Core::System::GetInstance(), iteration_index);
}

void QueueFrameSave(int iteration_index, u32 logic_steps)
{
  if (netplay_client)
    netplay_client->QueueFrameSave(iteration_index, logic_steps);
}

void PerformQueuedFrameSave(Core::System& system)
{
  if (netplay_client)
    netplay_client->PerformQueuedFrameSave(system);
}

void OnDisplayedFrameStart()
{
  if (netplay_client)
    netplay_client->OnDisplayedFrameStart();
}
}  // namespace NetPlay

// stuff hacked into dolphin

// called from ---CPU--- thread
// Actual Core function which is called on every frame
bool SerialInterface::CSIDevice_GCController::NetPlay_GetInput(int pad_num, GCPadStatus* status)
{
  std::lock_guard lk(NetPlay::crit_netplay_client);

  if (NetPlay::netplay_client)
    return NetPlay::netplay_client->GetNetPads(pad_num, NetPlay::s_si_poll_batching, status);

  return false;
}

bool NetPlay::NetPlay_GetWiimoteData(const std::span<NetPlayClient::WiimoteDataBatchEntry>& entries)
{
  std::lock_guard lk(crit_netplay_client);

  if (netplay_client)
    return netplay_client->WiimoteUpdate(entries);

  return false;
}

unsigned int NetPlay::NetPlay_GetLocalWiimoteForSlot(unsigned int slot)
{
  if (slot >= std::tuple_size_v<PadMappingArray>)
    return slot;

  std::lock_guard lk(crit_netplay_client);

  if (!netplay_client)
    return slot;

  const auto& mapping = netplay_client->GetWiimoteMapping();
  const auto& local_player_id = netplay_client->GetLocalPlayerId();

  std::array<unsigned int, std::tuple_size_v<std::decay_t<decltype(mapping)>>> slot_map;
  size_t player_count = 0;
  for (size_t i = 0; i < mapping.size(); ++i)
  {
    if (mapping[i] == local_player_id)
    {
      slot_map[i] = static_cast<unsigned int>(player_count);
      ++player_count;
    }
  }
  for (size_t i = 0; i < mapping.size(); ++i)
  {
    if (mapping[i] != local_player_id)
    {
      slot_map[i] = static_cast<unsigned int>(player_count);
      ++player_count;
    }
  }

  INFO_LOG_FMT(NETPLAY, "Wiimote slot map: [{}]", fmt::join(slot_map, ", "));

  return slot_map[slot];
}

// called from ---CPU--- thread
// so all players' games get the same time
//
// also called from ---GUI--- thread when starting input recording
u64 ExpansionInterface::CEXIIPL::NetPlay_GetEmulatedTime()
{
  std::lock_guard lk(NetPlay::crit_netplay_client);

  if (NetPlay::netplay_client)
    return NetPlay::netplay_client->GetInitialRTCValue();

  return 0;
}

// called from ---CPU--- thread
// return the local pad num that should rumble given a ingame pad num
int SerialInterface::CSIDevice_GCController::NetPlay_InGamePadToLocalPad(int pad_num)
{
  std::lock_guard lk(NetPlay::crit_netplay_client);

  if (NetPlay::netplay_client)
    return NetPlay::netplay_client->InGamePadToLocalPad(pad_num);

  return pad_num;
}
