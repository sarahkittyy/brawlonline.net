// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A match's packets marked as voice, so Wi-Fi (WMM) and routers that queue by DSCP send them
// first: busy Wi-Fi channels made most of the waits for the peer in the harness's Wi-Fi model.
// DSCP 46 with IP_TOS / IPV6_TCLASS (and the voice service type on macOS); Windows ignores IP_TOS,
// so qWAVE marks the traffic to each destination there (Track). PPR_MATCH_QOS=0 turns it off.

#pragma once

#include <cstdint>

#ifdef _WIN32
#include <array>
#include <mutex>
#include <vector>
#endif

#include "Common/CommonTypes.h"

namespace Online::MatchQoS
{
#ifdef _WIN32
using NativeSocket = std::uintptr_t;  // SOCKET
#else
using NativeSocket = int;
#endif

constexpr int VOICE_TOS = 46 << 2;  // DSCP 46 (Expedited Forwarding), ECN bits 0

// Marks what a bound socket sends as voice while it exists; destroy it before the socket closes.
class Marking
{
public:
  Marking(NativeSocket socket, bool ipv6);
  ~Marking();
  Marking(const Marking&) = delete;
  Marking& operator=(const Marking&) = delete;

  // Windows: marks the traffic to this address (IPv4 in host order, or IPv6 address bytes).
  void Track(u32 ipv4, u16 port);
  void Track(const u8* ipv6, u16 port);
  // Bulk data is not voice (routers give voice a small share; Wi-Fi does not aggregate it).
  void SetBulk(bool bulk);

private:
  NativeSocket m_socket;
  bool m_ipv6;
  bool m_on = false;
  bool m_bulk = false;
#ifdef _WIN32
  struct Flow
  {
    std::array<u8, 16> ip;
    u16 port;
    u32 id;  // 0: refused
  };
  void Add(const u8* ip, u16 port);
  void* m_qos = nullptr;
  std::mutex m_mutex;
  std::vector<Flow> m_flows;
#endif
};

#ifndef _WIN32
inline void Marking::Track(u32, u16)
{
}
inline void Marking::Track(const u8*, u16)
{
}
#endif

// Windows: Wi-Fi media streaming mode (the driver stops background scans) while it exists.
class StreamingMode
{
public:
  StreamingMode();
  ~StreamingMode();
  StreamingMode(const StreamingMode&) = delete;
  StreamingMode& operator=(const StreamingMode&) = delete;
};
}  // namespace Online::MatchQoS
