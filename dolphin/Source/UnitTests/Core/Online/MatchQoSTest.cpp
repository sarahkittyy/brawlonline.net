// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A marked socket's datagrams carry DSCP 46 on the wire (Core/Online/MatchQoS.h), read back over
// IPv4 loopback with IP_RECVTOS. Linux only: Windows marks through qWAVE, per destination.

#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "Core/Online/MatchQoS.h"

namespace
{
int Bound(sockaddr_in* addr)
{
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  *addr = {};
  addr->sin_family = AF_INET;
  addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(*addr);
  bind(fd, reinterpret_cast<sockaddr*>(addr), len);
  getsockname(fd, reinterpret_cast<sockaddr*>(addr), &len);
  return fd;
}

// The TOS byte a datagram from `tx` arrives at `rx` with (-1: none).
int ReceivedTos(int tx, int rx, const sockaddr_in& to)
{
  sendto(tx, "G", 1, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
  char buf[8];
  alignas(cmsghdr) char control[64];
  iovec iov{buf, sizeof(buf)};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  if (recvmsg(rx, &msg, 0) < 0)
    return -1;
  for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
  {
    if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TOS)
      return *CMSG_DATA(c);
  }
  return -1;
}
}  // namespace

TEST(MatchQoS, MarksVoiceButNotBulk)
{
  sockaddr_in rx_addr, tx_addr;
  const int rx = Bound(&rx_addr), tx = Bound(&tx_addr);
  const int on = 1;
  setsockopt(rx, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on));
  EXPECT_EQ(ReceivedTos(tx, rx, rx_addr), 0);
  {
    Online::MatchQoS::Marking qos(tx, false);
    EXPECT_EQ(ReceivedTos(tx, rx, rx_addr), Online::MatchQoS::VOICE_TOS);
    qos.SetBulk(true);
    EXPECT_EQ(ReceivedTos(tx, rx, rx_addr), 0);
    qos.SetBulk(false);
    EXPECT_EQ(ReceivedTos(tx, rx, rx_addr), Online::MatchQoS::VOICE_TOS);
  }
  close(tx);
  close(rx);
}
#endif
