// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/OnlineSession.h"

#include <chrono>
#include <map>
#include <mutex>

#include <fmt/format.h>

#include <enet/enet.h>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Core/Config/OnlineSettings.h"

namespace Online
{
std::string Endpoint::ToString() const
{
  if (IsIpv6())
    return fmt::format("[{}]:{}", ip, port);
  return fmt::format("{}:{}", ip, port);
}

void P2PLink::HostDeleter::operator()(_ENetHost* host) const
{
  enet_host_destroy(host);
}

P2PLink::P2PLink(HostPtr host, std::vector<ENetPeer*> peers, HostPtr host6,
                 std::vector<ENetPeer*> peers6)
    : m_host(std::move(host)), m_peers(std::move(peers)), m_host6(std::move(host6)),
      m_peers6(std::move(peers6))
{
}

P2PLink::~P2PLink()
{
  Release(0);
}

P2PLink::P2PLink(P2PLink&& other) noexcept
    : m_host(std::move(other.m_host)), m_peers(std::move(other.m_peers)),
      m_host6(std::move(other.m_host6)), m_peers6(std::move(other.m_peers6))
{
  other.m_peers.clear();
  other.m_peers6.clear();
}

P2PLink& P2PLink::operator=(P2PLink&& other) noexcept
{
  if (this != &other)
  {
    Release(0);
    m_host = std::move(other.m_host);
    m_peers = std::move(other.m_peers);
    m_host6 = std::move(other.m_host6);
    m_peers6 = std::move(other.m_peers6);
    other.m_peers.clear();
    other.m_peers6.clear();
  }
  return *this;
}

P2PLink::HostPtr P2PLink::TakeHost()
{
  m_peers.clear();
  // A backend that keeps the IPv4 host has no use for the IPv6 one.
  HostPtr host = std::move(m_host);
  Release(0);
  return host;
}

void P2PLink::Release(u32 linger_ms)
{
  // Both hosts (IPv4, and IPv6 if the connect had one) at once, so neither waits for the other.
  ENetHost* hosts[2] = {m_host.get(), m_host6.get()};
  // Every connected peer of each host, not only m_peers: both sides connect to each other at
  // once (Slippi), so there may be a second, superfluous connection to the same player.
  for (ENetHost* host : hosts)
  {
    for (size_t i = 0; host && i < host->peerCount; ++i)
    {
      ENetPeer* peer = &host->peers[i];
      if (peer->state != ENET_PEER_STATE_DISCONNECTED && peer->state != ENET_PEER_STATE_ZOMBIE)
        enet_peer_disconnect(peer, 0);
    }
  }
  // Wait for the disconnects to be acknowledged. Both sides release at about the same time and
  // then bind the same ports again; a disconnect still being retransmitted from here would reach
  // the peer's next socket on that port, and ENet accepts it into a connection that is still
  // being set up (its session id is not checked yet), which kills that connection.
  auto all_disconnected = [&hosts] {
    for (ENetHost* host : hosts)
    {
      for (size_t i = 0; host && i < host->peerCount; ++i)
      {
        if (host->peers[i].state != ENET_PEER_STATE_DISCONNECTED)
          return false;
      }
    }
    return true;
  };
  const bool both = hosts[0] && hosts[1];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(linger_ms);
  do
  {
    for (ENetHost* host : hosts)
    {
      if (!host)
        continue;
      ENetEvent event;
      const int r = enet_host_service(host, &event, linger_ms ? (both ? 5 : 10) : 0);
      if (r > 0 && event.type == ENET_EVENT_TYPE_RECEIVE)
        enet_packet_destroy(event.packet);
    }
  } while (!all_disconnected() && std::chrono::steady_clock::now() < deadline);
  for (ENetHost* host : hosts)
  {
    if (host)
      enet_host_flush(host);
  }
  m_host.reset();
  m_peers.clear();
  m_host6.reset();
  m_peers6.clear();
}

namespace Session
{
namespace
{
std::mutex s_mutex;
std::unique_ptr<SessionBackend> s_backend;

std::mutex s_factory_mutex;
std::map<std::string, Factory> s_factories;
}  // namespace

void RegisterFactory(const std::string& name, Factory factory)
{
  std::lock_guard lk(s_factory_mutex);
  s_factories[name] = std::move(factory);
}

std::vector<std::string> FactoryNames()
{
  std::lock_guard lk(s_factory_mutex);
  std::vector<std::string> names;
  for (const auto& [name, factory] : s_factories)
    names.push_back(name);
  return names;
}

std::optional<std::string> Select(const std::string& name)
{
  if (name == "none")
  {
    SetBackend(nullptr);
    return std::nullopt;
  }
  Factory factory;
  {
    std::lock_guard lk(s_factory_mutex);
    if (const auto it = s_factories.find(name); it != s_factories.end())
      factory = it->second;
  }
  if (!factory)
  {
    std::string names;
    for (const auto& n : FactoryNames())
      names += n + ", ";
    return "no online session backend '" + name + "' (have: " + names + "none)";
  }
  SetBackend(factory());
  NOTICE_LOG_FMT(NETPLAY, "Online: session backend '{}'", name);
  return std::nullopt;
}

void SelectConfigured()
{
  const std::string name = Config::Get(Config::ONLINE_SESSION_BACKEND);
  if (const auto error = Select(name))
    ERROR_LOG_FMT(NETPLAY, "Online: [Online] SessionBackend: {}", *error);
}

void SetBackend(std::unique_ptr<SessionBackend> backend)
{
  std::unique_ptr<SessionBackend> old;
  {
    std::lock_guard lk(s_mutex);
    old = std::move(s_backend);
    s_backend = std::move(backend);
  }
  if (old)
    old->Stop();
}

bool HasBackend()
{
  std::lock_guard lk(s_mutex);
  return s_backend != nullptr;
}

std::optional<std::string> Start(const Match& match, P2PLink link,
                                 const picojson::object& selections)
{
  std::lock_guard lk(s_mutex);
  if (!s_backend)
    return std::string("no online session backend is registered");
  NOTICE_LOG_FMT(NETPLAY, "Online: starting a {} session for match {} ({} on port {})",
                 s_backend->Name(), match.match_id, match.is_host ? "host" : "guest",
                 match.local_port);
  return s_backend->Start(match, std::move(link), selections);
}

void Stop()
{
  std::lock_guard lk(s_mutex);
  if (s_backend)
    s_backend->Stop();
}

std::string BackendName()
{
  std::lock_guard lk(s_mutex);
  return s_backend ? std::string(s_backend->Name()) : std::string();
}

SessionStatus Status()
{
  std::lock_guard lk(s_mutex);
  if (!s_backend)
    return SessionStatus{};
  SessionStatus status = s_backend->Status();
  status.backend = s_backend->Name();
  return status;
}
}  // namespace Session
}  // namespace Online
