// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
#pragma once

#include <memory>

#include <SFML/Network/Packet.hpp>
#include <enet/enet.h>

#include "Common/CommonTypes.h"

namespace Common::ENet
{
struct ENetHostDeleter
{
  void operator()(ENetHost* host) const noexcept { enet_host_destroy(host); }
};
using ENetHostPtr = std::unique_ptr<ENetHost, ENetHostDeleter>;

void WakeupThread(ENetHost* host);
int ENET_CALLBACK InterceptCallback(ENetHost* host, ENetEvent* event);
bool SendPacket(ENetPeer* socket, const sf::Packet& packet, u8 channel_id, _ENetPacketFlag flag = ENET_PACKET_FLAG_RELIABLE);

// The ports some carriers' NAT gives each new destination, from the bottom of the range: it
// keeps the player's own port toward mm, but every peer hears them from 1024 (bug reports #115,
// #122, #123: KZH#419 at 166.114.170.169, mm saw 45491, 44999, 49442, ...). A peer whose router
// lets in only the ports it sent to dropped their connect, having sent only to the port mm saw,
// so neither side heard the other. A datagram from `host`'s port to these ports of `to`'s address
// opens our router for theirs; their connect's next resend then gets through. One byte: ENet
// drops anything shorter than its header, on every build (and MachineSession's InterceptAcks
// too). Used by Unranked's connect (Online/Matchmaking.cpp) and Other Games rooms
// (Rollback/MachineSession.cpp), once a second for a peer at its internet address until it
// connects.
void PunchLowPorts(ENetHost* host, const ENetAddress& to);

// used for traversal packets and wake-up packets
constexpr int SKIPPABLE_EVENT = 42;
}  // namespace Common::ENet
