// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/SDIO/SDCardStorage.h"

#include <mutex>
#include <utility>

namespace IOS::HLE
{
SDCardFileStorage::SDCardFileStorage(const std::string& path)
{
  // Shared: the launcher's playback Dolphin (replays, spectating) boots the same card as a Play
  // Dolphin that may be running, and the launcher reads it back before each launch. On Windows the
  // default (_tfopen_s) let no other process open the file while a game ran, so spectating while
  // in a game or a queue started nothing (bug report #17, 2026-10-15). Writes stay gated by
  // [Core] WiiSDCardAllowWrites, which the playback Dolphin turns off.
  m_file.Open(path, "r+b", File::SharedAccess::ReadWrite);
}

bool SDCardFileStorage::IsOpen() const
{
  return m_file.IsOpen();
}

u64 SDCardFileStorage::GetSize() const
{
  return m_file.GetSize();
}

bool SDCardFileStorage::Read(u64 offset, u8* dst, u64 size)
{
  return m_file.Seek(static_cast<s64>(offset), File::SeekOrigin::Begin) &&
         m_file.ReadBytes(dst, size);
}

bool SDCardFileStorage::Write(u64 offset, const u8* src, u64 size)
{
  return m_file.Seek(static_cast<s64>(offset), File::SeekOrigin::Begin) &&
         m_file.WriteBytes(src, size);
}

static std::mutex s_session_storage_mutex;
static std::shared_ptr<SDCardStorage> s_session_storage;

void SetSDCardSessionStorage(std::shared_ptr<SDCardStorage> storage)
{
  std::lock_guard lock(s_session_storage_mutex);
  s_session_storage = std::move(storage);
}

std::shared_ptr<SDCardStorage> GetSDCardSessionStorage()
{
  std::lock_guard lock(s_session_storage_mutex);
  return s_session_storage;
}
}  // namespace IOS::HLE
