// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// What the emulated SD card reads and writes. Usually the SD card image file itself. A
// whole-machine session (Other Games rooms) hands the slot its own storage instead
// (Machine::SDCard::SessionCard): the session's writes land in a throwaway layer, a joiner plays
// the session's card, and the layer rolls back with the rest of the machine.

#pragma once

#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

namespace IOS::HLE
{
class SDCardStorage
{
public:
  virtual ~SDCardStorage() = default;

  virtual bool IsOpen() const = 0;
  virtual u64 GetSize() const = 0;
  virtual bool Read(u64 offset, u8* dst, u64 size) = 0;
  virtual bool Write(u64 offset, const u8* src, u64 size) = 0;

  // Rollback: a counter of the writes so far, kept in savestates; loading a state puts the
  // card's contents back to what they were at that count.
  virtual u64 GetWriteSeq() const { return 0; }
  virtual void RewindTo(u64 write_seq) {}
};

// The SD card image file, read and written in place (Dolphin outside a session).
class SDCardFileStorage final : public SDCardStorage
{
public:
  explicit SDCardFileStorage(const std::string& path);

  bool IsOpen() const override;
  u64 GetSize() const override;
  bool Read(u64 offset, u8* dst, u64 size) override;
  bool Write(u64 offset, const u8* src, u64 size) override;

private:
  File::IOFile m_file;
};

// The storage of the emulated session about to boot, or nullptr for the image file. Set by
// Core's emulation thread before the hardware starts and cleared after it shut down.
void SetSDCardSessionStorage(std::shared_ptr<SDCardStorage> storage);
std::shared_ptr<SDCardStorage> GetSDCardSessionStorage();
}  // namespace IOS::HLE
