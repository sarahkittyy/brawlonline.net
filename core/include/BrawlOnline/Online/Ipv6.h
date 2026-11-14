// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// IPv6 beside IPv4 for the P2P connections (Sarah's board, 2026-11-11: players behind carrier NAT
// and other NATs no hole punch gets through, where IPv6 has no NAT at all).
//
// IPv4 stays exactly as it was: every search still connects to mm over IPv4, and every P2P connect
// still tries IPv4 first and uses it whenever it connects. IPv6 is only added: a game that has a
// working IPv6 address sends a probe from it to mm's IPv6 listener (server/crates/common/src/
// probe6.rs), mm tells the players of a match each other's IPv6 address (`ipAddress6`), and the
// connect tries both at once. A pair whose IPv4 connect fails can then play over IPv6. A game
// without IPv6 (no address, no route, IPv6 off, mm without MM_LISTEN6) sends nothing over IPv6 and
// meets nothing of it.
//
// The IPv6 sockets are ENet's, with Brawl Online's IPv6 layer (Externals/enet/ipv6): an IPv6
// address is a handle in ENetAddress::host, so the connect code treats both families alike.

#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <enet/enet.h>

#include "Common/CommonTypes.h"

namespace Online::Ipv6
{
// [Online] IPv6 is on, there is a server name for it, and this build's ENet has IPv6.
bool Enabled();

// Resolves the matchmaking server's IPv6 address (Config::GetMatchmakingHost6) in the background,
// as UPnP's discovery: at boot, so a search does not wait for DNS.
void StartDiscovery();
// The server's IPv6 address and port, waiting up to `wait` for the discovery. nullopt: none
// (IPv6 off, no AAAA record, DNS failed).
std::optional<ENetAddress> Server(std::chrono::milliseconds wait);

// This machine's IPv6 address toward `server` (the one the system sends from; nothing is sent),
// if other players on the internet can reach it: a global unicast address (2000::/3, not 6to4,
// Teredo or documentation). ::1 counts only toward a server on ::1 (the tests). The port is 0.
std::optional<ENetAddress> LocalAddressToward(const ENetAddress& server, std::string* why);

// "[2001:db8::1]:41000" -> the handle and port. nullopt for anything else (IPv4 included).
std::optional<ENetAddress> ParseEndpoint(const std::string& text);

// A random token for a search's probes (`create-ticket`'s `ipv6Token`): 32 hex characters.
std::string NewToken();

// The probe datagram (server/crates/common/src/probe6.rs) for `token`.
std::string ProbeRequest(const std::string& token);
// The address a probe reply says mm saw, or nullopt if `data` is no reply.
std::optional<ENetAddress> ParseProbeReply(const u8* data, size_t size);

// A search's (or a room session's) probe socket: bound to the local IPv6 address and the P2P
// port, so mm sees the address and port the peers will be told.
class Probe
{
public:
  Probe() = default;
  ~Probe();
  Probe(const Probe&) = delete;
  Probe& operator=(const Probe&) = delete;

  // Binds `local` (an address from LocalAddressToward, with the port) and sends the probe a few
  // times. False (and closed) if the socket cannot be made or bound; `why` says why.
  bool Open(const ENetAddress& local, const ENetAddress& server, const std::string& token,
            std::string* why);
  // Sends the probe again (a lost datagram is not retried by anything else).
  void Send();
  // A reply that came (non-blocking): where mm saw the probe from.
  std::optional<ENetAddress> Reply();
  void Close();
  bool IsOpen() const { return m_socket != ENET_SOCKET_NULL; }
  const ENetAddress& Local() const { return m_local; }

private:
  ENetSocket m_socket = ENET_SOCKET_NULL;
  ENetAddress m_local{};
  ENetAddress m_server{};
  std::string m_token;
};
}  // namespace Online::Ipv6
