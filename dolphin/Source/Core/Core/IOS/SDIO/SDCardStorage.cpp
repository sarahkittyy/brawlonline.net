// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/SDIO/SDCardStorage.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <utility>
#include <vector>

#include "Common/WorkQueueThread.h"

namespace IOS::HLE
{
// Reads the card image ahead of the game, on a thread of its own, so that what the game reads next
// is already in the host's file cache when it asks. The game's reads run on the CPU thread (IOS),
// and one the cache doesn't hold waits there for the disk: 8 ms for one 16 KiB read was measured,
// half a frame. P+ streams the stage's music from the card all match long, 16 KiB at a time. Only
// the host's cache changes: what the game reads is the same.
class SDCardReadAhead
{
public:
  explicit SDCardReadAhead(std::string path) : m_path(std::move(path)) {}
  // Leaves what is still queued: waits for the piece being read at most.
  ~SDCardReadAhead() { m_worker.Stop(); }

  // The game read [offset, offset + size) (the CPU thread). Never waits for the disk.
  void OnRead(u64 offset, u64 size);

private:
  // A read continues a run when it starts within this distance of where the run's last read
  // ended (P+'s music reads follow each other exactly).
  static constexpr u64 GAP = 64 * 1024;
  // How far ahead of a run to read: about 25 s of P+'s music.
  static constexpr u64 WINDOW = 1024 * 1024;
  // What the thread reads at once. Each piece is read ahead once.
  static constexpr u64 PIECE = 64 * 1024;

  // A run of reads, each starting about where the one before it ended (a streamed file).
  struct Run
  {
    u64 next = 0;      // where the run's next read is expected
    u64 ahead_to = 0;  // read ahead up to here
    u64 last_used = 0;
  };

  void ReadPiece(u64 piece);

  std::string m_path;
  std::array<Run, 8> m_runs{};
  u64 m_reads = 0;
  std::vector<bool> m_queued;  // by piece

  // The thread's. Its own handle: reading through it fills the same host cache as the game's
  // reads, without moving the game's file position.
  File::IOFile m_file;
  std::array<u8, PIECE> m_buffer;
  Common::WorkQueueThreadSP<u64> m_worker;
};

void SDCardReadAhead::OnRead(u64 offset, u64 size)
{
  const u64 end = offset + size;
  ++m_reads;

  auto run = std::ranges::find_if(m_runs, [&](const Run& r) {
    return r.last_used != 0 && offset + GAP >= r.next && offset <= r.next + GAP;
  });
  if (run == m_runs.end())
  {
    // A read on its own (the file system's, or a file's first): it may start a run, in place of
    // the run read least recently.
    *std::ranges::min_element(m_runs, {}, &Run::last_used) = {end, end, m_reads};
    return;
  }

  run->next = end;
  run->last_used = m_reads;
  // Read ahead again once the game is halfway into what was read ahead last time.
  if (run->ahead_to >= end + WINDOW / 2)
    return;
  const u64 begin = std::max(run->ahead_to, end);
  run->ahead_to = end + WINDOW;
  for (u64 piece = begin / PIECE; piece * PIECE < run->ahead_to; ++piece)
  {
    if (piece >= m_queued.size())
      m_queued.resize(piece + 1);
    if (m_queued[piece])
      continue;
    m_queued[piece] = true;
    if (!m_worker.IsRunning())
      m_worker.Reset("SD read-ahead", [this](u64 p) { ReadPiece(p); });
    m_worker.Push(piece);
  }
}

void SDCardReadAhead::ReadPiece(u64 piece)
{
  if (!m_file.IsOpen())
    m_file.Open(m_path, "rb", File::SharedAccess::ReadWrite);
  // A piece at the image's end reads short, and what it holds is in the cache all the same.
  if (!m_file.Seek(static_cast<s64>(piece * PIECE), File::SeekOrigin::Begin) ||
      !m_file.ReadBytes(m_buffer.data(), m_buffer.size()))
  {
    m_file.ClearError();
  }
}

SDCardFileStorage::SDCardFileStorage(const std::string& path, bool read_only)
{
  // Shared: the launcher's playback Dolphin (replays, spectating) boots the same card as a Play
  // Dolphin that may be running, and the launcher reads it back before each launch. On Windows the
  // default (_tfopen_s) let no other process open the file while a game ran, so spectating while
  // in a game or a queue started nothing (bug report #17, 2026-10-15). Writes stay gated by
  // [Core] WiiSDCardAllowWrites, which the playback Dolphin turns off.
  m_file.Open(path, read_only ? "rb" : "r+b", File::SharedAccess::ReadWrite);
  if (m_file.IsOpen())
    m_read_ahead = std::make_unique<SDCardReadAhead>(path);
}

SDCardFileStorage::~SDCardFileStorage() = default;

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
  if (m_read_ahead)
    m_read_ahead->OnRead(offset, size);
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
