// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/UdpSocket6.h"

#include <cstring>

#include <fmt/format.h>

#include "Common/ENet.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Gprb
{
namespace
{
// sf::IpAddress holds the address as a host-order integer; ENet's handle is in network order.
sf::IpAddress FromHandle(enet_uint32 host)
{
  const auto* b = reinterpret_cast<const u8*>(&host);
  return sf::IpAddress(b[0], b[1], b[2], b[3]);
}

ENetAddress ToEnet(const sf::IpAddress& ip, u16 port)
{
  ENetAddress a{};
  const u32 v = ip.toInteger();
  const u8 b[4] = {static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8),
                   static_cast<u8>(v)};
  std::memcpy(&a.host, b, 4);
  a.port = port;
  return a;
}
}  // namespace

bool IsIpv6Handle(const sf::IpAddress& ip)
{
  return Common::ENet::IsIpv6(ToEnet(ip, 0));
}

std::string AddressText(const sf::IpAddress& ip)
{
  return IsIpv6Handle(ip) ? Common::ENet::HostText(ToEnet(ip, 0)) : ip.toString();
}

std::string EndpointText(const sf::IpAddress& ip, u16 port)
{
  return IsIpv6Handle(ip) ? fmt::format("[{}]:{}", AddressText(ip), port) :
                            fmt::format("{}:{}", ip.toString(), port);
}

std::optional<sf::IpAddress> ResolvePeerAddress(const std::string& text)
{
  if (text.find(':') == std::string::npos)
    return sf::IpAddress::resolve(text);
#ifdef ENET_BRAWL_IPV6
  ENetAddress a{};
  if (enet_address_set_host_ipv6(&a, text.c_str()) == 0 && a.host != ENET_HOST_ANY)
    return FromHandle(a.host);
#endif
  return std::nullopt;
}

UdpSocket6::UdpSocket6() : sf::Socket(sf::Socket::Type::Udp)
{
}

bool UdpSocket6::Bind(const std::string& ip6, u16 port)
{
#ifdef ENET_BRAWL_IPV6
  if (m_port != 0)
    return false;  // bound already
  sockaddr_in6 sin6{};
  sin6.sin6_family = AF_INET6;
  sin6.sin6_port = htons(port);
  if (inet_pton(AF_INET6, ip6.c_str(), &sin6.sin6_addr) != 1)
    return false;
#ifdef _WIN32
  const SOCKET fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == INVALID_SOCKET)
    return false;
#else
  const int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0)
    return false;
#endif
  const int on = 1;
  const bool ok =
      setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&on), sizeof(on)) ==
          0 &&
      bind(fd, reinterpret_cast<const sockaddr*>(&sin6), sizeof(sin6)) == 0;
  if (!ok)
  {
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
    return false;
  }
  // sf::Socket owns it from here (and closes it).
  create(static_cast<sf::SocketHandle>(fd));
  setBlocking(false);
  m_port = port;
  return true;
#else
  (void)ip6;
  (void)port;
  return false;
#endif
}

sf::Socket::Status UdpSocket6::Send(const void* data, std::size_t size, const sf::IpAddress& to,
                                    u16 port)
{
#ifdef ENET_BRAWL_IPV6
  sockaddr_in6 sin6{};
  sin6.sin6_family = AF_INET6;
  sin6.sin6_port = htons(port);
  if (m_port == 0 ||
      enet_ipv6_handle_bytes(ToEnet(to, 0).host, reinterpret_cast<enet_uint8*>(&sin6.sin6_addr)) != 0)
  {
    return Status::Error;
  }
  const auto n = sendto(getNativeHandle(), static_cast<const char*>(data), static_cast<int>(size),
                        0, reinterpret_cast<const sockaddr*>(&sin6), sizeof(sin6));
  if (n < 0)
  {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK ? Status::NotReady : Status::Error;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN ? Status::NotReady : Status::Error;
#endif
  }
  return Status::Done;
#else
  (void)data;
  (void)size;
  (void)to;
  (void)port;
  return Status::Error;
#endif
}

sf::Socket::Status UdpSocket6::Receive(void* data, std::size_t size, std::size_t& received,
                                       std::optional<sf::IpAddress>& from, unsigned short& port)
{
  received = 0;
  from.reset();
  port = 0;
#ifdef ENET_BRAWL_IPV6
  if (m_port == 0)
    return Status::Error;
  sockaddr_in6 sin6{};
#ifdef _WIN32
  int len = sizeof(sin6);
#else
  socklen_t len = sizeof(sin6);
#endif
  const auto n = recvfrom(getNativeHandle(), static_cast<char*>(data), static_cast<int>(size), 0,
                          reinterpret_cast<sockaddr*>(&sin6), &len);
  if (n < 0)
  {
#ifdef _WIN32
    const int e = WSAGetLastError();
    // An ICMP error for an earlier send (the peer's port closed): nothing to read.
    return e == WSAEWOULDBLOCK || e == WSAECONNRESET ? Status::NotReady : Status::Error;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == ECONNREFUSED ? Status::NotReady :
                                                                              Status::Error;
#endif
  }
  if (sin6.sin6_family != AF_INET6)
    return Status::NotReady;
  const enet_uint32 h = enet_ipv6_handle_of(reinterpret_cast<const enet_uint8*>(&sin6.sin6_addr));
  if (h == 0)
    return Status::NotReady;  // the table of addresses is full: a sender we cannot name
  received = static_cast<std::size_t>(n);
  from = FromHandle(h);
  port = ntohs(sin6.sin6_port);
  return Status::Done;
#else
  (void)data;
  (void)size;
  return Status::Error;
#endif
}
}  // namespace Gprb
