// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Matchmaking client: a port of Slippi's SlippiMatchmaking (Slippi Dolphin
// Source/Core/Core/Slippi/SlippiMatchmaking.cpp) and of the connect phase of SlippiNetplayClient
// (SlippiNetplay.cpp, ThreadFunc up to "Slippi online connection successful"). It speaks Slippi's
// ENet + JSON ticket protocol byte for byte with our mm server (server/crates/mm):
//
// 1. INITIALIZING: bind UDP 41000 + rand % 10000 (or the forced port); this port is kept for the
//    P2P connection, which is the hole punch. ENet-connect to the mm server (20 x 500 ms), send
//    create-ticket with the LAN address, wait 5 s for create-ticket-resp.
// 2. MATCHMAKING: wait for get-ticket-resp in 2 s windows, forever (the server owns expiry). Pick
//    each peer's address with Slippi's rule (LAN address when both share an external IP).
//    Disconnect from mm.
// 3. OPPONENT_CONNECTING: ENet-connect to every peer from the same port, from both sides at once,
//    for 8 s. Direct/unranked/ranked: on failure, back to 1 with a new ticket. Teams: error.
// 4. CONNECTION_SUCCESS: the Match and the live P2P link are handed to the callback (the online
//    session, OnlineSession.h), or kept for TakeLink().
//
// Errors (ERROR_ENCOUNTERED) carry Slippi's message strings, or the server's `error` verbatim.

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Online/OnlineSession.h"

namespace Online
{
class User;

class Matchmaking
{
public:
  // SlippiMatchmaking::OnlinePlayMode
  enum class Mode : u8
  {
    Ranked = 0,
    Unranked = 1,
    Direct = 2,
    Teams = 3,
    Party = 4,
  };

  // SlippiMatchmaking::ProcessState; the game reads these values (GET_MATCH_STATE).
  enum class State : u8
  {
    Idle = 0,
    Initializing = 1,
    Matchmaking = 2,
    OpponentConnecting = 3,
    ConnectionSuccess = 4,
    ErrorEncountered = 5,
  };

  // Where an error came from, so a UI can tell a server refusal from a local failure. The text
  // (GetErrorMessage) is what the game shows.
  enum class ErrorSource : u8
  {
    None,
    Client,        // local failure (socket, mm unreachable, no reply, bad reply)
    CreateTicket,  // create-ticket-resp.error from the server (bad key, unsupported mode, ...)
    GetTicket,     // get-ticket-resp.error from the server (expired, replaced, too many tries)
    Connection,    // lost the mm connection while waiting, or the P2P connect failed (teams)
  };

  struct SearchSettings
  {
    Mode mode = Mode::Direct;
    // The opponent's code as typed (e.g. "BOB#123"); sent as full-width Shift-JIS bytes like
    // Melee's name-entry screen produces. Empty for queue modes.
    std::string connect_code;
  };

  using ConnectedCallback = std::function<void(const Match&, P2PLink& link)>;

  // `on_connected` runs on the matchmaking thread once the P2P connection is up. It may move the
  // link out (to a session); if it leaves it open (or there is no callback) the link stays here
  // for TakeLink(), until this object is destroyed.
  explicit Matchmaking(User* user, ConnectedCallback on_connected = {});
  ~Matchmaking();

  Matchmaking(const Matchmaking&) = delete;
  Matchmaking& operator=(const Matchmaking&) = delete;

  void FindMatch(const SearchSettings& settings);
  State GetState() const { return m_state.load(); }
  bool IsSearching() const;
  std::string GetErrorMessage() const;
  ErrorSource GetErrorSource() const { return m_error_source.load(); }
  const SearchSettings& GetSearchSettings() const { return m_search_settings; }

  // The last get-ticket-resp result (valid from OPPONENT_CONNECTING on).
  std::optional<Match> GetMatch() const;
  int LocalPlayerIndex() const;
  u8 RemotePlayerCount() const;
  P2PLink TakeLink();

  // Diagnostics.
  u16 GetLocalPort() const { return m_host_port.load(); }
  std::string GetLanAddress() const;
  std::string GetServerAddress() const;
  int GetTicketCount() const { return m_ticket_count.load(); }
  int GetConnectAttempts() const { return m_connect_attempts.load(); }
  picojson::value GetLastTicketResponse() const;

  static const char* ModeName(Mode mode);
  static const char* StateName(State state);
  static const char* ErrorSourceName(ErrorSource source);
  static bool IsFixedRulesMode(Mode mode);
  // Full-width Shift-JIS bytes of an ASCII code (what Melee's code entry puts in FIND_OPPONENT).
  static std::vector<u8> EncodeConnectCode(const std::string& code);

private:
  void MatchmakeThread();
  void StartMatchmaking();
  void HandleMatchmaking();
  void HandleConnecting();

  void SetError(ErrorSource source, std::string message);
  void SendMmMessage(const picojson::value& msg);
  // 0 = message, -1 = timeout, -2 = lost the server.
  int ReceiveMmMessage(picojson::value& msg, int timeout_ms);
  void DisconnectFromServer();
  void TerminateMmConnection();

  User* m_user;
  ConnectedCallback m_on_connected;

  std::default_random_engine m_generator;

  _ENetHost* m_client = nullptr;
  _ENetPeer* m_server = nullptr;
  bool m_is_mm_connected = false;
  std::atomic<bool> m_is_mm_terminated{false};

  std::thread m_matchmake_thread;
  SearchSettings m_search_settings;

  std::atomic<State> m_state{State::Idle};
  std::atomic<ErrorSource> m_error_source{ErrorSource::None};

  std::atomic<u16> m_host_port{0};
  std::atomic<int> m_ticket_count{0};
  std::atomic<int> m_connect_attempts{0};

  mutable std::mutex m_mutex;  // guards the fields below
  std::string m_error_msg;
  std::string m_lan_address;
  std::string m_server_address;
  picojson::value m_last_ticket_response;
  std::optional<Match> m_match;
  P2PLink m_link;
};
}  // namespace Online
