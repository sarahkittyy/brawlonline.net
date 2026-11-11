// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/Ipv6.h"

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

#include <fmt/format.h>

#include "Common/ENet.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Core/Config/OnlineSettings.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace Online::Ipv6
{
namespace
{
constexpr char MAGIC[] = "BOP6";
constexpr u8 VERSION = 1;
constexpr u8 REPLY_KIND = 0x81;
constexpr size_t REQUEST_MIN = 48;
constexpr size_t REPLY_LEN = 4 + 1 + 16 + 2;
// A failed lookup is tried again at a search this long after it.
constexpr u64 RETRY_MS = 60 * 1000;

struct Discovery
{
  std::mutex mutex;
  std::condition_variable done_cv;
  std::string host;  // what was (or is being) looked up
  int port = 0;
  bool running = false;
  bool done = false;
  u64 done_ms = 0;
  std::optional<ENetAddress> address;
};
Discovery s_discovery;

bool Reachable(const u8* b, bool loopback)
{
  static const u8 LOOPBACK[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  if (std::memcmp(b, LOOPBACK, 16) == 0)
    return loopback;
  const u16 s0 = static_cast<u16>((b[0] << 8) | b[1]);
  const u16 s1 = static_cast<u16>((b[2] << 8) | b[3]);
  // 2000::/3 without 2001:db8::/32 (documentation), 2001::/32 (Teredo) and 2002::/16 (6to4),
  // whose relays a peer cannot count on. The same rule as mm's (probe6::reachable).
  return (s0 & 0xe000) == 0x2000 && !(s0 == 0x2001 && (s1 == 0x0db8 || s1 == 0)) && s0 != 0x2002;
}

std::optional<ENetAddress> Lookup(const std::string& host, int port)
{
#ifdef ENET_BRAWL_IPV6
  ENetAddress a{};
  if (enet_address_set_host_ipv6(&a, host.c_str()) == 0)
  {
    a.port = static_cast<enet_uint16>(port);
    return a;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* list = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0 || !list)
    return std::nullopt;
  std::optional<ENetAddress> found;
  for (const addrinfo* r = list; r && !found; r = r->ai_next)
  {
    if (r->ai_family != AF_INET6 || !r->ai_addr || r->ai_addrlen < sizeof(sockaddr_in6))
      continue;
    const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(r->ai_addr);
    const enet_uint32 h = enet_ipv6_handle_of(reinterpret_cast<const enet_uint8*>(&sin6->sin6_addr));
    if (h != 0)
    {
      ENetAddress out{};
      out.host = h;
      out.port = static_cast<enet_uint16>(port);
      found = out;
    }
  }
  freeaddrinfo(list);
  return found;
#else
  (void)host;
  (void)port;
  return std::nullopt;
#endif
}

void Resolve(std::string host, int port)
{
  const std::optional<ENetAddress> a = Lookup(host, port);
  if (a)
  {
    INFO_LOG_FMT(NETPLAY, "IPv6: the matchmaking server {} is at {}", host,
                 Common::ENet::EndpointText(*a));
  }
  else
  {
    INFO_LOG_FMT(NETPLAY, "IPv6: the matchmaking server {} has no IPv6 address (IPv4 only)", host);
  }
  std::lock_guard lk(s_discovery.mutex);
  if (s_discovery.host != host || s_discovery.port != port)
    return;  // the settings changed meanwhile: that lookup is under way
  s_discovery.address = a;
  s_discovery.running = false;
  s_discovery.done = true;
  s_discovery.done_ms = Common::Timer::NowMs();
  s_discovery.done_cv.notify_all();
}

// Starts a lookup unless one for these settings is running or done (under the mutex).
void StartLocked(const std::string& host, int port)
{
  const bool same = s_discovery.host == host && s_discovery.port == port;
  if (same && (s_discovery.running ||
               (s_discovery.done && (s_discovery.address ||
                                     Common::Timer::NowMs() - s_discovery.done_ms < RETRY_MS))))
  {
    return;
  }
  s_discovery.host = host;
  s_discovery.port = port;
  s_discovery.running = true;
  s_discovery.done = false;
  s_discovery.address.reset();
  // getaddrinfo can block for seconds when DNS is broken: never on a caller's thread.
  std::thread(Resolve, host, port).detach();
}
}  // namespace

bool Enabled()
{
  return Common::ENet::HasIpv6() && Config::Get(Config::ONLINE_IPV6) &&
         !Config::GetMatchmakingHost6().empty();
}

void StartDiscovery()
{
  if (!Enabled())
    return;
  std::lock_guard lk(s_discovery.mutex);
  StartLocked(Config::GetMatchmakingHost6(), Config::GetMatchmakingPort());
}

std::optional<ENetAddress> Server(std::chrono::milliseconds wait)
{
  if (!Enabled())
    return std::nullopt;
  const std::string host = Config::GetMatchmakingHost6();
  const int port = Config::GetMatchmakingPort();
  std::unique_lock lk(s_discovery.mutex);
  StartLocked(host, port);
  s_discovery.done_cv.wait_for(lk, wait, [] { return s_discovery.done; });
  if (!s_discovery.done || s_discovery.host != host || s_discovery.port != port)
    return std::nullopt;
  return s_discovery.address;
}

std::optional<ENetAddress> LocalAddressToward(const ENetAddress& server, std::string* why)
{
#ifdef ENET_BRAWL_IPV6
  u8 server_bytes[16]{};
  if (enet_ipv6_handle_bytes(server.host, server_bytes) != 0)
  {
    *why = "the server's address is not IPv6";
    return std::nullopt;
  }
  const bool loopback_server = Reachable(server_bytes, true) && !Reachable(server_bytes, false);
  const ENetSocket s = enet_socket_create_ipv6(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL)
  {
    *why = "no IPv6 on this machine";
    return std::nullopt;
  }
  ENetAddress local{};
  // A UDP "connect" sends nothing: the system only picks the route and the source address.
  const bool ok = enet_socket_connect(s, &server) == 0 && enet_socket_get_address(s, &local) == 0;
  enet_socket_destroy(s);
  if (!ok || local.host == ENET_HOST_ANY)
  {
    *why = "no IPv6 route to the server";
    return std::nullopt;
  }
  u8 b[16]{};
  if (enet_ipv6_handle_bytes(local.host, b) != 0 || !Reachable(b, loopback_server))
  {
    *why = fmt::format("this machine's IPv6 address {} is not one the internet can reach",
                       Common::ENet::HostText(local));
    return std::nullopt;
  }
  local.port = 0;
  return local;
#else
  (void)server;
  *why = "this build has no IPv6";
  return std::nullopt;
#endif
}

std::optional<ENetAddress> ParseEndpoint(const std::string& text)
{
#ifdef ENET_BRAWL_IPV6
  if (text.size() < 5 || text.front() != '[')
    return std::nullopt;
  const size_t close = text.find("]:");
  if (close == std::string::npos)
    return std::nullopt;
  const std::string host = text.substr(1, close - 1);
  const std::string port_text = text.substr(close + 2);
  if (port_text.empty() || port_text.size() > 5 ||
      port_text.find_first_not_of("0123456789") != std::string::npos)
  {
    return std::nullopt;
  }
  const int port = std::stoi(port_text);
  ENetAddress a{};
  if (port <= 0 || port > 0xFFFF || host.find('%') != std::string::npos ||
      enet_address_set_host_ipv6(&a, host.c_str()) != 0 || a.host == ENET_HOST_ANY)
  {
    return std::nullopt;
  }
  a.port = static_cast<enet_uint16>(port);
  return a;
#else
  (void)text;
  return std::nullopt;
#endif
}

std::string NewToken()
{
  std::random_device rd;
  std::string t;
  for (int i = 0; i < 4; ++i)
    t += fmt::format("{:08x}", rd());
  return t;
}

std::string ProbeRequest(const std::string& token)
{
  std::string r(MAGIC, 4);
  r += static_cast<char>(VERSION);
  r += static_cast<char>(token.size() & 0xFF);
  r += token;
  if (r.size() < REQUEST_MIN)
    r.resize(REQUEST_MIN, '\0');
  return r;
}

std::optional<ENetAddress> ParseProbeReply(const u8* data, size_t size)
{
#ifdef ENET_BRAWL_IPV6
  if (size != REPLY_LEN || std::memcmp(data, MAGIC, 4) != 0 || data[4] != REPLY_KIND)
    return std::nullopt;
  ENetAddress a{};
  a.host = enet_ipv6_handle_of(data + 5);
  if (a.host == 0)
    return std::nullopt;
  a.port = static_cast<enet_uint16>((data[21] << 8) | data[22]);
  return a;
#else
  (void)data;
  (void)size;
  return std::nullopt;
#endif
}

Probe::~Probe()
{
  Close();
}

bool Probe::Open(const ENetAddress& local, const ENetAddress& server, const std::string& token,
                 std::string* why)
{
  Close();
#ifdef ENET_BRAWL_IPV6
  m_socket = enet_socket_create_ipv6(ENET_SOCKET_TYPE_DATAGRAM);
  if (m_socket == ENET_SOCKET_NULL)
  {
    *why = "no IPv6 socket";
    return false;
  }
  if (enet_socket_bind(m_socket, &local) != 0)
  {
    *why = fmt::format("cannot bind {}", Common::ENet::EndpointText(local));
    Close();
    return false;
  }
  enet_socket_set_option(m_socket, ENET_SOCKOPT_NONBLOCK, 1);
  m_local = local;
  m_server = server;
  m_token = token;
  // A few copies: nothing else resends a lost one, and mm notes the first.
  for (int i = 0; i < 3; ++i)
    Send();
  return true;
#else
  (void)local;
  (void)server;
  (void)token;
  *why = "this build has no IPv6";
  return false;
#endif
}

void Probe::Send()
{
  if (!IsOpen())
    return;
  const std::string req = ProbeRequest(m_token);
  ENetBuffer buf{};
  buf.data = const_cast<char*>(req.data());
  buf.dataLength = req.size();
  enet_socket_send(m_socket, &m_server, &buf, 1);
}

std::optional<ENetAddress> Probe::Reply()
{
  if (!IsOpen())
    return std::nullopt;
  for (int i = 0; i < 16; ++i)
  {
    u8 data[64];
    ENetBuffer buf{};
    buf.data = data;
    buf.dataLength = sizeof(data);
    ENetAddress from{};
    const int n = enet_socket_receive(m_socket, &from, &buf, 1);
    if (n <= 0)
      return std::nullopt;
    if (from.host != m_server.host || from.port != m_server.port)
      continue;
    if (auto a = ParseProbeReply(data, static_cast<size_t>(n)))
      return a;
  }
  return std::nullopt;
}

void Probe::Close()
{
  if (m_socket != ENET_SOCKET_NULL)
    enet_socket_destroy(m_socket);
  m_socket = ENET_SOCKET_NULL;
}
}  // namespace Online::Ipv6
