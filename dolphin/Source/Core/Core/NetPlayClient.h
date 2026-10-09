// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <SFML/Network/Packet.hpp>
#include <atomic>
#include <array>
#include <chrono>
#include <optional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/SPSCQueue.h"
#include "Common/TraversalClient.h"
#include "Core/Core.h"
#include "Core/NetPlayProto.h"
#include "Core/SyncIdentifier.h"
#include "InputCommon/GCPadStatus.h"
#include "Brawlback/include/brawlback-common/BrawlbackConstants.h"
#include "Brawlback/include/brawlback-common/GfPadStatus.h"

class BootSessionData;

namespace Memory
{
class MemoryManager;
}

// Forward declare GekkoNet types
struct GekkoSession;
struct GekkoNetAdapter;
struct GekkoNetAddress;
struct GekkoNetResult;

namespace IOS::HLE::FS
{
class FileSystem;
}

namespace UICommon
{
class GameFile;
}

namespace WiimoteEmu
{
struct SerializedWiimoteState;
}

namespace NetPlay
{
// Brawl pad memory layout constants (EXI-style injection)
// gfPadSystem instance at 0x805bacc0, raw pads at +0x40, stride sizeof(gfPadStatus)
static constexpr u32 BRAWL_PADSYSTEM_INSTANCE = 0x805bacc0;
static constexpr u32 BRAWL_PAD_RAW_BASE = BRAWL_PADSYSTEM_INSTANCE + 0x40;
static constexpr u32 BRAWL_PAD_STRIDE = sizeof(gfPadStatus);

// Forward declare GekkoNet adapter functions
void GekkoNetAdapter_SendData(GekkoNetAddress* address, const char* data, int length);
GekkoNetResult** GekkoNetAdapter_ReceiveData(int* length);
void GekkoNetAdapter_FreeData(void* data);

class NetPlayUI
{
public:
  virtual ~NetPlayUI() {}
  virtual void BootGame(const std::string& filename,
                        std::unique_ptr<BootSessionData> boot_session_data) = 0;
  virtual void StopGame() = 0;
  virtual bool IsHosting() const = 0;

  virtual void Update() = 0;
  virtual void AppendChat(const std::string& msg) = 0;

  virtual void OnMsgChangeGame(const SyncIdentifier& sync_identifier,
                               const std::string& netplay_name) = 0;
  virtual void OnMsgChangeGBARom(int pad, const NetPlay::GBAConfig& config) = 0;
  virtual void OnMsgStartGame() = 0;
  virtual void OnMsgStopGame() = 0;
  virtual void OnMsgPowerButton() = 0;
  virtual void OnPlayerConnect(const std::string& player) = 0;
  virtual void OnPlayerDisconnect(const std::string& player) = 0;
  virtual void OnMinimumPadBufferChanged(u32 buffer) = 0;
  virtual void OnPlayerPadBufferChanged(u32 buffer) = 0;
  virtual void OnRollbackModeChanged(bool enabled) = 0;
  // The host's rollback input delay (frames from pad read to use).
  virtual void OnRollbackDelayChanged(u32 delay) {}
  virtual bool IsSpectator() = 0;
  virtual void OnHostInputAuthorityChanged(bool enabled) = 0;
  virtual void OnDesync(u32 frame, const std::string& player) = 0;
  virtual void OnConnectionLost() = 0;
  virtual void OnConnectionError(const std::string& message) = 0;
  virtual void OnTraversalError(Common::TraversalClient::FailureReason error) = 0;
  virtual void OnTraversalStateChanged(Common::TraversalClient::State state) = 0;
  virtual void OnGameStartAborted() = 0;
  virtual void OnGolferChanged(bool is_golfer, const std::string& golfer_name) = 0;
  virtual void OnTtlDetermined(u8 ttl) = 0;

  virtual bool IsRecording() = 0;
  virtual std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const SyncIdentifier& sync_identifier,
               SyncIdentifierComparison* found = nullptr) = 0;
  virtual std::string FindGBARomPath(const std::array<u8, 20>& hash, std::string_view title,
                                     int device_number) = 0;
  virtual void ShowGameDigestDialog(const std::string& title) = 0;
  virtual void SetGameDigestProgress(int pid, int progress) = 0;
  virtual void SetGameDigestResult(int pid, const std::string& result) = 0;
  virtual void AbortGameDigest() = 0;

  virtual void OnIndexAdded(bool success, std::string error) = 0;
  virtual void OnIndexRefreshFailed(std::string error) = 0;

  virtual void ShowChunkedProgressDialog(const std::string& title, u64 data_size,
                                         std::span<const int> players) = 0;
  virtual void HideChunkedProgressDialog() = 0;
  virtual void SetChunkedProgress(int pid, u64 progress) = 0;

  virtual void SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder) = 0;
};

class Player
{
public:
  PlayerId pid{};
  std::string name;
  std::string revision;
  u32 ping = 0;
  u32 buffer = 0;
  SyncIdentifierComparison game_status = SyncIdentifierComparison::Unknown;

  bool IsHost() const { return pid == 1; }
};

class NetPlayClient : public Common::TraversalClientClient
{
  // Friend declarations for GekkoNet adapter callbacks
  friend void GekkoNetAdapter_SendData(GekkoNetAddress* address, const char* data, int length);
  friend GekkoNetResult** GekkoNetAdapter_ReceiveData(int* length);
  friend void GekkoNetAdapter_FreeData(void* data);

public:
  void ThreadFunc();
  void SendAsync(sf::Packet&& packet, u8 channel_id = DEFAULT_CHANNEL);

  // local_port: UDP port to bind for a direct connection (0 = any). Online play binds the port
  // its matchmaking hole-punched (Online/OnlineSession.h).
  NetPlayClient(const std::string& address, const u16 port, NetPlayUI* dialog, std::string name,
                const NetTraversalConfig& traversal_config, u16 local_port = 0);
  ~NetPlayClient() override;

  std::vector<const Player*> GetPlayers();
  const NetSettings& GetNetSettings() const;

  // Called from the GUI thread.
  bool IsConnected() const { return m_is_connected; }
  bool StartGame(const std::string& path);
  void InvokeStop();
  bool StopGame();
  void Stop();
  bool ChangeGame(const std::string& game);
  void SendChatMessage(const std::string& msg);
  void RequestStopGame();
  void SendPowerButtonEvent();
  void RequestGolfControl(PlayerId pid);
  void RequestGolfControl();
  std::string GetCurrentGolfer();
  static SyncIdentifier GetBrawlFileIdentifier();
  void AdjustPlayerPadBufferSize(u32 buffer);
  void OnPadBufferPlayer(sf::Packet& packet);
  void OnPadBufferMinimum(sf::Packet& packet);

  // Send and receive pads values
  struct WiimoteDataBatchEntry
  {
    int wiimote;
    WiimoteEmu::SerializedWiimoteState* state;
  };
  bool WiimoteUpdate(const std::span<WiimoteDataBatchEntry>& entries);
  bool GetNetPads(int pad_nb, bool from_vi, GCPadStatus* pad_status);

  u64 GetInitialRTCValue() const;

  void OnTraversalStateChanged() override;
  void OnConnectReady(ENetAddress addr) override;
  void OnConnectFailed(Common::TraversalConnectFailedReason reason) override;
  void OnTtlDetermined(u8 ttl) override {}

  bool IsFirstInGamePad(int ingame_pad) const;
  int NumLocalPads() const;
  int NumLocalWiimotes() const;

  int InGamePadToLocalPad(int ingame_pad) const;
  int LocalPadToInGamePad(int local_pad) const;
  int InGameWiimoteToLocalWiimote(int ingame_wiimote) const;
  int LocalWiimoteToInGameWiimote(int local_wiimote) const;

  bool PlayerHasControllerMapped(PlayerId pid) const;
  bool LocalPlayerHasControllerMapped() const;
  bool IsLocalPlayer(PlayerId pid) const;
  const PlayerId& GetLocalPlayerId() const;

  static void SendTimeBase();
  bool DoAllPlayersHaveGame();

  const PadMappingArray& GetPadMapping() const;
  const GBAConfigArray& GetGBAConfig() const;
  const PadMappingArray& GetWiimoteMapping() const;

  void AdjustMinimumPadBufferSize(unsigned int size);

  void AdjustPadBufferSize(unsigned int size);

  void SetWiiSyncData(std::unique_ptr<IOS::HLE::FS::FileSystem> fs, std::vector<u64> titles,
                      std::string redirect_folder);

  static SyncIdentifier GetSDCardIdentifier();

  void OnFrameEnd(std::unique_lock<std::mutex>& lock);
  void OnFrameStart(std::unique_lock<std::mutex>& lock);
  void InjectPadsForIteration(int iteration_index);
  void SetGekkoResimulationPass(bool is_resimulation_pass);
  void CaptureGekkoPadInput(Core::System& system);
  void CheckForLocalAdvantage();
  double GetTimeSyncSpeedFactor() const { return m_time_sync_speed_factor; }
  bool IsRollingBack();
  bool IsInRollbackMode();
  int GetFramesToAdvance();
  bool IsTimeSynced();

  // GekkoNet outer game loop iteration control
  int GetGekkoCurrentIteration() const { return m_gekko_current_iteration; }
  void SetGekkoCurrentIteration(int iteration) { m_gekko_current_iteration = iteration; }
  bool GetGekkoPendingFinalSave() const { return m_gekko_pending_final_save; }
  void SetGekkoPendingFinalSave(bool value) { m_gekko_pending_final_save = value; }
  bool GetShouldSaveAfterIteration(int iteration_index) const
  {
    if (iteration_index < 0 || iteration_index >= m_gekko_pending_ops.adv_count)
      return false;
    return m_gekko_pending_ops.save_after[iteration_index];
  }
  void WriteGekkoChecksumForIteration(int iteration_index, u32 checksum)
  {
    if (iteration_index < 0 || iteration_index >= m_gekko_pending_ops.adv_count)
      return;
    if (u32* ptr = m_gekko_pending_ops.save_checksum_ptrs[iteration_index])
      *ptr = checksum;
  }

  // Only for use in NetPlayClient.cpp >:(
  u64 current_frame = 0;
  // Only meaningful once a Gekko session exists; see IsTimeSynced().
  bool time_synced = false;

  bool done_fast_forwarding;

  // EXI-style SI override interface for GekkoNet pad injection
  // Called from SI device code to override pad reads during resimulation
  static bool GetOverrideInput(int pad_num, gfPadStatus* status);
  
  inline GekkoSession* GetGekkoSession() { return m_gekko_session; }

  // Rollback counters, readable from any thread (used by the automation harness).
  struct RollbackStats
  {
    bool gekko_session = false;
    bool session_started = false;
    u64 current_frame = 0;
    u64 rollbacks = 0;
    u64 max_rollback_frames = 0;
    u64 frames_resimulated = 0;
    u64 desyncs_detected = 0;
    s64 last_desync_frame = -1;
    float frames_ahead = 0.0f;
    double time_sync_speed = 1.0;
    u64 stall_polls = 0;  // GekkoNet updates that gave no frame to advance
    u64 peer_disconnects = 0;   // GekkoNet dropped a remote player
    u64 skipped_loads = 0;      // rollbacks deeper than the snapshot ring (not performed)
    u64 dropped_advances = 0;   // advances beyond MAX_ADVANCE in one update (not run)
    u64 stall_fallbacks = 0;    // stall waits given up for a guest-side spin (time advanced)
    bool synctest = false;
    u64 synctest_mismatches = 0;  // resimulated frames that differed from their first run
    u64 save_count = 0, save_us_total = 0, save_us_max = 0;
    u64 load_count = 0, load_us_total = 0, load_us_max = 0;
    u64 save_sync_count = 0, save_sync_us_total = 0, save_sync_us_max = 0;
    // Per rollback depth (frames resimulated, index = depth): wall-clock cost of the load and of
    // resimulating the frames (from the end of the load to the start of the displayed frame,
    // including the snapshots of the resimulated frames).
    struct DepthStats
    {
      u64 count = 0;
      u64 load_us_total = 0, load_us_max = 0;
      u64 resim_us_total = 0, resim_us_max = 0;
    };
    static constexpr int MAX_DEPTH_STATS = 16;
    std::array<DepthStats, MAX_DEPTH_STATS> by_depth{};
  };
  RollbackStats GetRollbackStats() const;
  // The rollback input delay the host announced (the running game uses GetNetSettings()'s).
  u32 GetAnnouncedRollbackDelay() const { return m_announced_rollback_delay; }
  // CPU thread, at the start of every displayed (not resimulated) frame.
  void OnDisplayedFrameStart();

  // Per GekkoNet frame, the pads the game actually used for that frame (the last simulation of
  // the frame wins, so after a rollback this is the corrected input). For the automation harness:
  // both peers must agree on every frame.
  struct PadHistoryEntry
  {
    s64 frame = -1;
    std::array<u32, 4> buttons{};
    u32 crc = 0;  // CRC32 of all four raw gfPadStatus slots
    // Desync checksums at the end of the frame: combined (what GekkoNet compares), legacy,
    // frame_counter, fighters (see Rollback::DesyncChecksums), and the number of game-logic steps
    // (gameProc calls) the iteration ran.
    std::array<u32, 5> state{};
    std::array<u8, 4 * sizeof(gfPadStatus)> raw{};  // the four gfPadStatus slots (big endian)
  };
  void RecordPadHistory(Core::System& system, int iteration_index);
  void RecordStateHistory(s64 frame, const std::array<u32, 5>& state);

  // Frame saves are requested at the end of an iteration and taken at the top of the next pass
  // through the game loop, the same instruction boundary loads resume at.
  void QueueFrameSave(int iteration_index, u32 logic_steps);
  void PerformQueuedFrameSave(Core::System& system);

  // Desync hunting (enabled by the environment variable PPR_ROLLBACK_CHUNK_HASHES=1): at the end
  // of every simulated frame, an XXH3 hash of each 64 KiB of MEM1 then MEM2, kept for the last
  // 256 GekkoNet frames (last simulation wins). Costs a few ms per frame.
  static constexpr u32 CHUNK_HASH_SIZE = 64 * 1024;
  void RecordChunkHashes(Core::System& system, s64 frame);
  std::optional<std::vector<u64>> GetChunkHashes(s64 frame) const;
  u32 GetChunkHashSize() const { return m_chunk_hash_size; }
  u32 ChunkAddress(const Memory::MemoryManager& memory, size_t index) const;
  // PPR_SYNCTEST=N (1-5) with a single player: run a GekkoNet stress session (see InitGekkoSession).
  static int SyncTestDistance();
  std::vector<PadHistoryEntry> GetPadHistory(s64 since_frame) const;

protected:
  struct AsyncQueueEntry
  {
    sf::Packet packet;
    u8 channel_id = 0;
  };

  void ClearBuffers();

  struct
  {
    std::recursive_mutex game;
    // lock order
    std::recursive_mutex players;
    std::recursive_mutex async_queue_write;
  } m_crit;

  Common::SPSCQueue<AsyncQueueEntry> m_async_queue;

  std::array<Common::SPSCQueue<GCPadStatus>, 4> m_pad_buffer;
  std::array<Common::SPSCQueue<WiimoteEmu::SerializedWiimoteState>, 4> m_wiimote_buffer;

  std::array<GCPadStatus, 4> m_last_pad_status{};
  std::array<bool, 4> m_first_pad_status_received{};

  std::chrono::time_point<std::chrono::steady_clock> m_buffer_under_target_last;

  // the number of ticks in-between frames
  constexpr static int buffer_accuracy = 4;

  inline u32 BufferSizeForPort(int pad) const
  {
    if (GetPadMapping()[pad] <= 0)
      return 0;

    return std::max(m_minimum_buffer_size, m_players.at(GetPadMapping().at(pad)).buffer);
  }

  // used for chat, not the best place for it
  inline std::string FindPlayerPadName(const Player* player) const
  {
    for (int i = 0; i < 4; i++)
    {
      if (GetPadMapping()[i] == player->pid)
        return " (port " + std::to_string(i + 1) + ")";
    }

    return "";
  }

  NetPlayUI* m_dialog = nullptr;

  ENetHost* m_client = nullptr;
  ENetPeer* m_server = nullptr;
  std::thread m_thread;

  SyncIdentifier m_selected_game;
  Common::Flag m_is_running{false};
  Common::Flag m_do_loop{true};

  // In non-host input authority mode, this is how many packets each client should
  // try to keep in-flight to the other clients. In host input authority mode, this is how
  // many incoming input packets need to be queued up before the client starts
  // speeding up the game to drain the buffer.
  unsigned int m_target_buffer_size = 20;
  unsigned int m_minimum_buffer_size = 2;
  bool m_host_input_authority = false;
  PlayerId m_current_golfer = 1;

  // This bool will stall the client at the start of GetNetPads, used for switching input control
  // without deadlocking. Use the correspondingly named Event to wake it up.
  bool m_wait_on_input;
  bool m_wait_on_input_received;

  Player* m_local_player = nullptr;

  u32 m_current_game = 0;

  bool m_is_recording = false;

private:
  enum class ConnectionState
  {
    WaitingForTraversalClientConnection,
    WaitingForTraversalClientConnectReady,
    Connecting,
    WaitingForHelloResponse,
    Connected,
    Failure
  };

  void SendStartGamePacket();
  void SendStopGamePacket();

  void SyncSaveDataResponse(bool success);
  void SyncCodeResponse(bool success);

  bool PollLocalPad(int local_pad, sf::Packet& packet);
  void SendPadHostPoll(PadIndex pad_num);

  bool AddLocalWiimoteToBuffer(int local_wiimote, const WiimoteEmu::SerializedWiimoteState& state,
                               sf::Packet& packet);

  void AddPadStateToPacket(int in_game_pad, const GCPadStatus& np, sf::Packet& packet);
  void AddWiimoteStateToPacket(int in_game_pad, const WiimoteEmu::SerializedWiimoteState& np,
                               sf::Packet& packet);
  void Send(const sf::Packet& packet, u8 channel_id = DEFAULT_CHANNEL);
  void Disconnect();
  bool Connect();
  void SendGameStatus();
  void ComputeGameDigest(const SyncIdentifier& sync_identifier);
  void DisplayPlayersPing();
  void DisplayGekkoPing(u32 frame);
  u32 GetPlayersMaxPing() const;

  // GekkoNet session management
  void InitGekkoSession(const std::string& remote_addr, unsigned short local_port, bool is_host);
  void DestroyGekkoSession();
  void HandleGekkoFrame();
  bool ProcessGekkoEvents();
  void InjectGekkoInput(const std::array<GCPadStatus, 4>& pads);

  void OnData(sf::Packet& packet);
  void OnPlayerJoin(sf::Packet& packet);
  void OnPlayerLeave(sf::Packet& packet);
  void OnChatMessage(sf::Packet& packet);
  void OnChunkedDataStart(sf::Packet& packet);
  void OnChunkedDataEnd(sf::Packet& packet);
  void OnChunkedDataPayload(sf::Packet& packet);
  void OnChunkedDataAbort(sf::Packet& packet);
  void OnPadMapping(sf::Packet& packet);
  void OnWiimoteMapping(sf::Packet& packet);
  void OnGBAConfig(sf::Packet& packet);
  void OnPadData(sf::Packet& packet);
  void OnPadHostData(sf::Packet& packet);
  void OnWiimoteData(sf::Packet& packet);
  void OnPadBuffer(sf::Packet& packet);
  void OnHostInputAuthority(sf::Packet& packet);
  void OnRollbackDelay(sf::Packet& packet);
  void OnGolfSwitch(sf::Packet& packet);
  void OnGolfPrepare(sf::Packet& packet);
  void OnChangeGame(sf::Packet& packet);
  void OnGameStatus(sf::Packet& packet);
  void OnStartGame(sf::Packet& packet);
  void OnStopGame(sf::Packet& packet);
  void OnPowerButton();
  void OnPing(sf::Packet& packet);
  void OnPlayerPingData(sf::Packet& packet);
  void OnDesyncDetected(sf::Packet& packet);
  void OnSyncSaveData(sf::Packet& packet);
  void OnSyncSaveDataNotify(sf::Packet& packet);
  void OnSyncSaveDataRaw(sf::Packet& packet);
  void OnSyncSaveDataGCI(sf::Packet& packet);
  void OnSyncSaveDataWii(sf::Packet& packet);
  void OnSyncSaveDataGBA(sf::Packet& packet);
  void OnSyncCodes(sf::Packet& packet);
  void OnSyncCodesNotify();
  void OnSyncCodesNotifyGecko(sf::Packet& packet);
  void OnSyncCodesDataGecko(sf::Packet& packet);
  void OnSyncCodesNotifyAR(sf::Packet& packet);
  void OnSyncCodesDataAR(sf::Packet& packet);
  void OnComputeGameDigest(sf::Packet& packet);
  void OnGameDigestProgress(sf::Packet& packet);
  void OnGameDigestResult(sf::Packet& packet);
  void OnGameDigestError(sf::Packet& packet);
  void OnGameDigestAbort();
  size_t GetLatestRemoteFrame();

  bool m_is_connected = false;
  ConnectionState m_connection_state = ConnectionState::Failure;

  PlayerId m_pid = 0;
  NetSettings m_net_settings{};
  std::map<PlayerId, Player> m_players;
  std::string m_host_spec;
  std::string m_player_name;
  bool m_connecting = false;
  Common::TraversalClient* m_traversal_client = nullptr;
  std::thread m_game_digest_thread;
  bool m_should_compute_game_digest = false;
  Common::Event m_gc_pad_event;
  Common::Event m_wii_pad_event;
  Common::Event m_first_pad_status_received_event;
  Common::Event m_wait_on_input_event;
  u8 m_sync_save_data_count = 0;
  u8 m_sync_save_data_success_count = 0;
  u16 m_sync_gecko_codes_count = 0;
  u16 m_sync_gecko_codes_success_count = 0;
  bool m_sync_gecko_codes_complete = false;
  u16 m_sync_ar_codes_count = 0;
  u16 m_sync_ar_codes_success_count = 0;
  bool m_sync_ar_codes_complete = false;
  std::unordered_map<u32, sf::Packet> m_chunked_data_receive_queue;

  u64 m_initial_rtc = 0;
  u32 m_timebase_frame = 0;

  std::unique_ptr<IOS::HLE::FS::FileSystem> m_wii_sync_fs;
  std::vector<u64> m_wii_sync_titles;
  std::string m_wii_sync_redirect_folder;

  // GekkoNet adapter state
  struct GekkoNetPacket
  {
    std::vector<char> data;
    GekkoNetPacket() = default;
    explicit GekkoNetPacket(const char* d, size_t len) : data(d, d + len) {}
  };
  std::vector<GekkoNetPacket> m_gekko_received_packets;
  std::vector<void*> m_gekko_packet_ptrs;  // For adapter receive_data return
  std::mutex m_gekko_packet_mutex;
  std::mutex m_gekko_poll_mutex;  // Protects gekko_network_poll calls
  // Outgoing GekkoNet packets, queued by the CPU thread and sent by the netplay thread.
  std::vector<GekkoNetPacket> m_gekko_send_queue;
  std::mutex m_gekko_send_mutex;
  void FlushGekkoSendQueue();
  struct GekkoNetAdapter* m_gekko_adapter = nullptr;

  // GekkoNet session state
  GekkoSession* m_gekko_session = nullptr;
  int m_gekko_local_handle = -1;
  int m_gekko_remote_handle = -1;
  bool m_gekko_session_started = false;
  std::string m_gekko_remote_addr;
  bool m_gekko_seen_frame_zero = false;
  int m_gekko_connect_wait_ticks = 0;
  // Frames between ping line refreshes (Slippi's SLIPPI_PING_DISPLAY_INTERVAL).
  static constexpr u32 PING_DISPLAY_INTERVAL = 60;
  u32 m_gekko_next_ping_display_frame = 0;
  bool m_use_gekko_netplay = false;
  gfPadStatus m_gekko_last_local_input{};
  std::array<gfPadStatus, MAX_NUM_PLAYERS> m_gekko_last_synced_pads{};
  std::array<bool, MAX_NUM_PLAYERS> m_gekko_has_last_synced_pad{};
  bool m_is_rolling_back = false;
  std::atomic<u32> m_announced_rollback_delay{ROLLBACK_DELAY_DEFAULT};

  // Rollback counters (see GetRollbackStats()).
  std::atomic<bool> m_stat_gekko_session{false};
  std::atomic<bool> m_stat_session_started{false};
  std::atomic<u64> m_stat_current_frame{0};
  std::atomic<u64> m_stat_rollbacks{0};
  std::atomic<u64> m_stat_max_rollback_frames{0};
  std::atomic<u64> m_stat_frames_resimulated{0};
  std::atomic<u64> m_stat_desyncs_detected{0};
  std::atomic<s64> m_stat_last_desync_frame{-1};
  std::atomic<float> m_stat_frames_ahead{0.0f};
  struct AtomicDepthStats
  {
    std::atomic<u64> count{0};
    std::atomic<u64> load_us_total{0}, load_us_max{0};
    std::atomic<u64> resim_us_total{0}, resim_us_max{0};
  };
  std::array<AtomicDepthStats, RollbackStats::MAX_DEPTH_STATS> m_stat_depth{};
  // CPU thread: the rollback whose resimulation is running (depth 0 = none).
  int m_resim_depth = 0;
  std::chrono::steady_clock::time_point m_resim_start{};

  // Synchronized start (see PollStartSync()). Small control messages share the GekkoNet channel
  // (the host's server relays it), marked by a first byte GekkoNet never uses.
  static constexpr u8 GEKKO_CONTROL_MAGIC = 0xB7;
  enum class StartSyncMsg : u8
  {
    Ready = 'R',  // joiner -> host: my GekkoNet session has started
    Go = 'G',     // host -> joiner: start advancing now
  };
  void SendStartSyncMsg(StartSyncMsg msg);
  void OnGekkoControlPacket(const u8* data, size_t size);  // netplay thread
  bool PollStartSync();                                      // CPU thread
  bool m_start_sync_waiting = false;
  std::atomic<bool> m_start_sync_remote_ready{false};
  std::atomic<bool> m_start_sync_go_sent{false};
  std::atomic<bool> m_start_sync_go_received{false};
  std::optional<std::chrono::steady_clock::time_point> m_start_sync_start_at;
  std::chrono::steady_clock::time_point m_start_sync_last_ready{};

  // Time sync (see UpdateTimeSync()): emulation speed factor, read on the CPU thread.
  void UpdateTimeSync();
  std::atomic<double> m_time_sync_speed_factor{1.0};
  std::atomic<u64> m_stat_stall_polls{0};
  // Paths that leave this side out of step with GekkoNet (should all stay 0).
  std::atomic<u64> m_stat_peer_disconnects{0};
  std::atomic<u64> m_stat_skipped_loads{0};
  std::atomic<u64> m_stat_dropped_advances{0};
  std::atomic<u64> m_stat_stall_fallbacks{0};
  bool m_gekko_disconnect_stop_requested = false;  // CPU thread

public:
  void NoteStallFallback() { m_stat_stall_fallbacks++; }

private:
  u32 m_time_sync_last_update_frame = UINT32_MAX;

  struct ChunkHashEntry
  {
    s64 frame = -1;
    std::vector<u64> hashes;
  };
  struct QueuedFrameSave
  {
    bool pending = false;
    s64 frame = -1;
    u32* checksum = nullptr;
    u32 logic_steps = 0;
  };
  QueuedFrameSave m_queued_save;
  bool m_chunk_hashes_enabled = false;
  u32 m_chunk_hash_size = CHUNK_HASH_SIZE;
  bool m_synctest = false;
  int m_gekko_num_players = 2;
  std::atomic<u64> m_stat_synctest_mismatches{0};
  // Sync test: full MEM1+MEM2 copies of recent first runs, to report the differing words.
  struct SyncTestRamCopy
  {
    s64 frame = -1;
    std::vector<u8> ram;
  };
  std::array<SyncTestRamCopy, 8> m_synctest_ram;
  std::array<ChunkHashEntry, 256> m_chunk_hashes;
  mutable std::mutex m_chunk_hash_mutex;

  static constexpr size_t PAD_HISTORY_SIZE = 2048;
  std::array<PadHistoryEntry, PAD_HISTORY_SIZE> m_pad_history{};
  mutable std::mutex m_pad_history_mutex;

  // GekkoNet pending operations for current frame
  struct GekkonetPendingOps
  {
    static constexpr int MAX_ADVANCE = MAX_ROLLBACK_FRAMES + 1;
    int adv_count = 0;
    u32 adv_frames[MAX_ADVANCE]{};
    bool adv_rollback[MAX_ADVANCE]{};
    std::array<std::array<gfPadStatus, MAX_ADVANCE>, 4> adv_pads{};
    bool save_after[MAX_ADVANCE]{};
    u32* save_checksum_ptrs[MAX_ADVANCE]{};
    bool load_before[MAX_ADVANCE]{};
    int load_before_frame[MAX_ADVANCE]{};

    void Clear()
    {
      adv_count = 0;
      std::memset(adv_frames, 0, sizeof(adv_frames));
      std::memset(adv_rollback, 0, sizeof(adv_rollback));
      std::memset(save_after, 0, sizeof(save_after));
      std::memset(save_checksum_ptrs, 0, sizeof(save_checksum_ptrs));
      std::memset(load_before, 0, sizeof(load_before));
      std::memset(load_before_frame, 0, sizeof(load_before_frame));
      for (auto& player_pads : adv_pads)
        player_pads.fill(gfPadStatus{});
    }
  };
  GekkonetPendingOps m_gekko_pending_ops;
  bool m_gekko_pending_final_save = false;
  int m_gekko_current_iteration = 0;
  bool m_gekko_frame_save_initialized = false;
  static inline std::atomic<bool> s_override_active{false};
  static inline std::array<gfPadStatus, 4> s_override_pads{};

  void InjectPads(const std::array<gfPadStatus, 4>& pads, Core::System& system);
};

void NetPlay_Enable(NetPlayClient* const np);
void NetPlay_Disable();
bool NetPlay_GetWiimoteData(const std::span<NetPlayClient::WiimoteDataBatchEntry>& entries);
unsigned int NetPlay_GetLocalWiimoteForSlot(unsigned int slot);
void OnFrameStart();
void InjectPadsForIteration(int iteration_index);
void SetGekkoResimulationPass(bool is_resimulation_pass);
void CaptureGekkoPadInput(Core::System& system);
void RecordPadHistory(int iteration_index);
void QueueFrameSave(int iteration_index, u32 logic_steps);
void PerformQueuedFrameSave(Core::System& system);
void OnDisplayedFrameStart();
bool HasPendingSave();
void ClearPendingSave();
bool ShouldSaveAfterIteration(int iteration_index);
bool ShouldSaveBeforeFirstIteration();
void WriteGekkoChecksumForIteration(int iteration_index, u32 checksum);
int GetCurrentIteration();
void SetCurrentIteration(int iteration);
bool IsTimeSynced();
bool IsRollingBack();
bool IsInRollbackMode();
u64 GetInitialRTCValue();
bool IsGekkoCpuStalled();
void SetGekkoCpuStalled(bool stalled);
// The CPU thread gave up a host-side stall wait and ran guest code instead (see
// WaitForGekkoFrames); counted for the harness.
void NoteGekkoStallFallback();
int GetFramesToAdvance();
// Emulation speed factor for rollback time sync (1.0 outside a rollback session).
double GetTimeSyncSpeedFactor();
}  // namespace NetPlay
