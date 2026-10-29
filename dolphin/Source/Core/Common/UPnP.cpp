// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef USE_UPNP

#include "Common/UPnP.h"

#include "Common/Logging/Log.h"

#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <miniupnpc.h>
#include <miniwget.h>
#include <mutex>
#include <string>
#include <thread>
#include <upnpcommands.h>
#include <upnperrors.h>

static UPNPUrls s_urls;
static IGDdatas s_data;
static std::array<char, 20> s_our_ip;
static u16 s_mapped = 0;
static std::thread s_thread;

// called from ---UPnP--- thread
// discovers the IGD
static bool InitUPnP()
{
  static bool s_inited = false;
  static bool s_error = false;

  // Don't init if already inited
  if (s_inited)
    return true;

  // Don't init if it failed before
  if (s_error)
    return false;

  s_urls = {};
  s_data = {};

  // Find all UPnP devices
  int upnperror = 0;
  std::unique_ptr<UPNPDev, decltype(&freeUPNPDevlist)> devlist(nullptr, freeUPNPDevlist);
#if MINIUPNPC_API_VERSION >= 14
  devlist.reset(upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &upnperror));
#else
  devlist.reset(upnpDiscover(2000, nullptr, nullptr, 0, 0, &upnperror));
#endif
  if (!devlist)
  {
    if (upnperror == UPNPDISCOVER_SUCCESS)
    {
      WARN_LOG_FMT(NETPLAY, "No UPnP devices could be found.");
    }
    else
    {
      WARN_LOG_FMT(NETPLAY, "An error occurred trying to discover UPnP devices: {}",
                   strupnperror(upnperror));
    }

    s_error = true;

    return false;
  }

  // Look for the IGD
  bool found_valid_igd = false;
  for (UPNPDev* dev = devlist.get(); dev; dev = dev->pNext)
  {
    if (!std::strstr(dev->st, "InternetGatewayDevice"))
      continue;

    int desc_xml_size = 0;
    std::unique_ptr<char, decltype(&std::free)> desc_xml(nullptr, std::free);
    int statusCode = 200;
#if MINIUPNPC_API_VERSION >= 16
    desc_xml.reset(
        static_cast<char*>(miniwget_getaddr(dev->descURL, &desc_xml_size, s_our_ip.data(),
                                            static_cast<int>(s_our_ip.size()), 0, &statusCode)));
#else
    desc_xml.reset(static_cast<char*>(miniwget_getaddr(
        dev->descURL, &desc_xml_size, s_our_ip.data(), static_cast<int>(s_our_ip.size()), 0)));
#endif
    if (desc_xml && statusCode == 200)
    {
      parserootdesc(desc_xml.get(), desc_xml_size, &s_data);
      GetUPNPUrls(&s_urls, &s_data, dev->descURL, 0);

      found_valid_igd = true;
      NOTICE_LOG_FMT(NETPLAY, "Got info from IGD at {}.", dev->descURL);
      break;
    }
    else
    {
      WARN_LOG_FMT(NETPLAY, "Error getting info from IGD at {}.", dev->descURL);
    }
  }

  if (!found_valid_igd)
    WARN_LOG_FMT(NETPLAY, "Could not find a valid IGD in the discovered UPnP devices.");

  s_inited = true;

  return true;
}

// called from ---UPnP--- thread
// Attempt to stop portforwarding.
// --
// NOTE: It is important that this happens! A few very crappy routers
// apparently do not delete UPnP mappings on their own, so if you leave them
// hanging, the NVRAM will fill with portmappings, and eventually all UPnP
// requests will fail silently, with the only recourse being a factory reset.
// --
static bool UnmapPort(const u16 port)
{
  std::string port_str = std::to_string(port);
  UPNP_DeletePortMapping(s_urls.controlURL, s_data.first.servicetype, port_str.c_str(), "UDP",
                         nullptr);

  return true;
}

// called from ---UPnP--- thread
// Attempt to portforward!
static bool MapPort(const char* addr, const u16 port)
{
  if (s_mapped > 0)
    UnmapPort(s_mapped);

  std::string port_str = std::to_string(port);
  int result = UPNP_AddPortMapping(
      s_urls.controlURL, s_data.first.servicetype, port_str.c_str(), port_str.c_str(), addr,
      (std::string("dolphin-emu UDP on ") + addr).c_str(), "UDP", nullptr, nullptr);

  if (result != 0)
    return false;

  s_mapped = port;

  return true;
}

// UPnP thread: try to map a port
static void MapPortThread(const u16 port)
{
  if (InitUPnP() && MapPort(s_our_ip.data(), port))
  {
    NOTICE_LOG_FMT(NETPLAY, "Successfully mapped port {} to {}.", port, s_our_ip.data());
    return;
  }

  WARN_LOG_FMT(NETPLAY, "Failed to map port {} to {}.", port, s_our_ip.data());
}

// UPnP thread: try to unmap a port
static void UnmapPortThread()
{
  if (s_mapped > 0)
    UnmapPort(s_mapped);
}

void Common::UPnP::TryPortmapping(u16 port)
{
  if (s_thread.joinable())
    s_thread.join();
  s_thread = std::thread(&MapPortThread, port);
}

void Common::UPnP::StopPortmapping()
{
  if (s_thread.joinable())
    s_thread.join();
  s_thread = std::thread(&UnmapPortThread);
  s_thread.join();
}

// Online play's mapping. A router that changes the source port of every new destination
// (endpoint-dependent mapping, "symmetric NAT") breaks the hole punch against players whose
// router only lets in replies from the exact address and port it sent to: the peers send to the
// port the matchmaking server saw, which the router only keeps open for the server. A UPnP
// mapping of the port makes the router forward it, and with it most routers also keep the port
// for what the machine sends from it (miniupnpd adds a MASQUERADE --to-ports rule), so the
// server sees the port the peers can reach.
namespace
{
struct OnlineState
{
  std::mutex mutex;  // guards everything below
  std::condition_variable discovered;
  bool started = false;
  bool done = false;
  bool have_igd = false;
  std::string external;  // the router's internet address, as it reported it ("" if none)
  UPNPUrls urls{};
  IGDdatas data{};
  std::string lan_addr;  // this machine's address toward the router
  u16 mapped = 0;
};

// Never destroyed: the discovery thread is detached and may still run when the process exits.
OnlineState& GetOnlineState()
{
  static auto* const state = new OnlineState;
  return *state;
}

// strupnperror() is null for codes it does not know.
const char* ErrorText(int code)
{
  const char* text = strupnperror(code);
  return text ? text : "unknown error";
}

bool IsPrivateIPv4(const char* addr)
{
  unsigned a = 0, b = 0;
  if (std::sscanf(addr, "%u.%u", &a, &b) != 2)
    return false;
  return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
         (a == 100 && b >= 64 && b <= 127);
}

void DiscoverOnlineIGD()
{
  int error = 0;
  std::unique_ptr<UPNPDev, decltype(&freeUPNPDevlist)> devlist(nullptr, freeUPNPDevlist);
#if MINIUPNPC_API_VERSION >= 14
  devlist.reset(upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error));
#else
  devlist.reset(upnpDiscover(2000, nullptr, nullptr, 0, 0, &error));
#endif

  UPNPUrls urls{};
  IGDdatas data{};
  std::array<char, 64> lan_addr{};
  int result = 0;
  if (devlist)
  {
#if MINIUPNPC_API_VERSION >= 18
    std::array<char, 64> wan_addr{};
    result = UPNP_GetValidIGD(devlist.get(), &urls, &data, lan_addr.data(),
                              static_cast<int>(lan_addr.size()), wan_addr.data(),
                              static_cast<int>(wan_addr.size()));
    // 1 connected, 2 connected with a private internet address, 3 not connected (some routers
    // say so wrongly), 4 not an IGD.
    const bool igd = result >= 1 && result <= 3;
#else
    result = UPNP_GetValidIGD(devlist.get(), &urls, &data, lan_addr.data(),
                              static_cast<int>(lan_addr.size()));
    // 1 connected, 2 not connected (some routers say so wrongly), 3 not an IGD.
    const bool igd = result >= 1 && result <= 2;
#endif
    if (!igd && result != 0)
      FreeUPNPUrls(&urls);
    if (!igd)
      result = 0;
  }

  std::array<char, 64> external{};
  if (result != 0 &&
      UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, external.data()) != 0)
  {
    external[0] = '\0';
  }

  if (result == 0)
  {
    if (!devlist && error != UPNPDISCOVER_SUCCESS)
      NOTICE_LOG_FMT(NETPLAY, "UPnP: discovery failed: {}", ErrorText(error));
    else
      NOTICE_LOG_FMT(NETPLAY, "UPnP: no router with UPnP found");
  }
  else if (IsPrivateIPv4(external.data()))
  {
    // Another NAT above the router (the ISP's, a building's): its mapping does not reach the
    // internet, but costs nothing.
    WARN_LOG_FMT(NETPLAY,
                 "UPnP: router at {} for {}; its internet address {} is private (another NAT "
                 "above it)",
                 urls.controlURL, lan_addr.data(), external.data());
  }
  else
  {
    NOTICE_LOG_FMT(NETPLAY, "UPnP: router at {} for {}, internet address {}", urls.controlURL,
                   lan_addr.data(), external.data());
  }

  OnlineState& state = GetOnlineState();
  std::lock_guard lk(state.mutex);
  state.have_igd = result != 0;
  state.urls = urls;
  state.data = data;
  state.lan_addr = lan_addr.data();
  state.external = result != 0 ? external.data() : "";
  state.done = true;
  state.discovered.notify_all();
}

void StartDiscoveryLocked(OnlineState& state)
{
  if (state.started)
    return;
  state.started = true;
  std::thread(&DiscoverOnlineIGD).detach();
}

void UnmapLocked(OnlineState& state)
{
  if (state.mapped == 0)
    return;
  const std::string port = std::to_string(state.mapped);
  const int r = UPNP_DeletePortMapping(state.urls.controlURL, state.data.first.servicetype,
                                       port.c_str(), "UDP", nullptr);
  if (r == UPNPCOMMAND_SUCCESS)
    INFO_LOG_FMT(NETPLAY, "UPnP: removed the mapping of UDP port {}", state.mapped);
  else
    WARN_LOG_FMT(NETPLAY, "UPnP: removing the mapping of UDP port {} failed: {}", state.mapped,
                 ErrorText(r));
  state.mapped = 0;
}
}  // namespace

void Common::UPnP::StartDiscovery()
{
  OnlineState& state = GetOnlineState();
  std::lock_guard lk(state.mutex);
  StartDiscoveryLocked(state);
}

bool Common::UPnP::MapUdpPort(u16 port, std::chrono::milliseconds wait)
{
  OnlineState& state = GetOnlineState();
  std::unique_lock lk(state.mutex);
  StartDiscoveryLocked(state);
  if (!state.discovered.wait_for(lk, wait, [&] { return state.done; }))
  {
    NOTICE_LOG_FMT(NETPLAY, "UPnP: still looking for the router; UDP port {} is not mapped", port);
    return false;
  }
  if (!state.have_igd)
    return false;

  UnmapLocked(state);
  const std::string port_str = std::to_string(port);
  const std::string desc = "Brawl Online UDP " + port_str;
  // A lease, so that a mapping left behind by a crash goes away. Some routers only take
  // permanent ones (0).
  int r = UPNP_AddPortMapping(state.urls.controlURL, state.data.first.servicetype,
                              port_str.c_str(), port_str.c_str(), state.lan_addr.c_str(),
                              desc.c_str(), "UDP", nullptr, "7200");
  if (r == 725)
  {
    r = UPNP_AddPortMapping(state.urls.controlURL, state.data.first.servicetype, port_str.c_str(),
                            port_str.c_str(), state.lan_addr.c_str(), desc.c_str(), "UDP",
                            nullptr, "0");
  }
  if (r != UPNPCOMMAND_SUCCESS)
  {
    WARN_LOG_FMT(NETPLAY, "UPnP: the router refused to map UDP port {} to {}: {} ({})", port,
                 state.lan_addr, ErrorText(r), r);
    return false;
  }
  state.mapped = port;
  NOTICE_LOG_FMT(NETPLAY, "UPnP: mapped UDP port {} to {}", port, state.lan_addr);
  return true;
}

std::string Common::UPnP::ExternalAddress()
{
  OnlineState& state = GetOnlineState();
  std::lock_guard lk(state.mutex);
  return state.external;
}

void Common::UPnP::UnmapUdpPort(u16 only_port)
{
  OnlineState& state = GetOnlineState();
  std::lock_guard lk(state.mutex);
  if (only_port != 0 && state.mapped != only_port)
    return;
  UnmapLocked(state);
}

#endif
