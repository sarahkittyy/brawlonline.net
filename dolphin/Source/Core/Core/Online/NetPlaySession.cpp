// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Netplay without a netplay UI (NetPlaySession.h). This mirrors what DolphinQt's MainWindow
// (NetPlayHost/NetPlayJoin) and NetPlayDialog (the NetPlayUI implementation) do, without the
// dialog. Used by online play (the whole-machine session backend) and by the harness.

#include "Core/Online/NetPlaySession.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <array>
#include <atomic>
#include <string_view>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Core.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "Core/NetPlayServer.h"
#include "Core/Online/OnlineSession.h"
#include "Core/System.h"
#include "Core/TitleDatabase.h"
#include "UICommon/GameFile.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/VideoConfig.h"

namespace Online::NetPlaySession
{
namespace
{
std::mutex s_frontend_mutex;
Frontend s_frontend;

Frontend GetFrontend()
{
  std::lock_guard lk(s_frontend_mutex);
  return s_frontend;
}

class HeadlessNetPlayUI final : public NetPlay::NetPlayUI
{
public:
  void BootGame(const std::string& filename,
                std::unique_ptr<BootSessionData> boot_session_data) override
  {
    // Called from NetPlayClient::StartGame, which we always run on the host thread.
    INFO_LOG_FMT(NETPLAY, "Netplay boot: {}", filename);
    m_got_stop_request = false;
    auto boot = BootParameters::GenerateFromFile(filename, std::move(*boot_session_data));
    const Frontend frontend = GetFrontend();
    if (!boot || !frontend.boot || !frontend.boot(std::move(boot)))
      ERROR_LOG_FMT(NETPLAY, "Netplay boot of {} failed", filename);
  }

  void StopGame() override
  {
    if (m_got_stop_request.exchange(true))
      return;
    Core::QueueHostJob([](Core::System& system) { Core::Stop(system); }, false);
  }

  bool IsHosting() const override { return m_hosting; }
  void Update() override {}
  void AppendChat(const std::string& msg) override
  {
    INFO_LOG_FMT(NETPLAY, "Netplay chat: {}", msg);
  }

  void OnMsgChangeGame(const NetPlay::SyncIdentifier& sync_identifier,
                       const std::string& netplay_name) override
  {
    std::lock_guard lk(m_mutex);
    m_current_game = sync_identifier;
    m_current_game_name = netplay_name;
    INFO_LOG_FMT(NETPLAY, "Netplay game changed to {}", netplay_name);
  }
  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig&) override {}

  void OnMsgStartGame() override
  {
    // Same as NetPlayDialog::OnMsgStartGame: look up the selected game and boot it. That has to
    // happen on the host thread (it boots the core).
    Core::QueueHostJob(
        [this](Core::System&) {
          std::shared_ptr<NetPlay::NetPlayClient> client = GetClient();
          if (!client)
            return;
          NetPlay::SyncIdentifier id;
          {
            std::lock_guard lk(m_mutex);
            id = m_current_game;
          }
          if (const auto game = FindGameFile(id))
            client->StartGame(game->GetFilePath());
          else
            ERROR_LOG_FMT(NETPLAY, "Netplay: selected game not found locally");
        },
        true);
  }
  void OnMsgStopGame() override {}
  void OnMsgPowerButton() override {}
  void OnPlayerConnect(const std::string& player) override
  {
    INFO_LOG_FMT(NETPLAY, "Netplay: {} joined", player);
  }
  void OnPlayerDisconnect(const std::string& player) override
  {
    INFO_LOG_FMT(NETPLAY, "Netplay: {} left", player);
  }
  void OnMinimumPadBufferChanged(u32) override {}
  void OnPlayerPadBufferChanged(u32) override {}
  void OnRollbackModeChanged(bool) override {}
  bool IsSpectator() override { return false; }
  void OnHostInputAuthorityChanged(bool) override {}
  void OnDesync(u32 frame, const std::string& player) override
  {
    WARN_LOG_FMT(NETPLAY, "Netplay: possible desync with {} at frame {}", player, frame);
    std::lock_guard lk(m_mutex);
    m_desync_reports++;
  }
  void OnConnectionLost() override
  {
    WARN_LOG_FMT(NETPLAY, "Netplay: connection lost");
    std::lock_guard lk(m_mutex);
    m_last_error = "connection lost";
  }
  void OnConnectionError(const std::string& message) override
  {
    WARN_LOG_FMT(NETPLAY, "Netplay: connection error: {}", message);
    std::lock_guard lk(m_mutex);
    m_last_error = message;
  }
  void OnTraversalError(Common::TraversalClient::FailureReason) override {}
  void OnTraversalStateChanged(Common::TraversalClient::State) override {}
  void OnGameStartAborted() override { WARN_LOG_FMT(NETPLAY, "Netplay: game start aborted"); }
  void OnGolferChanged(bool, const std::string&) override {}
  void OnTtlDetermined(u8) override {}

  bool IsRecording() override { return false; }

  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier& sync_identifier,
               NetPlay::SyncIdentifierComparison* found = nullptr) override
  {
    NetPlay::SyncIdentifierComparison temp;
    if (!found)
      found = &temp;
    *found = NetPlay::SyncIdentifierComparison::DifferentGame;

    std::lock_guard lk(m_mutex);
    for (const auto& file : m_candidates)
    {
      *found = std::min(*found, file->CompareSyncIdentifier(sync_identifier));
      if (*found == NetPlay::SyncIdentifierComparison::SameGame)
        return file;
    }
    return nullptr;
  }

  std::string FindGBARomPath(const std::array<u8, 20>&, std::string_view, int) override
  {
    return {};
  }
  void ShowGameDigestDialog(const std::string&) override {}
  void SetGameDigestProgress(int, int) override {}
  void SetGameDigestResult(int, const std::string&) override {}
  void AbortGameDigest() override {}
  void OnIndexAdded(bool, std::string) override {}
  void OnIndexRefreshFailed(std::string) override {}
  void ShowChunkedProgressDialog(const std::string&, u64, std::span<const int>) override {}
  void HideChunkedProgressDialog() override {}
  void SetChunkedProgress(int, u64) override {}

  void SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder) override
  {
    if (auto client = GetClient())
      client->SetWiiSyncData(nullptr, std::move(titles), std::move(redirect_folder));
  }

  // --- harness side ---

  void Reset(bool hosting)
  {
    std::lock_guard lk(m_mutex);
    m_hosting = hosting;
    m_current_game = {};
    m_current_game_name.clear();
    m_last_error.clear();
    m_desync_reports = 0;
    m_got_stop_request = false;
  }

  // Adds a game file to the list FindGameFile searches. Returns it, or nullptr if invalid.
  std::shared_ptr<const UICommon::GameFile> AddCandidate(std::string path)
  {
    // Dolphin's path helpers (and thus the generated "ID-<file name>" game ID of .dol/.elf
    // files, which is part of the SyncIdentifier) expect '/' separators, like the Qt game list
    // provides. A Windows-style path would otherwise produce a different ID on every machine.
    std::ranges::replace(path, '\\', '/');
    if (path.empty() || !File::Exists(path))
      return nullptr;
    std::lock_guard lk(m_mutex);
    for (const auto& file : m_candidates)
    {
      if (file->GetFilePath() == path)
        return file;
    }
    auto file = std::make_shared<const UICommon::GameFile>(path);
    if (!file->IsValid())
      return nullptr;
    m_candidates.push_back(file);
    return file;
  }

  void AddDefaultCandidates()
  {
    // Launcher .dol/.elf files shipped in the user directory, the default ISO and the
    // configured game folders (non-recursive), mirroring what the Qt game list would contain.
    std::vector<std::string> dirs{File::GetUserPath(D_USER_IDX) + "Launcher"};
    for (const auto& dir : Config::GetIsoPaths())
      dirs.push_back(dir);
    const std::vector<std::string_view> dir_views(dirs.begin(), dirs.end());
    static constexpr std::array<std::string_view, 9> exts{
        ".dol", ".elf", ".iso", ".rvz", ".wbfs", ".gcm", ".ciso", ".gcz", ".wia"};
    const auto files = Common::DoFileSearch(dir_views, exts, false);
    for (const auto& file : files)
      AddCandidate(file);
    AddCandidate(Config::Get(Config::MAIN_DEFAULT_ISO));
  }

  std::string LastError()
  {
    std::lock_guard lk(m_mutex);
    return m_last_error;
  }

  u64 DesyncReports()
  {
    std::lock_guard lk(m_mutex);
    return m_desync_reports;
  }

  NetPlay::SyncIdentifier CurrentGame()
  {
    std::lock_guard lk(m_mutex);
    return m_current_game;
  }

  std::string CurrentGameName()
  {
    std::lock_guard lk(m_mutex);
    return m_current_game_name;
  }

  std::shared_ptr<NetPlay::NetPlayClient> GetClient();

private:
  std::mutex m_mutex;
  bool m_hosting = false;
  std::atomic<bool> m_got_stop_request{false};
  NetPlay::SyncIdentifier m_current_game;
  std::string m_current_game_name;
  std::string m_last_error;
  u64 m_desync_reports = 0;
  std::vector<std::shared_ptr<const UICommon::GameFile>> m_candidates;
};

std::mutex s_session_mutex;
HeadlessNetPlayUI s_ui;
std::shared_ptr<NetPlay::NetPlayServer> s_server;
std::shared_ptr<NetPlay::NetPlayClient> s_client;
Common::EventHook s_state_hook;

std::shared_ptr<NetPlay::NetPlayClient> HeadlessNetPlayUI::GetClient()
{
  std::lock_guard lk(s_session_mutex);
  return s_client;
}

std::optional<std::string> CheckCanStart()
{
  const Frontend frontend = GetFrontend();
  if (!frontend.boot)
    return std::string("this Dolphin frontend has no headless netplay");
  if (frontend.other_session_active && frontend.other_session_active())
    return std::string("a NetPlay session is already open in the NetPlay window");
  if (!Core::IsUninitialized(Core::System::GetInstance()))
    return std::string("can't start a netplay session while a game is running");
  if (s_client || s_server)
    return std::string("a netplay session is already in progress");
  return std::nullopt;
}

// Moves the session out under the lock and destroys it after unlocking, so that netplay threads
// calling back into the UI (which takes s_session_mutex) can't deadlock against the destructors.
void ResetSession(std::unique_lock<std::mutex>& lock)
{
  auto client = std::move(s_client);
  auto server = std::move(s_server);
  s_state_hook.reset();
  lock.unlock();
  // Client first, like MainWindow::NetPlayQuit.
  client.reset();
  server.reset();
  lock.lock();
}

void InstallStateHook()
{
  // Mirrors NetPlayDialog: if emulation stops locally, ask the server to stop the game for
  // everyone.
  s_state_hook = Core::AddOnStateChangedCallback([](Core::State state) {
    if (state != Core::State::Uninitialized && state != Core::State::Stopping)
      return;
    std::shared_ptr<NetPlay::NetPlayClient> client;
    {
      std::lock_guard lk(s_session_mutex);
      client = s_client;
    }
    if (client)
      client->RequestStopGame();
  });
}

std::optional<std::string> CreateClientLocked(const std::string& host, u16 port,
                                              const std::string& name, u16 local_port = 0)
{
  auto client = std::make_shared<NetPlay::NetPlayClient>(
      host, port, &s_ui, name, NetPlay::NetTraversalConfig{false, "", 0}, local_port);
  if (!client->IsConnected())
  {
    const std::string error = s_ui.LastError();
    return fmt::format("failed to connect to {}:{}{}{}", host, port, error.empty() ? "" : ": ",
                       error);
  }
  s_client = std::move(client);
  InstallStateHook();
  return std::nullopt;
}
}  // namespace

std::optional<std::string> Host(u16 port, const std::string& game, const std::string& name,
                                bool rollback, std::optional<int> delay)
{
  std::unique_lock lk(s_session_mutex);
  if (auto error = CheckCanStart())
    return error;
  if (rollback && delay &&
      (*delay < static_cast<int>(NetPlay::ROLLBACK_DELAY_MIN) ||
       *delay > static_cast<int>(NetPlay::ROLLBACK_DELAY_MAX)))
  {
    return fmt::format("rollback delay must be {}-{}", NetPlay::ROLLBACK_DELAY_MIN,
                       NetPlay::ROLLBACK_DELAY_MAX);
  }

  s_ui.Reset(true);
  const auto game_file = s_ui.AddCandidate(game);
  if (!game_file)
    return fmt::format("'{}' is not a valid game file", game);

  // What NetPlayDialog's "Rollback [WIP]" network-mode action does: NetworkMode=rollback (the
  // server reads it in SetupNetSettings) and host input authority off.
  Config::SetCurrent(Config::NETPLAY_NETWORK_MODE, rollback ? "rollback" : "fixeddelay");
  Config::SetCurrent(Config::NETPLAY_NICKNAME, name);

  auto server = std::make_shared<NetPlay::NetPlayServer>(port, false, &s_ui,
                                                         NetPlay::NetTraversalConfig{false, "", 0});
  if (!server->is_connected)
    return fmt::format("failed to listen on port {}", port);

  server->SetHostInputAuthority(false);
  server->AdjustMinimumPadBufferSize(Config::Get(Config::NETPLAY_MINIMUM_BUFFER_SIZE));
  // Rollback: the input delay (frames from pad read to use). Fixed delay: the pad buffer.
  if (delay && rollback)
    server->SetRollbackDelay(static_cast<u32>(*delay));
  else if (delay)
    server->AdjustPadBufferSize(static_cast<unsigned int>(std::max(0, *delay)));

  const std::string netplay_name = game_file->GetNetPlayName(Core::TitleDatabase());
  server->ChangeGame(game_file->GetSyncIdentifier(), netplay_name);
  s_server = std::move(server);

  // Join our own server as player 1.
  if (auto error = CreateClientLocked("127.0.0.1", port, name))
  {
    ResetSession(lk);
    return error;
  }
  NOTICE_LOG_FMT(NETPLAY, "Netplay: hosting '{}' on port {} (rollback={})", netplay_name, port,
                 rollback);
  return std::nullopt;
}

std::optional<std::string> Join(const std::string& host, u16 port, const std::string& name,
                                const std::string& game_hint, u16 local_port)
{
  std::unique_lock lk(s_session_mutex);
  if (auto error = CheckCanStart())
    return error;

  s_ui.Reset(false);
  if (!game_hint.empty() && !s_ui.AddCandidate(game_hint))
    return fmt::format("'{}' is not a valid game file", game_hint);
  s_ui.AddDefaultCandidates();

  Config::SetCurrent(Config::NETPLAY_NICKNAME, name);
  if (auto error = CreateClientLocked(host, port, name, local_port))
  {
    ResetSession(lk);
    return error;
  }
  NOTICE_LOG_FMT(NETPLAY, "Netplay: joined {}:{} (local port {})", host, port, local_port);
  return std::nullopt;
}

std::optional<std::string> Start()
{
  std::shared_ptr<NetPlay::NetPlayServer> server;
  std::shared_ptr<NetPlay::NetPlayClient> client;
  {
    std::lock_guard lk(s_session_mutex);
    server = s_server;
    client = s_client;
  }
  if (!server || !client)
    return std::string("not hosting a netplay session");
  if (!Core::IsUninitialized(Core::System::GetInstance()))
    return std::string("a game is already running");
  if (!client->DoAllPlayersHaveGame())
    return std::string("not all players have the game (yet); check netplay_status and retry");

  // Same check NetPlayDialog::OnStart does before asking the server to start.
  if (!s_ui.FindGameFile(s_ui.CurrentGame()))
    return std::string("selected game not found");

  if (!server->RequestStartGame())
    return std::string("the server refused to start the game (see dolphin.log)");
  return std::nullopt;
}

std::optional<std::string> Leave()
{
  std::unique_lock lk(s_session_mutex);
  if (!s_client && !s_server)
    return std::string("no netplay session");
  ResetSession(lk);
  return std::nullopt;
}

bool IsActive()
{
  std::lock_guard lk(s_session_mutex);
  return s_client != nullptr || s_server != nullptr;
}

void ShutdownSession()
{
  std::unique_lock lk(s_session_mutex);
  ResetSession(lk);
}

picojson::value Status()
{
  std::shared_ptr<NetPlay::NetPlayServer> server;
  std::shared_ptr<NetPlay::NetPlayClient> client;
  {
    std::lock_guard lk(s_session_mutex);
    server = s_server;
    client = s_client;
  }
  if (!client && !server)
    return picojson::value();

  picojson::object result;
  result["role"] = picojson::value(server ? "host" : "client");
  result["connected"] = picojson::value(client && client->IsConnected());

  picojson::array players;
  if (client)
  {
    for (const NetPlay::Player* player : client->GetPlayers())
    {
      picojson::object p;
      p["pid"] = picojson::value(static_cast<double>(player->pid));
      p["name"] = picojson::value(player->name);
      p["ping_ms"] = picojson::value(static_cast<double>(player->ping));
      p["is_local"] = picojson::value(client->IsLocalPlayer(player->pid));
      players.emplace_back(std::move(p));
    }
  }
  result["players"] = picojson::value(std::move(players));

  const bool running = !Core::IsUninitialized(Core::System::GetInstance());
  result["game_running"] = picojson::value(running && NetPlay::IsNetPlayRunning());
  result["game"] = picojson::value(s_ui.CurrentGameName());
  result["last_error"] = picojson::value(s_ui.LastError());

  if (client)
  {
    result["local_pid"] = picojson::value(static_cast<double>(client->GetLocalPlayerId()));
    picojson::array pad_map;
    for (const auto pid : client->GetPadMapping())
      pad_map.emplace_back(static_cast<double>(pid));
    // pad_map[i] = pid controlling in-game port i (0 = nobody).
    result["pad_map"] = picojson::value(std::move(pad_map));

    // Local controller port -> in-game port, for this instance.
    picojson::array local_to_ingame;
    for (int local = 0; local < client->NumLocalPads(); ++local)
      local_to_ingame.emplace_back(static_cast<double>(client->LocalPadToInGamePad(local)));
    result["local_port_to_ingame_port"] = picojson::value(std::move(local_to_ingame));

    const auto& settings = client->GetNetSettings();
    picojson::object rollback;
    const auto stats = client->GetRollbackStats();
    rollback["enabled"] = picojson::value(settings.m_RollbackMode);
    // input_delay: the running game's (set at game start); announced_delay: the host's current.
    rollback["input_delay"] = picojson::value(static_cast<double>(settings.rollback_delay));
    rollback["announced_delay"] =
        picojson::value(static_cast<double>(client->GetAnnouncedRollbackDelay()));
    rollback["gekko_session"] = picojson::value(stats.gekko_session);
    rollback["session_started"] = picojson::value(stats.session_started);
    rollback["current_frame"] = picojson::value(static_cast<double>(stats.current_frame));
    rollback["rollbacks"] = picojson::value(static_cast<double>(stats.rollbacks));
    rollback["max_rollback_frames"] =
        picojson::value(static_cast<double>(stats.max_rollback_frames));
    rollback["frames_resimulated"] = picojson::value(static_cast<double>(stats.frames_resimulated));
    rollback["desyncs_detected"] = picojson::value(static_cast<double>(stats.desyncs_detected));
    rollback["last_desync_frame"] = picojson::value(static_cast<double>(stats.last_desync_frame));
    rollback["frames_ahead"] = picojson::value(static_cast<double>(stats.frames_ahead));
    rollback["time_sync_speed"] = picojson::value(stats.time_sync_speed);
    rollback["stall_polls"] = picojson::value(static_cast<double>(stats.stall_polls));
    rollback["peer_disconnects"] = picojson::value(static_cast<double>(stats.peer_disconnects));
    rollback["skipped_loads"] = picojson::value(static_cast<double>(stats.skipped_loads));
    rollback["dropped_advances"] = picojson::value(static_cast<double>(stats.dropped_advances));
    rollback["stall_fallbacks"] = picojson::value(static_cast<double>(stats.stall_fallbacks));
    rollback["synctest"] = picojson::value(stats.synctest);
    rollback["synctest_mismatches"] =
        picojson::value(static_cast<double>(stats.synctest_mismatches));
    rollback["save_count"] = picojson::value(static_cast<double>(stats.save_count));
    rollback["save_us_total"] = picojson::value(static_cast<double>(stats.save_us_total));
    rollback["save_us_max"] = picojson::value(static_cast<double>(stats.save_us_max));
    rollback["load_count"] = picojson::value(static_cast<double>(stats.load_count));
    rollback["load_us_total"] = picojson::value(static_cast<double>(stats.load_us_total));
    rollback["load_us_max"] = picojson::value(static_cast<double>(stats.load_us_max));
    rollback["save_sync_count"] = picojson::value(static_cast<double>(stats.save_sync_count));
    rollback["save_sync_us_total"] =
        picojson::value(static_cast<double>(stats.save_sync_us_total));
    rollback["save_sync_us_max"] = picojson::value(static_cast<double>(stats.save_sync_us_max));
    {
      // Dual core: whether the GPU thread runs in deterministic mode (CPU-side FIFO/CP/PE).
      auto& system = Core::System::GetInstance();
      rollback["gpu_deterministic"] =
          picojson::value(system.IsDualCoreMode() && system.GetFifo().UseDeterministicGPUThread());
      const auto compiles = VideoCommon::GetSyncPipelineCompileStats();
      rollback["gpu_sync_compiles"] = picojson::value(static_cast<double>(compiles.count));
      rollback["gpu_sync_compile_us_total"] =
          picojson::value(static_cast<double>(compiles.us_total));
      rollback["gpu_sync_compile_us_max"] = picojson::value(static_cast<double>(compiles.us_max));
      rollback["gpu_sync_utility_compiles"] =
          picojson::value(static_cast<double>(compiles.utility_count));
      rollback["gpu_sync_utility_compile_us_total"] =
          picojson::value(static_cast<double>(compiles.utility_us_total));
      rollback["shader_compilation_mode"] =
          picojson::value(static_cast<double>(g_ActiveConfig.iShaderCompilationMode));
    }
    picojson::array by_depth;
    for (size_t depth = 1; depth < stats.by_depth.size(); ++depth)
    {
      const auto& d = stats.by_depth[depth];
      if (d.count == 0)
        continue;
      picojson::object o;
      o["depth"] = picojson::value(static_cast<double>(depth));
      o["count"] = picojson::value(static_cast<double>(d.count));
      o["load_us_total"] = picojson::value(static_cast<double>(d.load_us_total));
      o["load_us_max"] = picojson::value(static_cast<double>(d.load_us_max));
      o["resim_us_total"] = picojson::value(static_cast<double>(d.resim_us_total));
      o["resim_us_max"] = picojson::value(static_cast<double>(d.resim_us_max));
      by_depth.emplace_back(std::move(o));
    }
    rollback["by_depth"] = picojson::value(std::move(by_depth));
    rollback["max_rollback_window"] = picojson::value(static_cast<double>(MAX_ROLLBACK_FRAMES));
    rollback["netplay_desync_reports"] = picojson::value(static_cast<double>(s_ui.DesyncReports()));
    result["rollback"] = picojson::value(std::move(rollback));
  }
  else
  {
    result["rollback"] = picojson::value();
  }
  return picojson::value(std::move(result));
}

picojson::value PadHistory(s64 since_frame, bool with_raw)
{
  std::shared_ptr<NetPlay::NetPlayClient> client;
  {
    std::lock_guard lk(s_session_mutex);
    client = s_client;
  }
  if (!client)
    return picojson::value();

  picojson::array frames;
  for (const auto& entry : client->GetPadHistory(since_frame))
  {
    picojson::array row;
    row.emplace_back(static_cast<double>(entry.frame));
    for (const u32 buttons : entry.buttons)
      row.emplace_back(static_cast<double>(buttons));
    row.emplace_back(static_cast<double>(entry.crc));
    for (const u32 value : entry.state)
      row.emplace_back(static_cast<double>(value));
    if (with_raw)
    {
      std::string hex;
      hex.reserve(entry.raw.size() * 2);
      for (const u8 b : entry.raw)
        hex += fmt::format("{:02x}", b);
      row.emplace_back(std::move(hex));
    }
    frames.emplace_back(std::move(row));
  }
  picojson::object result;
  result["frames"] = picojson::value(std::move(frames));
  return picojson::value(std::move(result));
}

picojson::value ChunkHashes(s64 frame)
{
  std::shared_ptr<NetPlay::NetPlayClient> client;
  {
    std::lock_guard lk(s_session_mutex);
    client = s_client;
  }
  if (!client)
    return picojson::value();
  const auto hashes = client->GetChunkHashes(frame);
  if (!hashes)
    return picojson::value();
  picojson::array out;
  for (const u64 h : *hashes)
    out.emplace_back(fmt::format("{:016x}", h));
  picojson::object result;
  result["frame"] = picojson::value(static_cast<double>(frame));
  result["chunk_size"] =
      picojson::value(static_cast<double>(client->GetChunkHashSize()));
  result["hashes"] = picojson::value(std::move(out));
  return picojson::value(std::move(result));
}
namespace
{
std::mutex s_online_options_mutex;
OnlineOptions s_online_options;

std::string DefaultOnlineGame()
{
  const std::string launcher =
      File::GetUserPath(D_LAUNCHERS_IDX) + "Project+ Netplay Launcher.dol";
  if (File::Exists(launcher))
    return launcher;
  return Config::Get(Config::MAIN_DEFAULT_ISO);
}

// Online::SessionBackend over Dolphin's whole-machine netplay (synchronized boot, option A of
// docs/backend-design.md 5.1).
class NetPlayOnlineBackend final : public Online::SessionBackend
{
public:
  ~NetPlayOnlineBackend() override { StopWorker(); }

  const char* Name() const override { return "netplay"; }

  std::optional<std::string> Start(const Online::Match& match, Online::P2PLink link,
                                   const picojson::object&) override
  {
    StopWorker();
    OnlineOptions options;
    {
      std::lock_guard lk(s_online_options_mutex);
      options = s_online_options;
    }
    if (options.game.empty())
      options.game = DefaultOnlineGame();
    {
      std::lock_guard lk(m_mutex);
      m_match = match;
      m_options = options;
      m_phase = "starting";
      m_error.clear();
      m_peer.clear();
    }
    // Free the punched port: the netplay server (host) or client (guest) binds it again.
    link.Release();
    m_stop = false;
    m_worker = std::thread([this] { Run(); });
    return std::nullopt;
  }

  void Stop() override
  {
    StopWorker();
    if (IsActive())
      Leave();
    std::lock_guard lk(m_mutex);
    if (!m_phase.empty())
      m_phase = "ended";
  }

  Online::SessionStatus Status() const override
  {
    Online::SessionStatus status;
    std::lock_guard lk(m_mutex);
    status.phase = m_phase;
    status.error = m_error;
    if (m_phase == "joined" || m_phase == "started")
    {
      const bool running = !Core::IsUninitialized(Core::System::GetInstance()) &&
                           NetPlay::IsNetPlayRunning();
      if (running)
        status.phase = "running";
    }
    if (m_match)
    {
      status.detail["role"] = picojson::value(m_match->is_host ? "host" : "guest");
      status.detail["local_port"] = picojson::value(static_cast<double>(m_match->local_port));
      status.detail["match_id"] = picojson::value(m_match->match_id);
      if (!m_peer.empty())
        status.detail["peer"] = picojson::value(m_peer);
      status.detail["game"] = picojson::value(m_options.game);
    }
    return status;
  }

private:
  void SetPhase(std::string phase, std::string error = {})
  {
    std::lock_guard lk(m_mutex);
    m_phase = std::move(phase);
    if (!error.empty())
    {
      m_error = std::move(error);
      WARN_LOG_FMT(NETPLAY, "Online session: {}", m_error);
    }
  }

  bool Sleep(std::chrono::milliseconds ms)
  {
    const auto end = std::chrono::steady_clock::now() + ms;
    while (std::chrono::steady_clock::now() < end)
    {
      if (m_stop)
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !m_stop;
  }

  void Run()
  {
    Common::SetCurrentThreadName("Online netplay session");
    Online::Match match;
    OnlineOptions options;
    {
      std::lock_guard lk(m_mutex);
      match = *m_match;
      options = m_options;
    }
    auto& system = Core::System::GetInstance();

    // A game running locally (searching from the game's own menus) is stopped first: both
    // machines boot together under netplay.
    if (!Core::IsUninitialized(system))
    {
      SetPhase("stopping_game");
      Core::QueueHostJob([](Core::System& sys) { Core::Stop(sys); }, false);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while (!Core::IsUninitialized(system))
      {
        if (std::chrono::steady_clock::now() > deadline)
          return SetPhase("error", "the running game did not stop");
        if (!Sleep(std::chrono::milliseconds(50)))
          return;
      }
    }

    std::string name;
    for (const auto& p : match.players)
    {
      if (p.is_local)
        name = !p.display_name.empty() ? p.display_name : p.connect_code;
    }
    if (name.empty())
      name = match.is_host ? "host" : "guest";

    if (match.is_host)
    {
      SetPhase("hosting");
      std::optional<std::string> error;
      for (int attempt = 0; attempt < 10; ++attempt)
      {
        error = Host(match.local_port, options.game, name, true, options.delay);
        if (!error)
          break;
        if (!Sleep(std::chrono::milliseconds(100)))
          return;
      }
      if (error)
        return SetPhase("error", "host: " + *error);
      if (!options.auto_start)
        return SetPhase("hosting");

      SetPhase("waiting_for_peer");
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (true)
      {
        std::shared_ptr<NetPlay::NetPlayClient> client = s_ui.GetClient();
        if (!client)
          return SetPhase("error", "the netplay session ended");
        const auto players = client->GetPlayers();
        if (players.size() >= 2 && client->DoAllPlayersHaveGame())
        {
          std::lock_guard lk(m_mutex);
          for (const auto* player : players)
          {
            if (!client->IsLocalPlayer(player->pid))
              m_peer = player->name;
          }
          break;
        }
        if (std::chrono::steady_clock::now() > deadline)
          return SetPhase("error", "the guest did not join within 30 s");
        if (!Sleep(std::chrono::milliseconds(100)))
          return;
      }
      if (auto start_error = NetPlaySession::Start())
        return SetPhase("error", "start: " + *start_error);
      SetPhase("started");
      return;
    }

    // Guest: connect to the address the P2P connection came up with (the host's punched port).
    const Online::Endpoint host_ep = !match.connected.empty() ? match.connected[0] :
                                     !match.remotes.empty()   ? match.remotes[0] :
                                                                Online::Endpoint{};
    if (host_ep.port == 0)
      return SetPhase("error", "no host address");
    {
      std::lock_guard lk(m_mutex);
      m_peer = host_ep.ToString();
    }
    SetPhase("joining");
    // Give the host time to bind its port again (it released the P2P link at the same time).
    if (!Sleep(std::chrono::milliseconds(300)))
      return;
    // NetPlayClient waits 5 s for each connect; a few attempts cover a slow host.
    std::optional<std::string> error;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
      error = Join(host_ep.ip, host_ep.port, name, options.game, match.local_port);
      if (!error)
        break;
      if (!Sleep(std::chrono::milliseconds(250)))
        return;
    }
    if (error)
      return SetPhase("error", "join: " + *error);
    SetPhase("joined");
  }

  void StopWorker()
  {
    m_stop = true;
    if (m_worker.joinable())
      m_worker.join();
  }

  mutable std::mutex m_mutex;
  std::optional<Online::Match> m_match;
  OnlineOptions m_options;
  std::string m_phase;
  std::string m_error;
  std::string m_peer;
  std::atomic<bool> m_stop{false};
  std::thread m_worker;
};
}  // namespace

void SetOnlineOptions(const OnlineOptions& options)
{
  std::lock_guard lk(s_online_options_mutex);
  s_online_options = options;
}

std::unique_ptr<Online::SessionBackend> MakeOnlineBackend()
{
  return std::make_unique<NetPlayOnlineBackend>();
}

void SetFrontend(Frontend frontend)
{
  {
    std::lock_guard lk(s_frontend_mutex);
    s_frontend = std::move(frontend);
  }
  Online::Session::RegisterFactory("netplay", [] { return MakeOnlineBackend(); });
}

bool HasFrontend()
{
  std::lock_guard lk(s_frontend_mutex);
  return static_cast<bool>(s_frontend.boot);
}

void Shutdown()
{
  // The backend first: its worker may be hosting or joining right now.
  Online::Session::SetBackend(nullptr);
  ShutdownSession();
}
}  // namespace Online::NetPlaySession
