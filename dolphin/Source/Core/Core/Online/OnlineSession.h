// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The hand-off from matchmaking to the rollback session.
//
// Matchmaking (Matchmaking.h) ends, like Slippi's, with a live P2P ENet connection to every remote
// player, made from the same local UDP port the mm server saw (the hole punch). What it hands over
// is a Match: who the players are, who decides (isHost), the local port and the peer endpoints that
// worked, plus that connection (P2PLink).
//
// A SessionBackend turns a Match into a running rollback session. Exactly one backend is active
// at a time. Backends are made by named factories, and the active one is chosen by name
// ([Online] SessionBackend, default "gameplay"):
// - "gameplay": the gameplay-only, Slippi-style session (Gprb::Session, Rollback/
//   GameplayOnlineBackend.h): both games stay on their own online character select, exchange
//   selections and start the same match; only the match is rolled back. Both frontends register
//   its factory at start-up (Gprb::RegisterOnlineBackend) and then call SelectConfigured().
//   Contract: docs/backend-design.md 5.5;
// - "netplay": the whole-machine netplay backend (Dolphin's NetPlayServer/NetPlayClient with
//   GekkoNet, the synchronized-boot "option A" of docs/backend-design.md 5.1), in
//   Online/NetPlaySession.h, kept as a fallback. Its factory is registered once a frontend can
//   boot games headlessly (NetPlaySession::SetFrontend: DolphinQt at start-up, DolphinNoGUI under
//   the harness);
// - "record": the harness's test backend (records the hand-off, holds the link).
//
// Backends that bind their own UDP socket (both of the above) call P2PLink::Release() first: it
// closes the ENet connection politely and frees the port, which they then bind again, so the NAT
// mapping the mm server and the peer already used stays the one in use.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

// ENet's types, forward-declared so this header does not pull in enet.h (and with it winsock.h,
// whose macros clash with ordinary names such as s_host).
struct _ENetHost;
struct _ENetPeer;

namespace Online
{
struct PlayerInfo
{
  std::string uid;
  std::string display_name;
  std::string connect_code;
  int port = 0;  // controller port 1-4 from the server
  bool is_local = false;
  bool is_bot = false;
  std::string ip_address;      // external ip:port the mm server saw
  std::string ip_address_lan;  // the LAN ip:port the player reported
  std::vector<std::string> chat_messages;
};

struct Endpoint
{
  std::string ip;
  u16 port = 0;
  std::string ToString() const;
};

// One finished matchmaking: the get-ticket-resp plus what the P2P connect found.
struct Match
{
  std::string match_id;
  bool is_host = false;          // Slippi's "decider"
  int local_player_index = 0;    // 0-based (players[].port - 1 of the local player)
  u16 local_port = 0;            // the punched UDP port this side used for mm and P2P
  std::vector<PlayerInfo> players;  // in server order
  std::vector<u16> stages;
  u32 items = 0;
  // One per remote player, in the order of `players` without the local one: the address chosen
  // with Slippi's rule (LAN address when both share an external IP) ...
  std::vector<Endpoint> remotes;
  // ... and the address the ENet connection actually came up with (the peer's source address).
  std::vector<Endpoint> connected;
  u64 connect_ms = 0;  // time the P2P connect took
};

// The live P2P ENet connection from the connect window.
class P2PLink
{
public:
  struct HostDeleter
  {
    void operator()(_ENetHost* host) const;
  };
  using HostPtr = std::unique_ptr<_ENetHost, HostDeleter>;

  P2PLink() = default;
  P2PLink(HostPtr host, std::vector<_ENetPeer*> peers);
  ~P2PLink();
  P2PLink(P2PLink&&) noexcept;
  P2PLink& operator=(P2PLink&&) noexcept;

  bool IsOpen() const { return m_host != nullptr; }
  _ENetHost* GetHost() const { return m_host.get(); }
  const std::vector<_ENetPeer*>& GetPeers() const { return m_peers; }

  // For backends that keep using this ENet host (Slippi does: SlippiNetplayClient owns it).
  HostPtr TakeHost();
  // Disconnects the peers (waiting up to `linger_ms` for the disconnects to be acknowledged) and
  // destroys the host, so the local port can be bound again.
  void Release(u32 linger_ms = 1000);

private:
  HostPtr m_host;
  std::vector<_ENetPeer*> m_peers;
};

struct SessionStatus
{
  std::string backend;  // "netplay", "gameplay", ... or "" when none is registered
  std::string phase;    // backend-defined: "idle", "starting", "connecting", "running", "ended"...
  std::string error;
  picojson::object detail;  // backend-specific
};

class SessionBackend
{
public:
  virtual ~SessionBackend() = default;
  virtual const char* Name() const = 0;
  // Starts the session for `match` over `link` (the backend owns it from now on). `selections`
  // is this player's lock-in (free-form JSON object, e.g. {"character": "fox", "costume": 0});
  // backends that make the character select shared state (whole-machine) ignore it. Called on
  // the matchmaking thread; must not block for long.
  virtual std::optional<std::string> Start(const Match& match, P2PLink link,
                                           const picojson::object& selections) = 0;
  // Leaves the session (CLEANUP_CONNECTION / hold Z on the CSS).
  virtual void Stop() = 0;
  virtual SessionStatus Status() const = 0;
};

namespace Session
{
using Factory = std::function<std::unique_ptr<SessionBackend>()>;
// Adds (or replaces) a named backend factory. Does not change the active backend.
void RegisterFactory(const std::string& name, Factory factory);
std::vector<std::string> FactoryNames();
// Makes the named backend the active one ("none" unregisters). Error if no such factory.
std::optional<std::string> Select(const std::string& name);
// Select([Online] SessionBackend). Logs and keeps the current backend if it is not registered.
void SelectConfigured();

// Registers the backend that Start() uses (nullptr unregisters). Stops a running session first.
void SetBackend(std::unique_ptr<SessionBackend> backend);
bool HasBackend();
// OnlineSession::Start: hands a matched, connected peer to the registered backend.
std::optional<std::string> Start(const Match& match, P2PLink link,
                                 const picojson::object& selections = {});
void Stop();
SessionStatus Status();
}  // namespace Session
}  // namespace Online
