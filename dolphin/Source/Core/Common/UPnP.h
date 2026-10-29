// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef USE_UPNP

#include <chrono>
#include <string>

#include "Common/CommonTypes.h"

namespace Common::UPnP
{
void TryPortmapping(u16 port);
void StopPortmapping();

// Online play's mapping (Core/Online/Matchmaking.cpp), separate from the netplay host's above.
// Looking for the router's UPnP service takes up to about 2 s, so it starts in the background
// (once per process; later calls do nothing).
void StartDiscovery();
// Asks the router to forward UDP `port` on its internet side to `port` on this machine, waiting
// up to `wait` for the discovery. Replaces the mapping this made before. Logs what happened;
// returns whether the router took the mapping.
bool MapUdpPort(u16 port, std::chrono::milliseconds wait);
// Removes the mapping MapUdpPort made, if any (with `only_port`, only if it is that port's: an
// Other Games room's session leaves a later search's mapping alone).
void UnmapUdpPort(u16 only_port = 0);
// The router's internet address as it reported it over UPnP, once the discovery is done ("" before
// that, or with no router found).
std::string ExternalAddress();
}  // namespace Common::UPnP

#endif
