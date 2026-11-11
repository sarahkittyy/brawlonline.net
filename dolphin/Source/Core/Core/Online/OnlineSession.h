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

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Online/MatchRules.h"
#include "Core/Online/SetFormat.h"

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
  // "[ip]:port" where mm saw the player's IPv6 probe come from (Online/Ipv6.h); empty without.
  std::string ip_address6;
  std::vector<std::string> chat_messages;
};

struct Endpoint
{
  std::string ip;  // an IPv4 address, or an IPv6 one (no brackets)
  u16 port = 0;
  // "ip:port", "[ip]:port" for IPv6.
  std::string ToString() const;
  bool IsIpv6() const { return ip.find(':') != std::string::npos; }
};

// One finished matchmaking: the get-ticket-resp plus what the P2P connect found.
struct Match
{
  std::string match_id;
  // The search mode that made this match (Matchmaking::Mode: 0 ranked, 1 unranked, 2 direct,
  // 3 teams); -1 unknown.
  int mode = -1;
  bool is_host = false;          // Slippi's "decider"
  int local_player_index = 0;    // 0-based (players[].port - 1 of the local player)
  u16 local_port = 0;            // the punched UDP port this side used for mm and P2P
  std::vector<PlayerInfo> players;  // in server order
  std::vector<u16> stages;
  // A room whose loser picks from the stage list (get-ticket-resp `pickFromStages`): a stage pick
  // must be one of `stages`.
  bool pick_from_stages = false;
  // P+'s alternate stages (get-ticket-resp `alternateStages`: Direct, Teams, a room whose ruleset
  // has them on): a stage pick plays the layout its held L/R/Z selects; else the normal layout.
  bool alternate_stages = false;
  // Every player picks every game's stage at once, game 1 too; the first pick the host sees plays
  // (get-ticket-resp `anyonePicks`: Direct, 2026-10-29).
  bool anyone_picks = false;
  // Ranked: the starter stages struck for game 1 (get-ticket-resp `starters`, ours; Slippi's
  // client has them built in). Empty: Online::GameSetup's default.
  std::vector<u16> starters;
  u32 items = 0;
  // The match's rules (get-ticket-resp `rules`, MatchRules.h); nullopt: the server sent none and the
  // game plays its built-in set (P+'s competitive defaults).
  std::optional<MatchRules> rules;
  // A room's set (get-ticket-resp `set`, SetFormat.h): the session plays its games from the set's
  // game `first_game` on, GameSetup running the strikes, bans and picks.
  std::optional<GameSetup::SetTicket> set;
  // One per remote player, in the order of `players` without the local one: the address chosen
  // with Slippi's rule (LAN address when both share an external IP) ...
  std::vector<Endpoint> remotes;
  // ... and the address the ENet connection actually came up with (the peer's source address).
  // An IPv6 one (Endpoint::IsIpv6) only when IPv4 did not connect to that player.
  std::vector<Endpoint> connected;
  // IPv6 (Online/Ipv6.h), one per remote player as `remotes`: its IPv6 address when both it and
  // this player have one (ip empty otherwise), tried beside IPv4. The session may switch a
  // player's packets to it if the IPv4 path goes quiet, and back.
  std::vector<Endpoint> remotes6;
  // This player's IPv6 address the remotes6 are reached from (empty: no IPv6 in this match), on
  // local_port.
  std::string local_ip6;
  u64 connect_ms = 0;  // time the P2P connect took
  // The players' ports (1-4) the P2P connect timed out on (set with the failure; a room tells the
  // room's server, which names the pair to every member).
  std::vector<int> unreachable_ports;
  // A room's game (docs/rooms-protocol.md §3; mode 3): the room host's slot (1-4; it decides),
  // the Teams switch (and its team colours) and who picks the first stage. room_host_port 0: not a
  // room game.
  int room_host_port = 0;
  bool room_teams = false;
  bool room_team_colors = false;
  u8 room_pickers = 0;
  // A 2v2 match (mode 5, get-ticket-resp `splits`): the team splits the session may play, a team
  // (0, 1) per in-game port, the most even first; game 1 plays the first, every later game the
  // most even one that is not the last game's. Empty: not a 2v2 match.
  std::vector<std::array<u8, 4>> twos_splits;
  // A 2v2 party's pair room (OnlineClient.h; a party ticket's match, mode 3): the two partners play
  // the waiting room together while their 2v2 search runs. Its ports are the party's slots, the
  // party's host deciding (room_host_port).
  bool pair_room = false;
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
  // `host6`, `peers6`: the IPv6 host of the connect (Online/Ipv6.h) and its connections, if any.
  P2PLink(HostPtr host, std::vector<_ENetPeer*> peers, HostPtr host6 = {},
          std::vector<_ENetPeer*> peers6 = {});
  ~P2PLink();
  P2PLink(P2PLink&&) noexcept;
  P2PLink& operator=(P2PLink&&) noexcept;

  bool IsOpen() const { return m_host != nullptr; }
  _ENetHost* GetHost() const { return m_host.get(); }
  const std::vector<_ENetPeer*>& GetPeers() const { return m_peers; }

  // For backends that keep using this ENet host (Slippi does: SlippiNetplayClient owns it).
  HostPtr TakeHost();
  // Disconnects the peers (waiting up to `linger_ms` for the disconnects to be acknowledged) and
  // destroys the host, so the local port can be bound again. The IPv6 host too.
  void Release(u32 linger_ms = 1000);

private:
  HostPtr m_host;
  std::vector<_ENetPeer*> m_peers;
  HostPtr m_host6;
  std::vector<_ENetPeer*> m_peers6;
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
// The registered backend's name ("" when none).
std::string BackendName();
}  // namespace Session
}  // namespace Online
