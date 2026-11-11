// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ENet's IPv6 (Externals/enet/ipv6): handles in ENetAddress::host, IPv6 hosts on ::1, and IPv4
// sockets exactly as before.

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "Common/ENet.h"

#ifdef ENET_BRAWL_IPV6

namespace
{
struct ENetInit
{
  ENetInit() { enet_initialize(); }
  ~ENetInit() { enet_deinitialize(); }
};

ENetAddress V6(const char* text, u16 port = 0)
{
  ENetAddress a{};
  EXPECT_EQ(enet_address_set_host_ipv6(&a, text), 0) << text;
  a.port = port;
  return a;
}

// Services both hosts until each has a connect event (or the time is up).
bool ConnectPair(ENetHost* a, ENetHost* b, const ENetAddress& b_addr, ENetPeer** a_peer,
                 ENetPeer** b_peer)
{
  *a_peer = enet_host_connect(a, &b_addr, 1, 7);
  if (!*a_peer)
    return false;
  bool a_up = false, b_up = false;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!(a_up && b_up) && std::chrono::steady_clock::now() < until)
  {
    ENetEvent ev;
    while (enet_host_service(a, &ev, 1) > 0)
      a_up |= ev.type == ENET_EVENT_TYPE_CONNECT;
    while (enet_host_service(b, &ev, 1) > 0)
    {
      if (ev.type == ENET_EVENT_TYPE_CONNECT)
      {
        b_up = true;
        *b_peer = ev.peer;
      }
    }
  }
  return a_up && b_up;
}

ENetHost* V6Host(u16 port = 0)
{
  ENetAddress local = V6("::1", port);
  return enet_host_create_ipv6(&local, 4, 1, 0, 0);
}
}  // namespace

TEST(ENetIpv6, HandlesNameAddresses)
{
  ENetAddress a = V6("2001:db8::1", 41000);
  EXPECT_TRUE(enet_address_is_ipv6(&a));
  EXPECT_TRUE(Common::ENet::IsIpv6(a));
  EXPECT_EQ(reinterpret_cast<const u8*>(&a.host)[0], 240);
  EXPECT_EQ(Common::ENet::HostText(a), "2001:db8::1");
  EXPECT_EQ(Common::ENet::EndpointText(a), "[2001:db8::1]:41000");

  // The same address in another spelling is the same handle; another address another one.
  ENetAddress same = V6("2001:0db8:0000::0001");
  EXPECT_EQ(same.host, a.host);
  ENetAddress other = V6("2001:db8::2");
  EXPECT_NE(other.host, a.host);
  u8 bytes[16]{};
  ASSERT_EQ(enet_ipv6_handle_bytes(other.host, bytes), 0);
  EXPECT_EQ(bytes[0], 0x20);
  EXPECT_EQ(bytes[15], 0x02);
  EXPECT_EQ(enet_ipv6_handle_of(bytes), other.host);

  // Not IPv6: refused, and the address is left alone.
  ENetAddress v4{};
  v4.host = 0x12345678;
  EXPECT_NE(enet_address_set_host_ipv6(&v4, "192.0.2.1"), 0);
  EXPECT_NE(enet_address_set_host_ipv6(&v4, "nonsense:"), 0);
  EXPECT_NE(enet_address_set_host_ipv6(&v4, ""), 0);
  EXPECT_EQ(v4.host, 0x12345678u);
  // The unspecified address is "any".
  ENetAddress any = V6("::");
  EXPECT_EQ(any.host, static_cast<enet_uint32>(ENET_HOST_ANY));
  // A 240.x handle that was never made has no address.
  ENetAddress bogus{};
  bogus.host = 0xFFFFFFF0;  // bytes 240.255.255.255
  EXPECT_NE(enet_ipv6_handle_bytes(bogus.host, bytes), 0);
}

TEST(ENetIpv6, Ipv4IsUnchanged)
{
  ENetInit init;
  // IPv4 text through the usual functions: upstream's result, byte for byte.
  ENetAddress a{};
  ASSERT_EQ(enet_address_set_host_ip(&a, "192.0.2.7"), 0);
  const u8 expect[4] = {192, 0, 2, 7};
  EXPECT_EQ(std::memcmp(&a.host, expect, 4), 0);
  EXPECT_FALSE(Common::ENet::IsIpv6(a));
  a.port = 41234;
  EXPECT_EQ(Common::ENet::EndpointText(a), "192.0.2.7:41234");
  char text[64]{};
  ASSERT_EQ(enet_address_get_host_ip(&a, text, sizeof(text)), 0);
  EXPECT_STREQ(text, "192.0.2.7");
  // enet_address_set_host with IPv6 text gives a handle.
  ENetAddress h{};
  ASSERT_EQ(enet_address_set_host(&h, "::1"), 0);
  EXPECT_TRUE(Common::ENet::IsIpv6(h));

  // Two IPv4 hosts on 127.0.0.1 connect as ever, and their sockets are not ours.
  ENetAddress local{};
  local.host = ENET_HOST_ANY;
  local.port = 0;
  ENetHost* x = enet_host_create(&local, 4, 1, 0, 0);
  ENetHost* y = enet_host_create(&local, 4, 1, 0, 0);
  ASSERT_TRUE(x && y);
  EXPECT_FALSE(enet_socket_is_ipv6(x->socket));
  EXPECT_FALSE(Common::ENet::IsIpv6(x->address));
  ENetAddress to{};
  ASSERT_EQ(enet_address_set_host_ip(&to, "127.0.0.1"), 0);
  to.port = y->address.port;
  ENetPeer *xp = nullptr, *yp = nullptr;
  EXPECT_TRUE(ConnectPair(x, y, to, &xp, &yp));
  ASSERT_NE(yp, nullptr);
  EXPECT_FALSE(Common::ENet::IsIpv6(yp->address));
  EXPECT_EQ(Common::ENet::HostText(yp->address), "127.0.0.1");
  EXPECT_EQ(yp->eventData, 7u);
  enet_host_destroy(x);
  enet_host_destroy(y);
}

TEST(ENetIpv6, HostsConnectOverIpv6)
{
  ENetInit init;
  ENetHost* x = V6Host();
  if (!x)
    GTEST_SKIP() << "no IPv6 on this machine (::1)";
  ENetHost* y = V6Host();
  ASSERT_NE(y, nullptr);
  EXPECT_TRUE(enet_socket_is_ipv6(x->socket));
  EXPECT_NE(y->address.port, 0);
  EXPECT_EQ(Common::ENet::HostText(y->address), "::1");

  ENetPeer *xp = nullptr, *yp = nullptr;
  ASSERT_TRUE(ConnectPair(x, y, V6("::1", y->address.port), &xp, &yp));
  // The peer is named by its handle, which reads back as ::1 and x's port.
  EXPECT_TRUE(Common::ENet::IsIpv6(yp->address));
  EXPECT_EQ(Common::ENet::EndpointText(yp->address),
            "[::1]:" + std::to_string(x->address.port));
  EXPECT_EQ(yp->eventData, 7u);

  // A packet each way.
  const char msg[] = "over six";
  enet_peer_send(xp, 0, enet_packet_create(msg, sizeof(msg), ENET_PACKET_FLAG_RELIABLE));
  bool got = false;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!got && std::chrono::steady_clock::now() < until)
  {
    ENetEvent ev;
    enet_host_service(x, &ev, 1);
    while (enet_host_service(y, &ev, 1) > 0)
    {
      if (ev.type == ENET_EVENT_TYPE_RECEIVE)
      {
        got = ev.packet->dataLength == sizeof(msg) && std::memcmp(ev.packet->data, msg, sizeof(msg)) == 0;
        enet_packet_destroy(ev.packet);
      }
    }
  }
  EXPECT_TRUE(got);

  // Destroyed, a socket is no longer taken for ours.
  const ENetSocket xs = x->socket;
  enet_host_destroy(x);
  EXPECT_FALSE(enet_socket_is_ipv6(xs));
  enet_host_destroy(y);
}

TEST(ENetIpv6, FamiliesStayApart)
{
  ENetInit init;
  ENetHost* six = V6Host();
  if (!six)
    GTEST_SKIP() << "no IPv6 on this machine (::1)";
  ENetAddress local{};
  local.host = ENET_HOST_ANY;
  local.port = six->address.port;  // the same port number on IPv4: a separate socket (V6ONLY)
  ENetHost* four = enet_host_create(&local, 4, 1, 0, 0);
  ASSERT_NE(four, nullptr) << "IPv4 could not bind the IPv6 socket's port";

  // From the IPv6 host to an IPv4 address: nothing leaves, and nothing connects.
  ENetAddress to4{};
  ASSERT_EQ(enet_address_set_host_ip(&to4, "127.0.0.1"), 0);
  to4.port = four->address.port;
  ENetPeer* p = enet_host_connect(six, &to4, 1, 0);
  ASSERT_NE(p, nullptr);
  bool connected = false;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
  while (std::chrono::steady_clock::now() < until)
  {
    ENetEvent ev;
    while (enet_host_service(six, &ev, 1) > 0)
      connected |= ev.type == ENET_EVENT_TYPE_CONNECT;
    while (enet_host_service(four, &ev, 1) > 0)
      connected |= ev.type == ENET_EVENT_TYPE_CONNECT;
  }
  EXPECT_FALSE(connected);
  EXPECT_EQ(four->totalReceivedPackets, 0u);
  enet_host_destroy(six);
  enet_host_destroy(four);
}

#endif  // ENET_BRAWL_IPV6
