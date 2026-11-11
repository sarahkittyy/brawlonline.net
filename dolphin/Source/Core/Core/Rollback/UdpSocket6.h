// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The gameplay session's IPv6 socket (Online/Ipv6.h), beside its IPv4 sf::UdpSocket.
//
// SFML's sockets are IPv4 only. This one is an sf::Socket, so the session's sf::SocketSelector
// waits on it with the IPv4 one, and its addresses are sf::IpAddress values too: an IPv6 address
// is ENet's IPv6 handle for it (Externals/enet/ipv6, 240.x.y.z, an address nothing on the
// internet sends from), so the session compares and stores peers' addresses as it always did.
// The IPv4 socket is not touched.

#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/Socket.hpp>

#include "Common/CommonTypes.h"

namespace Gprb
{
// Whether `ip` is an IPv6 address's handle.
bool IsIpv6Handle(const sf::IpAddress& ip);
// An address in text for logs and the status: "a.b.c.d", or the IPv6 address of a handle.
std::string AddressText(const sf::IpAddress& ip);
// "a.b.c.d:port" or "[ipv6]:port".
std::string EndpointText(const sf::IpAddress& ip, u16 port);
// A peer's address from text: IPv6 ("2001:db8::1") as its handle, else sf::IpAddress::resolve.
std::optional<sf::IpAddress> ResolvePeerAddress(const std::string& text);

class UdpSocket6 : public sf::Socket
{
public:
  UdpSocket6();
  // Binds `ip6` (an IPv6 address in text) and `port`, IPv6 only, non-blocking. False if this
  // machine cannot (the session then plays on over IPv4).
  bool Bind(const std::string& ip6, u16 port);
  // As sf::UdpSocket's; `to` must be a handle (anything else: Error, nothing sent).
  sf::Socket::Status Send(const void* data, std::size_t size, const sf::IpAddress& to, u16 port);
  sf::Socket::Status Receive(void* data, std::size_t size, std::size_t& received,
                             std::optional<sf::IpAddress>& from, unsigned short& port);
  u16 LocalPort() const { return m_port; }

private:
  u16 m_port = 0;
};
}  // namespace Gprb
