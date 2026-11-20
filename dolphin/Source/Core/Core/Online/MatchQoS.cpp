// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/MatchQoS.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#include <Qos2.h>
#pragma comment(lib, "qwave")
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "Common/CommonFuncs.h"
#include "Common/Logging/Log.h"

namespace Online::MatchQoS
{
static bool Enabled()
{
  static const bool on = [] {
    const char* e = std::getenv("PPR_MATCH_QOS");
    return !(e && e[0] == '0');
  }();
  return on;
}

#ifndef _WIN32
static bool Apply(NativeSocket socket, bool ipv6, bool voice)
{
#if defined(__APPLE__) && defined(SO_NET_SERVICE_TYPE)
  const int service = voice ? NET_SERVICE_TYPE_VO : NET_SERVICE_TYPE_BE;
  setsockopt(socket, SOL_SOCKET, SO_NET_SERVICE_TYPE, &service, sizeof(service));
#endif
  const int tos = voice ? VOICE_TOS : 0;
  return setsockopt(socket, ipv6 ? IPPROTO_IPV6 : IPPROTO_IP, ipv6 ? IPV6_TCLASS : IP_TOS, &tos,
                    sizeof(tos)) == 0;
}

Marking::Marking(NativeSocket socket, bool ipv6) : m_socket(socket), m_ipv6(ipv6)
{
  if (!Enabled())
    return;
  m_on = Apply(socket, ipv6, true);
  if (!m_on)
    WARN_LOG_FMT(BRAWLBACK, "match QoS: socket not marked ({})", Common::LastStrerrorString());
}

Marking::~Marking() = default;

void Marking::SetBulk(bool bulk)
{
  if (!m_on || bulk == m_bulk)
    return;
  m_bulk = bulk;
  Apply(m_socket, m_ipv6, !bulk);
}

StreamingMode::StreamingMode() = default;
StreamingMode::~StreamingMode() = default;
#else
Marking::Marking(NativeSocket socket, bool ipv6) : m_socket(socket), m_ipv6(ipv6)
{
  QOS_VERSION version{1, 0};
  m_on = Enabled() && QOSCreateHandle(&version, &m_qos);
  if (Enabled() && !m_on)
    WARN_LOG_FMT(BRAWLBACK, "match QoS: no qWAVE handle ({})", Common::GetLastErrorString());
}

Marking::~Marking()
{
  if (m_on)
    QOSCloseHandle(m_qos);  // ends its flows too
}

void Marking::Track(u32 ipv4, u16 port)
{
  const u8 ip[4] = {static_cast<u8>(ipv4 >> 24), static_cast<u8>(ipv4 >> 16),
                    static_cast<u8>(ipv4 >> 8), static_cast<u8>(ipv4)};
  Add(ip, port);
}

void Marking::Track(const u8* ipv6, u16 port)
{
  Add(ipv6, port);
}

void Marking::Add(const u8* ip, u16 port)
{
  if (!m_on)
    return;
  const size_t len = m_ipv6 ? 16 : 4;
  std::lock_guard lk(m_mutex);
  for (const Flow& f : m_flows)
  {
    if (f.port == port && std::memcmp(f.ip.data(), ip, len) == 0)
      return;
  }
  if (m_flows.size() >= 64)
    return;
  Flow f{{}, port, 0};
  std::memcpy(f.ip.data(), ip, len);
  sockaddr_storage to{};
  if (m_ipv6)
  {
    auto* a = reinterpret_cast<sockaddr_in6*>(&to);
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(port);
    std::memcpy(&a->sin6_addr, ip, 16);
  }
  else
  {
    auto* a = reinterpret_cast<sockaddr_in*>(&to);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    std::memcpy(&a->sin_addr, ip, 4);
  }
  QOS_FLOWID id = 0;
  if (QOSAddSocketToFlow(m_qos, static_cast<SOCKET>(m_socket), reinterpret_cast<PSOCKADDR>(&to),
                         m_bulk ? QOSTrafficTypeBestEffort : QOSTrafficTypeVoice,
                         QOS_NON_ADAPTIVE_FLOW, &id))
  {
    f.id = id;
  }
  else
  {
    WARN_LOG_FMT(BRAWLBACK, "match QoS: no voice flow to port {} ({})", port,
                 Common::GetLastErrorString());
  }
  m_flows.push_back(f);
}

void Marking::SetBulk(bool bulk)
{
  if (!m_on || bulk == m_bulk)
    return;
  std::lock_guard lk(m_mutex);
  m_bulk = bulk;
  QOS_TRAFFIC_TYPE type = bulk ? QOSTrafficTypeBestEffort : QOSTrafficTypeVoice;
  for (const Flow& f : m_flows)
  {
    if (f.id != 0)
      QOSSetFlow(m_qos, f.id, QOSSetTrafficType, sizeof(type), &type, 0, nullptr);
  }
}

// SetSocketMediaStreamingMode (Windows 8+, Windows.Networking.dll), looked up at run time.
static void SetStreamingMode(BOOL on)
{
  using Fn = HRESULT(WINAPI*)(BOOL);
  static const Fn fn = [] {
    HMODULE dll = LoadLibraryExW(L"Windows.Networking.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return dll ? reinterpret_cast<Fn>(
                     reinterpret_cast<void*>(GetProcAddress(dll, "SetSocketMediaStreamingMode"))) :
                 nullptr;
  }();
  const HRESULT hr = fn ? fn(on) : E_NOTIMPL;
  if (on && FAILED(hr))
    WARN_LOG_FMT(BRAWLBACK, "match QoS: no media streaming mode ({:#x})", static_cast<u32>(hr));
}

StreamingMode::StreamingMode()
{
  if (Enabled())
    SetStreamingMode(TRUE);
}

StreamingMode::~StreamingMode()
{
  if (Enabled())
    SetStreamingMode(FALSE);
}
#endif
}  // namespace Online::MatchQoS
