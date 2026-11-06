// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"

// NAT-PMP (RFC 6886), the port mapping protocol of routers that do not answer UPnP or have it
// off: miniupnpd's other half (OpenWrt, pfSense, OPNsense), Apple's routers, some ISP gateways.
// Online play's port mapping (Common/UPnP.cpp) falls back to it.
namespace Common::NatPmp
{
// This machine's default IPv4 gateway (the router), as in_addr::s_addr (network byte order).
std::optional<u32> DefaultGateway();

std::string AddressToString(u32 addr);

struct Answer
{
  bool answered = false;     // a well-formed answer came
  u16 result = 0;            // RFC 6886's result code: 0 success, 2 refused, 3 network failure ...
  u32 external_address = 0;  // ExternalAddress's, as in_addr::s_addr
  u16 external_port = 0;     // MapUdp's: the router's port for it (0 removed)
  u32 lifetime = 0;          // MapUdp's, in seconds
  std::string error;         // why nothing came, if nothing did
};

// Sends each request up to `tries` times, the first answer wait 250 ms, then twice as long
// (RFC 6886 3.1); a router that refuses the datagram ends it at once on systems that report that.
Answer ExternalAddress(u32 gateway, int tries);
// Asks the router to forward UDP `external_port` (a wish: the router may give another) to this
// machine's `internal_port` for `lifetime` seconds. Lifetime 0 with external port 0 removes it.
Answer MapUdp(u32 gateway, u16 internal_port, u16 external_port, u32 lifetime, int tries);

const char* ResultText(u16 result);
}  // namespace Common::NatPmp
