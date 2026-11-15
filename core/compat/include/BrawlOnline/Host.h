// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// What a program running the core outside Dolphin provides (docs/design/brawl-online-core.md §10):
// the compat stand-ins for Dolphin's Core/ headers (core/compat/include) answer through it. Only the
// standalone build has it; in Dolphin the real headers answer.

#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace BrawlOnline
{
class Host
{
public:
  virtual ~Host() = default;

  // Guest RAM: a host pointer for [addr, addr + size), effective (0x8/0x9/0xC/0xD...) or physical,
  // or nullptr when the range is not all RAM. The bytes are the guest's, big-endian.
  virtual u8* GuestPointer(u32 addr, u32 size) = 0;
  virtual u8* Mem1() = 0;
  virtual u32 Mem1Size() const = 0;
  virtual u8* Mem2() = 0;
  virtual u32 Mem2Size() const = 0;
  // A host write into guest RAM (CopyToEmu, Write_U8, HostWrite): the snapshots must see it.
  virtual void MarkDirty(u32 addr, u32 size) = 0;

  // A message on the game's screen (OSD::AddMessage): text, how long, ARGB colour.
  virtual void OnScreenMessage(std::string_view text, u32 ms, u32 argb) = 0;
  // The local controller on port `chan` as the game would read it now.
  virtual GCPadStatus LocalPad(int chan) = 0;
  // The game's own thread runs separately from the host's (Dolphin's dual core).
  virtual bool DualCore() const { return false; }
  // Stop the game (Core::Stop): the program closes as when its window is closed.
  virtual void StopGame() = 0;
  // Run on the host's main thread (Core::QueueHostJob).
  virtual void QueueHostJob(std::function<void()> job) = 0;
  // The disc: game ID and revision.
  virtual std::string GameId() const { return "RSBE01"; }
  virtual int Revision() const { return 1; }
  // The width over height the picture is shown at (Presenter::CalculateDrawAspectRatio).
  virtual float DrawAspectRatio() const { return 16.0f / 9.0f; }
};

Host& GetHost();
void SetHost(Host* host);
bool HasHost();

// The host's state changes, as Dolphin's Core::AddOnStateChangedCallback reports them.
void NotifyStopping();
}  // namespace BrawlOnline
