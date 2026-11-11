// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/ENet.h"

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace Common::ENet
{
void WakeupThread(ENetHost* host)
{
  // Send ourselves a spurious message.  This is hackier than it should be.
  // comex reported this as https://github.com/lsalzman/enet/issues/23, so
  // hopefully there will be a better way to do it in the future.
  ENetAddress address;
  if (host->address.port != 0)
    address.port = host->address.port;
  else
    enet_socket_get_address(host->socket, &address);
  address.host = 0x0100007f;  // localhost
#ifdef ENET_BRAWL_IPV6
  if (enet_socket_is_ipv6(host->socket))
    enet_address_set_host_ipv6(&address, "::1");
#endif
  u8 byte = 0;
  ENetBuffer buf;
  buf.data = &byte;
  buf.dataLength = 1;
  enet_socket_send(host->socket, &address, &buf, 1);
}

int ENET_CALLBACK InterceptCallback(ENetHost* host, ENetEvent* event)
{
  // wakeup packet received
  if (host->receivedDataLength == 1 && host->receivedData[0] == 0)
  {
    event->type = static_cast<ENetEventType>(SKIPPABLE_EVENT);
    return 1;
  }
  return 0;
}

void PunchLowPorts(ENetHost* host, const ENetAddress& to)
{
  constexpr u16 PUNCH_PORT_FIRST = 1024;
  constexpr u16 PUNCH_PORT_COUNT = 8;
  u8 byte = 0;
  ENetBuffer buffer{};
  buffer.data = &byte;
  buffer.dataLength = 1;
  for (u16 i = 0; i < PUNCH_PORT_COUNT; i++)
  {
    ENetAddress a = to;
    a.port = static_cast<enet_uint16>(PUNCH_PORT_FIRST + i);
    enet_socket_send(host->socket, &a, &buffer, 1);
  }
}

bool HasIpv6()
{
#ifdef ENET_BRAWL_IPV6
  return true;
#else
  return false;
#endif
}

bool IsIpv6(const ENetAddress& address)
{
#ifdef ENET_BRAWL_IPV6
  return enet_address_is_ipv6(&address) != 0;
#else
  (void)address;
  return false;
#endif
}

std::string HostText(const ENetAddress& address)
{
  if (IsIpv6(address))
  {
    char text[64]{};
    return enet_address_get_host_ip(&address, text, sizeof(text)) == 0 ? std::string(text) :
                                                                         std::string();
  }
  // ENetAddress::host is in network byte order: the bytes in memory are a.b.c.d.
  const auto* b = reinterpret_cast<const u8*>(&address.host);
  return fmt::format("{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
}

std::string EndpointText(const ENetAddress& address)
{
  if (IsIpv6(address))
    return fmt::format("[{}]:{}", HostText(address), address.port);
  return fmt::format("{}:{}", HostText(address), address.port);
}

bool SendPacket(ENetPeer* socket, const sf::Packet& packet, u8 channel_id, _ENetPacketFlag flag)
{
  if (!socket)
  {
    ERROR_LOG_FMT(NETPLAY, "Target socket is null.");
    return false;
  }

  ENetPacket* epac = enet_packet_create(packet.getData(), packet.getDataSize(), flag);
  if (!epac)
  {
    ERROR_LOG_FMT(NETPLAY, "Failed to create ENetPacket ({} bytes).", packet.getDataSize());
    return false;
  }

  const int result = enet_peer_send(socket, channel_id, epac);
  if (result != 0)
  {
    ERROR_LOG_FMT(NETPLAY, "Failed to send ENetPacket (error code {}).", result);
    return false;
  }

  return true;
}
}  // namespace Common::ENet
