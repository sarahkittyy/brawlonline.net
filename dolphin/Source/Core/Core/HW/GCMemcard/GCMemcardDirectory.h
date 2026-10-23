// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Common/Event.h"
#include "Core/HW/GCMemcard/GCIFile.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardBase.h"
#include "DiscIO/Enums.h"

// Uncomment this to write the system data of the memorycard from directory to disc
// #define _WRITE_MC_HEADER 1
void MigrateFromMemcardFile(const std::string& directory_name, ExpansionInterface::Slot card_slot,
                            DiscIO::Region region);

class GCMemcardDirectory : public MemoryCardBase
{
public:
  // A whole-machine session's card (`session`, docs/design/brawl-mods.md §6.5) is written to
  // `directory` only when the session ends, and only with `write_back` (the player who booted the
  // game); nothing is written while the game runs, since rollbacks rewrite the card's past.
  GCMemcardDirectory(std::string directory, ExpansionInterface::Slot slot,
                     const Memcard::HeaderData& header_data, u32 game_id, bool session = false,
                     bool write_back = false);
  ~GCMemcardDirectory() override;

  GCMemcardDirectory(const GCMemcardDirectory&) = delete;
  GCMemcardDirectory& operator=(const GCMemcardDirectory&) = delete;
  GCMemcardDirectory(GCMemcardDirectory&&) = delete;
  GCMemcardDirectory& operator=(GCMemcardDirectory&&) = delete;

  static std::vector<std::string> GetFileNamesForGameID(const std::string& directory,
                                                        const std::string& game_id);
  void FlushToFile();
  void FlushThread();
  s32 Read(u32 src_address, s32 length, u8* dest_address) override;
  s32 Write(u32 dest_address, s32 length, const u8* src_address) override;
  void ClearBlock(u32 address) override;
  void ClearAll() override {}
  void DoState(PointerWrap& p) override;

private:
  bool LoadGCI(Memcard::GCIFile gci);
  inline s32 SaveAreaRW(u32 block, bool writing = false);
  // s32 DirectoryRead(u32 offset, u32 length, u8* dest_address);
  s32 DirectoryWrite(u32 dest_address, u32 length, const u8* src_address);
  inline void SyncSaves();
  bool SetUsedBlocks(int save_index);

  u32 m_game_id;
  s32 m_last_block;
  u8* m_last_block_address;

  Memcard::Header m_hdr;
  Memcard::Directory m_dir1;
  Memcard::Directory m_dir2;
  Memcard::BlockAlloc m_bat1;
  Memcard::BlockAlloc m_bat2;
  std::vector<Memcard::GCIFile> m_saves;

  std::string m_save_directory;
  bool m_session = false;
  bool m_session_write_back = false;
  // The files this card loaded itself: in a session, a loaded state keeps only these names (a
  // joiner's state comes from another player, whose paths are theirs).
  std::set<std::string> m_own_files;
  Common::Event m_flush_trigger;
  std::mutex m_write_mutex;
  Common::Flag m_exiting;
  std::thread m_flush_thread;
};
