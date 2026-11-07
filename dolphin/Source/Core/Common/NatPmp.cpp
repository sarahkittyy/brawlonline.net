// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/NatPmp.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
// After winsock2.h.
#include <iphlpapi.h>

#include "Common/SocketContext.h"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <net/route.h>
#include <sys/sysctl.h>
#endif
#endif

namespace Common::NatPmp
{
namespace
{
constexpr u16 SERVER_PORT = 5351;
constexpr int FIRST_WAIT_MS = 250;
#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;
#endif

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket NO_SOCKET = INVALID_SOCKET;
void CloseSocket(Socket s)
{
  closesocket(s);
}
int LastError()
{
  return WSAGetLastError();
}
bool IsRefused(int error)
{
  // An ICMP port unreachable from the router, reported on the next receive.
  return error == WSAECONNRESET || error == WSAECONNREFUSED;
}
#else
using Socket = int;
constexpr Socket NO_SOCKET = -1;
void CloseSocket(Socket s)
{
  close(s);
}
int LastError()
{
  return errno;
}
bool IsRefused(int error)
{
  return error == ECONNREFUSED;
}
#endif

u16 Get16(const u8* p)
{
  return static_cast<u16>((p[0] << 8) | p[1]);
}

u32 Get32(const u8* p)
{
  return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}

void Put16(u8* p, u16 v)
{
  p[0] = static_cast<u8>(v >> 8);
  p[1] = static_cast<u8>(v);
}

void Put32(u8* p, u32 v)
{
  Put16(p, static_cast<u16>(v >> 16));
  Put16(p + 2, static_cast<u16>(v));
}

// Sends `request` to the gateway's NAT-PMP port and waits for an answer to its opcode of at least
// `answer_size` bytes, which it leaves in `answer`.
bool Exchange(u32 gateway, const u8* request, size_t request_size, u8* answer, size_t answer_size,
              int tries, std::string* error)
{
#ifdef _WIN32
  const SocketContext context;
#endif
  const Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == NO_SOCKET)
  {
    *error = "no socket (error " + std::to_string(LastError()) + ")";
    return false;
  }
#ifdef SO_NOSIGPIPE
  // macOS can fail the send with EPIPE (bug report #253), which would also kill the process with
  // SIGPIPE.
  const int no_sigpipe = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(SERVER_PORT);
  to.sin_addr.s_addr = gateway;
  // Connected: only the router's port 5351 is heard, and a refusal comes back as an error.
  if (connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != 0)
  {
    *error = "connect failed (error " + std::to_string(LastError()) + ")";
    CloseSocket(s);
    return false;
  }

  bool got = false;
  int wait_ms = FIRST_WAIT_MS;
  *error = "no answer";
  for (int t = 0; t < tries && !got; ++t, wait_ms *= 2)
  {
    if (send(s, reinterpret_cast<const char*>(request), static_cast<int>(request_size),
             SEND_FLAGS) < 0)
    {
      *error = "send failed (error " + std::to_string(LastError()) + ")";
      break;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (!got)
    {
      const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0)
        break;
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(s, &fds);
      timeval tv{};
      tv.tv_sec = static_cast<long>(left.count() / 1000000);
      tv.tv_usec = static_cast<long>(left.count() % 1000000);
      const int r = select(static_cast<int>(s) + 1, &fds, nullptr, nullptr, &tv);
      if (r <= 0)
        break;
      std::array<u8, 64> buf{};
      const int n = static_cast<int>(
          recv(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0));
      if (n < 0)
      {
        const int e = LastError();
        if (IsRefused(e))
        {
          *error = "the router refused it (nothing listens at its port 5351)";
          tries = 0;
          break;
        }
        continue;
      }
      // Version 0, the request's opcode + 128 (RFC 6886 3.5); others are ignored.
      if (static_cast<size_t>(n) < 4 || buf[0] != 0 || buf[1] != (request[1] | 0x80))
        continue;
      // An error answer may be short (only the header); a success has the full size.
      if (Get16(&buf[2]) == 0 && static_cast<size_t>(n) < answer_size)
        continue;
      std::memcpy(answer, buf.data(), std::min(static_cast<size_t>(n), answer_size));
      got = true;
    }
  }
  CloseSocket(s);
  return got;
}
}  // namespace

std::string AddressToString(u32 addr)
{
  const u8* b = reinterpret_cast<const u8*>(&addr);
  return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
         std::to_string(b[3]);
}

std::optional<u32> DefaultGateway()
{
#if defined(_WIN32)
  // The route toward a public address (the one the internet traffic takes).
  MIB_IPFORWARDROW row{};
  if (GetBestRoute(htonl(0x08080808), 0, &row) != NO_ERROR || row.dwForwardNextHop == 0)
    return std::nullopt;
  return static_cast<u32>(row.dwForwardNextHop);
#elif defined(__APPLE__)
  int mib[] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY};
  size_t size = 0;
  if (sysctl(mib, 6, nullptr, &size, nullptr, 0) != 0 || size == 0)
    return std::nullopt;
  std::vector<char> buf(size);
  if (sysctl(mib, 6, buf.data(), &size, nullptr, 0) != 0)
    return std::nullopt;
  // The addresses after each header are padded to 4 bytes on Darwin (netstat's ROUNDUP).
  const auto padded = [](size_t len) { return len > 0 ? ((len + 3) & ~size_t{3}) : 4; };
  for (size_t off = 0; off + sizeof(rt_msghdr) <= size;)
  {
    rt_msghdr hdr;
    std::memcpy(&hdr, buf.data() + off, sizeof(hdr));
    if (hdr.rtm_msglen == 0)
      break;
    const size_t end = std::min(off + hdr.rtm_msglen, size);
    size_t at = off + sizeof(rt_msghdr);
    std::optional<sockaddr_in> dst, gw;
    for (int i = 0; i < RTAX_MAX && at < end; ++i)
    {
      if (!(hdr.rtm_addrs & (1 << i)))
        continue;
      const u8 len = static_cast<u8>(buf[at]);
      if ((i == RTAX_DST || i == RTAX_GATEWAY) && at + sizeof(sockaddr_in) <= end)
      {
        sockaddr_in sa;
        std::memcpy(&sa, buf.data() + at, sizeof(sa));
        if (sa.sin_family == AF_INET)
          (i == RTAX_DST ? dst : gw) = sa;
      }
      at += padded(len);
    }
    if (dst && gw && dst->sin_addr.s_addr == 0 && gw->sin_addr.s_addr != 0)
      return static_cast<u32>(gw->sin_addr.s_addr);
    off += hdr.rtm_msglen;
  }
  return std::nullopt;
#elif defined(__linux__)
  // Iface Destination Gateway Flags RefCnt Use Metric Mask ..., the addresses as s_addr in hex.
  FILE* f = std::fopen("/proc/net/route", "r");
  if (!f)
    return std::nullopt;
  std::optional<u32> best;
  unsigned long best_metric = 0;
  char line[512];
  bool header = true;
  while (std::fgets(line, sizeof(line), f))
  {
    if (header)
    {
      header = false;
      continue;
    }
    char iface[64];
    unsigned long dest = 0, gateway = 0, flags = 0, refcnt = 0, use = 0, metric = 0, mask = 0;
    if (std::sscanf(line, "%63s %lx %lx %lx %lu %lu %lu %lx", iface, &dest, &gateway, &flags,
                    &refcnt, &use, &metric, &mask) != 8)
    {
      continue;
    }
    constexpr unsigned long ROUTE_UP = 0x1, ROUTE_GATEWAY = 0x2;  // RTF_UP, RTF_GATEWAY
    constexpr unsigned long WANTED = ROUTE_UP | ROUTE_GATEWAY;
    if (dest != 0 || mask != 0 || gateway == 0 || (flags & WANTED) != WANTED)
    {
      continue;
    }
    if (!best || metric < best_metric)
    {
      best = static_cast<u32>(gateway);
      best_metric = metric;
    }
  }
  std::fclose(f);
  return best;
#else
  return std::nullopt;
#endif
}

Answer ExternalAddress(u32 gateway, int tries)
{
  const std::array<u8, 2> request{0, 0};
  std::array<u8, 12> answer{};
  Answer a;
  if (!Exchange(gateway, request.data(), request.size(), answer.data(), answer.size(), tries,
                &a.error))
  {
    return a;
  }
  a.answered = true;
  a.result = Get16(&answer[2]);
  if (a.result == 0)
    std::memcpy(&a.external_address, &answer[8], 4);
  return a;
}

Answer MapUdp(u32 gateway, u16 internal_port, u16 external_port, u32 lifetime, int tries)
{
  std::array<u8, 12> request{};
  request[0] = 0;  // version
  request[1] = 1;  // map UDP
  Put16(&request[4], internal_port);
  Put16(&request[6], external_port);
  Put32(&request[8], lifetime);
  std::array<u8, 16> answer{};
  Answer a;
  if (!Exchange(gateway, request.data(), request.size(), answer.data(), answer.size(), tries,
                &a.error))
  {
    return a;
  }
  a.answered = true;
  a.result = Get16(&answer[2]);
  if (a.result == 0)
  {
    if (Get16(&answer[8]) != internal_port)
    {
      // An answer to another request (another program's); treated as none.
      a.answered = false;
      a.error = "an answer for another port";
      return a;
    }
    a.external_port = Get16(&answer[10]);
    a.lifetime = Get32(&answer[12]);
  }
  return a;
}

const char* ResultText(u16 result)
{
  switch (result)
  {
  case 0:
    return "success";
  case 1:
    return "unsupported version";
  case 2:
    return "not authorized (mapping turned off on the router)";
  case 3:
    return "network failure (the router has no internet address)";
  case 4:
    return "out of resources";
  case 5:
    return "unsupported opcode";
  default:
    return "unknown result";
  }
}
}  // namespace Common::NatPmp
