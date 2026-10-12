// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Orca rollback: an undo journal of NAND changes. A rollback snapshot can't afford to copy the NAND
// (Brawl's /tmp alone is about 37 MB), and most frames change none of it, so each snapshot records
// a journal mark and every change after it is journaled with what it destroyed. Loading the
// snapshot undoes those changes newest first, which leaves the NAND exactly as it was at the mark.

#include "Core/IOS/FS/HostBackend/FS.h"

#include <algorithm>
#include <cstdio>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

namespace IOS::HLE::FS
{
namespace
{
// Appends a directory's contents, parents before children. False if a file could not be read.
bool CollectTree(const File::FSTEntry& entry, std::vector<std::string>* paths,
                 std::vector<std::string>* contents)
{
  bool ok = true;
  for (const File::FSTEntry& child : entry.children)
  {
    if (child.isDirectory)
    {
      paths->push_back(child.physicalName + '/');
      contents->emplace_back();
      ok &= CollectTree(child, paths, contents);
    }
    else
    {
      std::string content;
      if (!File::ReadFileToString(child.physicalName, content))
      {
        ERROR_LOG_FMT(IOS_FS, "NAND journal: could not read {}", child.physicalName);
        ok = false;
      }
      paths->push_back(child.physicalName);
      contents->push_back(std::move(content));
    }
  }
  return ok;
}

void RemoveHostPath(const std::string& host_path)
{
  if (File::IsDirectory(host_path))
    File::DeleteDirRecursively(host_path);
  else if (File::Exists(host_path))
    File::Delete(host_path);
}
}  // namespace

u64 HostFileSystem::JournalMark()
{
  m_journal_active = true;
  m_journal_has_fst = false;
  m_journal_mark = m_journal_next_seq;
  return m_journal_mark;
}

void HostFileSystem::JournalStop()
{
  m_journal_active = false;
  m_journal_has_fst = false;
  m_journal.clear();
}

void HostFileSystem::JournalTrim(u64 mark)
{
  while (!m_journal.empty() && m_journal.front().seq < mark)
    m_journal.pop_front();
}

void HostFileSystem::JournalUndo(u64 mark)
{
  while (!m_journal.empty() && m_journal.back().seq >= mark)
  {
    UndoJournalEntry(m_journal.back());
    m_journal.pop_back();
  }
  // Changes from here on are relative to the restored FST. Numbering restarts at the mark (every
  // snapshot with a later mark was taken after the load point and is discarded), so a re-run
  // records the same marks as the first run.
  m_journal_has_fst = false;
  m_journal_next_seq = mark;
}

HostFileSystem::JournalCheckpoint HostFileSystem::JournalBeginTransaction(bool preserve_fst) const
{
  JournalCheckpoint checkpoint{m_journal_next_seq, m_journal_has_fst};
  if (m_journal_active && preserve_fst)
  {
    checkpoint.root_fst = m_root_entry;
    checkpoint.redirect_fst = m_redirect_fst;
  }
  return checkpoint;
}

void HostFileSystem::JournalAbortTransaction(const JournalCheckpoint& checkpoint)
{
  if (!m_journal_active)
    return;

  // A failed host mutation can leave buffered stdio state behind. Close every host handle before
  // applying the undo entries, just like a normal rollback load does, then reopen the restored
  // guest handles. This also prevents a late flush from overwriting the repaired file.
  CloseHostFiles();
  while (!m_journal.empty() && m_journal.back().seq >= checkpoint.next_seq)
  {
    UndoJournalEntry(m_journal.back());
    m_journal.pop_back();
  }
  m_journal_next_seq = checkpoint.next_seq;
  m_journal_has_fst = checkpoint.had_fst;
  if (checkpoint.root_fst)
  {
    m_root_entry = *checkpoint.root_fst;
    m_redirect_fst = *checkpoint.redirect_fst;
    if (!SaveFst())
      m_journal_error = true;
  }
  ReopenHostFiles();
}

void HostFileSystem::UndoJournalEntry(const JournalEntry& entry)
{
  switch (entry.kind)
  {
  case JournalEntry::Kind::Fst:
    m_root_entry = entry.root_fst;
    m_redirect_fst = entry.redirect_fst;
    if (!SaveFst())
      m_journal_error = true;
    break;

  case JournalEntry::Kind::Created:
    RemoveHostPath(entry.host_path);
    break;

  case JournalEntry::Kind::Removed:
    RemoveHostPath(entry.host_path);
    for (std::size_t i = 0; i < entry.removed_paths.size(); ++i)
    {
      const std::string& path = entry.removed_paths[i];
      const bool ok = path.ends_with('/') ? File::CreateDir(path.substr(0, path.size() - 1)) :
                                            File::WriteStringToFile(path, entry.removed_contents[i]);
      if (!ok)
        {
        ERROR_LOG_FMT(IOS_FS, "NAND journal: could not restore {}", path);
        m_journal_error = true;
      }
    }
    break;

  case JournalEntry::Kind::Written:
  {
    File::IOFile file(entry.host_path, "r+b");
    bool ok = file.IsOpen();
    if (ok && !entry.old_bytes.empty())
    {
      ok = file.Seek(entry.offset, File::SeekOrigin::Begin) &&
           file.WriteBytes(entry.old_bytes.data(), entry.old_bytes.size());
    }
    ok = ok && file.Flush();
    if (ok && file.GetSize() != entry.old_size)
      ok = file.Resize(entry.old_size);
    if (!ok)
    {
      ERROR_LOG_FMT(IOS_FS, "NAND journal: could not undo a write to {}", entry.host_path);
      m_journal_error = true;
    }
    break;
  }

  case JournalEntry::Kind::Renamed:
    if (!File::Rename(entry.new_host_path, entry.host_path) &&
        !(File::CopyRegularFile(entry.new_host_path, entry.host_path) &&
          File::Delete(entry.new_host_path)))
    {
      ERROR_LOG_FMT(IOS_FS, "NAND journal: could not rename {} back to {}", entry.new_host_path,
                    entry.host_path);
      m_journal_error = true;
    }
    break;
  }
}

void HostFileSystem::JournalFst()
{
  if (!m_journal_active || m_journal_has_fst)
    return;
  m_journal_has_fst = true;
  JournalEntry& entry = m_journal.emplace_back();
  entry.kind = JournalEntry::Kind::Fst;
  entry.seq = m_journal_next_seq++;
  entry.root_fst = m_root_entry;
  entry.redirect_fst = m_redirect_fst;
}

void HostFileSystem::JournalCreated(const std::string& host_path)
{
  if (!m_journal_active)
    return;
  JournalEntry& entry = m_journal.emplace_back();
  entry.kind = JournalEntry::Kind::Created;
  entry.seq = m_journal_next_seq++;
  entry.host_path = host_path;
}

bool HostFileSystem::JournalRemoved(const std::string& host_path)
{
  if (!m_journal_active)
    return true;

  // Build the entry off-journal. A partial backup must never authorize the destructive operation.
  JournalEntry entry;
  entry.kind = JournalEntry::Kind::Removed;
  entry.seq = m_journal_next_seq;
  entry.host_path = host_path;
  if (File::IsDirectory(host_path))
  {
    entry.removed_paths.push_back(host_path + '/');
    entry.removed_contents.emplace_back();
    if (!CollectTree(File::ScanDirectoryTree(host_path, true), &entry.removed_paths,
                     &entry.removed_contents))
    {
      m_journal_error = true;
      return false;
    }
  }
  else
  {
    std::string content;
    if (!File::ReadFileToString(host_path, content))
    {
      ERROR_LOG_FMT(IOS_FS, "NAND journal: could not read {}", host_path);
      m_journal_error = true;
      return false;
    }
    entry.removed_paths.push_back(host_path);
    entry.removed_contents.push_back(std::move(content));
  }

  m_journal.push_back(std::move(entry));
  ++m_journal_next_seq;
  return true;
}

bool HostFileSystem::JournalWritten(const std::string& host_path, File::IOFile& file, u64 offset,
                                    u64 count)
{
  if (!m_journal_active)
    return true;

  // Do not publish an undo entry until all bytes needed to reverse the write are available.
  JournalEntry entry;
  entry.kind = JournalEntry::Kind::Written;
  entry.seq = m_journal_next_seq;
  entry.host_path = host_path;
  entry.offset = offset;
  entry.old_size = file.GetSize();
  if (offset < entry.old_size)
  {
    // Raw stdio, like ReadBytesFromFile: a failure must not mark the shared IOFile bad, which would
    // fail every later write through it.
    entry.old_bytes.resize(std::min(count, entry.old_size - offset));
    std::FILE* const handle = file.GetHandle();
    if (std::fseek(handle, static_cast<long>(offset), SEEK_SET) != 0 ||
        std::fread(entry.old_bytes.data(), 1, entry.old_bytes.size(), handle) !=
            entry.old_bytes.size())
    {
      std::clearerr(handle);
      ERROR_LOG_FMT(IOS_FS, "NAND journal: could not read {} before a write", host_path);
      m_journal_error = true;
      return false;
    }
  }

  m_journal.push_back(std::move(entry));
  ++m_journal_next_seq;
  return true;
}

void HostFileSystem::JournalRenamed(const std::string& old_host_path,
                                    const std::string& new_host_path)
{
  if (!m_journal_active)
    return;
  JournalEntry& entry = m_journal.emplace_back();
  entry.kind = JournalEntry::Kind::Renamed;
  entry.seq = m_journal_next_seq++;
  entry.host_path = old_host_path;
  entry.new_host_path = new_host_path;
}
}  // namespace IOS::HLE::FS
