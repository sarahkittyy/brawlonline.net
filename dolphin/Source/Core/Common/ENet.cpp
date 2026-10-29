// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/ENet.h"

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
