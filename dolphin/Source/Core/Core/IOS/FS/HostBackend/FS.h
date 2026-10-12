// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Core/IOS/FS/FileSystem.h"

namespace IOS::HLE::FS
{
/// Backend that uses the host file system as backend.
///
/// Ignores metadata like permissions, attributes and various checks and also
/// sometimes returns wrong information because metadata is not available.
class HostFileSystem final : public FileSystem
{
public:
  HostFileSystem(std::string root_path, std::vector<NandRedirect> nand_redirects = {});
  ~HostFileSystem() override;

  void DoState(PointerWrap& p) override;

  ResultCode Format(Uid uid) override;

  Result<FileHandle> OpenFile(Uid uid, Gid gid, const std::string& path, Mode mode) override;
  ResultCode Close(Fd fd) override;
  Result<u32> ReadBytesFromFile(Fd fd, u8* ptr, u32 size) override;
  Result<u32> WriteBytesToFile(Fd fd, const u8* ptr, u32 size) override;
  Result<u32> SeekFile(Fd fd, u32 offset, SeekMode mode) override;
  Result<FileStatus> GetFileStatus(Fd fd) override;

  ResultCode CreateFile(Uid caller_uid, Gid caller_gid, const std::string& path,
                        FileAttribute attribute, Modes modes) override;

  ResultCode CreateDirectory(Uid caller_uid, Gid caller_gid, const std::string& path,
                             FileAttribute attribute, Modes modes) override;

  ResultCode Delete(Uid caller_uid, Gid caller_gid, const std::string& path) override;
  ResultCode Rename(Uid caller_uid, Gid caller_gid, const std::string& old_path,
                    const std::string& new_path) override;

  Result<std::vector<std::string>> ReadDirectory(Uid caller_uid, Gid caller_gid,
                                                 const std::string& path) override;

  Result<Metadata> GetMetadata(Uid caller_uid, Gid caller_gid, const std::string& path) override;
  ResultCode SetMetadata(Uid caller_uid, const std::string& path, Uid uid, Gid gid,
                         FileAttribute attribute, Modes modes) override;

  Result<NandStats> GetNandStats() override;
  Result<DirectoryStats> GetDirectoryStats(const std::string& path) override;
  Result<ExtendedDirectoryStats> GetExtendedDirectoryStats(const std::string& path) override;

  void SetNandRedirects(std::vector<NandRedirect> nand_redirects) override;

  // Orca rollback: rollback snapshots keep no NAND contents. Each records a journal mark instead
  // (DoState), and loading one undoes every NAND change journaled after its mark. Journaling starts
  // with the first snapshot. The snapshot ring forgets changes older than its oldest snapshot.
  u64 LastJournalMark() const { return m_journal_mark; }
  void JournalTrim(u64 mark);
  // Stops journaling and forgets every change (no snapshot remains to undo to).
  void JournalStop();
  // True once if journaling or undoing failed since the last call: the NAND may no longer match
  // what a snapshot expects.
  bool TakeJournalError() { return std::exchange(m_journal_error, false); }
  // Orca drop-in: the NAND's host folder, and re-reading the FST after its files were replaced
  // with a keyframe's (no handle may be open on them: load the keyframe's state next).
  const std::string& HostRoot() const { return m_root_path; }
  // Windows opens host files unshared: while the game has a NAND file open, nothing can read,
  // copy or delete it by path. These close (and so flush) every open host file, and reopen them
  // for the handles still open. Loading a keyframe's state reopens them itself.
  void CloseHostFiles();
  void ReopenHostFiles();
  void ReloadFst()
  {
    ResetFst();
    LoadFst();
  }

private:
  struct FstEntry
  {
    bool CheckPermission(Uid uid, Gid gid, Mode requested_mode) const;
    std::string name;
    Metadata data{};
    /// Children of this FST entry. Only valid for directories.
    ///
    /// We use a vector rather than a list here because iterating over children
    /// happens a lot more often than removals.
    /// Newly created entries are added at the end.
    std::vector<FstEntry> children;
  };

  struct Handle
  {
    bool opened = false;
    Mode mode = Mode::None;
    std::string wii_path;
    std::shared_ptr<File::IOFile> host_file;
    u32 file_offset = 0;
  };
  Handle* AssignFreeHandle();

  // Orca rollback journal (Journal.cpp). Paths are host paths. Entries are undone newest first.
  struct JournalEntry
  {
    enum class Kind
    {
      Fst,      // the FST before the first change since the last mark
      Created,  // a file or directory created at host_path
      Removed,  // a file or directory tree removed: removed_paths/removed_contents
      Written,  // bytes overwritten at offset (the old ones), and the size before the write
      Renamed,  // host_path renamed to new_host_path
    };
    Kind kind;
    u64 seq;
    std::string host_path;
    std::string new_host_path;
    u64 offset = 0;
    u64 old_size = 0;
    std::string old_bytes;
    // Removed: parents before children; a directory's content is empty and its path ends in '/'.
    std::vector<std::string> removed_paths;
    std::vector<std::string> removed_contents;
    FstEntry root_fst;
    FstEntry redirect_fst;
  };
  struct JournalCheckpoint
  {
    u64 next_seq;
    bool had_fst;
    std::optional<FstEntry> root_fst;
    std::optional<FstEntry> redirect_fst;
  };
  u64 JournalMark();
  void JournalUndo(u64 mark);
  void UndoJournalEntry(const JournalEntry& entry);
  JournalCheckpoint JournalBeginTransaction(bool preserve_fst = false) const;
  void JournalAbortTransaction(const JournalCheckpoint& checkpoint);
  void JournalFst();
  void JournalCreated(const std::string& host_path);
  bool JournalRemoved(const std::string& host_path);
  bool JournalWritten(const std::string& host_path, File::IOFile& file, u64 offset, u64 count);
  void JournalRenamed(const std::string& old_host_path, const std::string& new_host_path);

  Handle* GetHandleFromFd(Fd fd);
  Fd ConvertHandleToFd(const Handle* handle) const;

  struct HostFilename
  {
    std::string host_path;
    bool is_redirect;
  };
  HostFilename BuildFilename(
      const std::string& wii_path, std::string_view context = "unspecified",
      const std::source_location& location = std::source_location::current()) const;
  std::shared_ptr<File::IOFile> OpenHostFile(const std::string& host_path);

  ResultCode CreateFileOrDirectory(Uid uid, Gid gid, const std::string& path,
                                   FileAttribute attribute, Modes modes, bool is_file);
  bool IsFileOpened(const std::string& path) const;
  bool IsDirectoryInUse(const std::string& path) const;

  std::string GetFstFilePath() const;
  void ResetFst();
  void LoadFst();
  bool SaveFst();
  /// Get the FST entry for a file (or directory).
  /// Automatically creates fallback entries for parents if they do not exist.
  /// Returns nullptr if the path is invalid or the file does not exist.
  FstEntry* GetFstEntryForPath(const std::string& path);

  /// FST entry for the filesystem root.
  ///
  /// Note that unlike a real Wii's FST, ours is the single source of truth only for
  /// filesystem metadata and ordering. File existence must be checked by querying
  /// the host filesystem.
  /// The reasons for this design are twofold: existing users do not have a FST
  /// and we do not want FS to break if the user adds or removes files in their
  /// filesystem root manually.
  FstEntry m_root_entry{};
  std::string m_root_path;
  std::map<std::string, std::weak_ptr<File::IOFile>> m_open_files;
  // Rollback snapshots can run while asynchronous IOS work is completing. Keep partially
  // constructed handles private until OpenFile publishes them, and serialize all handle-table
  // access against DoState.
  mutable std::recursive_mutex m_handles_mutex;
  std::array<Handle, 16> m_handles{};

  FstEntry m_redirect_fst{};
  std::vector<NandRedirect> m_nand_redirects;

  bool m_journal_active = false;
  bool m_journal_has_fst = false;  // the FST was journaled since the last mark or undo
  bool m_journal_error = false;
  u64 m_journal_next_seq = 0;
  u64 m_journal_mark = 0;
  std::deque<JournalEntry> m_journal;
};

}  // namespace IOS::HLE::FS
