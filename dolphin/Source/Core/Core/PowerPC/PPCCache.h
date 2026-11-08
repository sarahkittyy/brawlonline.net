// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

class JitInterface;
namespace Memory
{
class MemoryManager;
}
class PointerWrap;
namespace PowerPC
{
struct PowerPCState;
}

namespace PowerPC
{
constexpr u32 CACHE_SETS = 128;
constexpr u32 CACHE_WAYS = 8;
// size of an instruction cache block in words
constexpr u32 CACHE_BLOCK_SIZE = 8;

constexpr u32 CACHE_EXRAM_BIT = 0x10000000;
constexpr u32 CACHE_VMEM_BIT = 0x20000000;

struct Cache
{
  std::array<std::array<std::array<u32, CACHE_BLOCK_SIZE>, CACHE_WAYS>, CACHE_SETS> data{};

  // Stores the 32-byte aligned address of the start of each cache block. This consists of the cache
  // set and tag. Real hardware only needs to store the tag, but also including the set simplifies
  // debugging and getting the actual address in the cache, without changing behavior (as the set
  // portion of the address is by definition the same for all addresses in a set).
  std::array<std::array<u32, CACHE_WAYS>, CACHE_SETS> addrs{};

  std::array<u8, CACHE_SETS> plru{};
  std::array<u8, CACHE_SETS> valid{};
  std::array<u8, CACHE_SETS> modified{};

  // Note: This is only for performance purposes; this same data could be computed at runtime
  // from the tags and valid fields (and that's how it's done on the actual cache)
  std::vector<u8> lookup_table{};
  std::vector<u8> lookup_table_ex{};
  std::vector<u8> lookup_table_vmem{};

  void Store(Memory::MemoryManager& memory, u32 addr);
  void Invalidate(Memory::MemoryManager& memory, u32 addr);
  void Flush(Memory::MemoryManager& memory, u32 addr);
  void Touch(Memory::MemoryManager& memory, u32 addr, bool store);

  void FlushAll(Memory::MemoryManager& memory);

  std::pair<u32, u32> GetCache(Memory::MemoryManager& memory, u32 addr, bool locked);

  void Read(Memory::MemoryManager& memory, u32 addr, void* buffer, u32 len, bool locked);
  void Write(Memory::MemoryManager& memory, u32 addr, const void* buffer, u32 len, bool locked);

  void Init(Memory::MemoryManager& memory);
  void Reset();

  void DoState(Memory::MemoryManager& memory, PointerWrap& p);
};

// A whole-machine session (Core/Rollback/MachineSession.cpp): the instructions each JIT block
// was compiled from, by its entry, until the game invalidates them (an icbi, a reset of the
// instruction cache). A block compiled again for the same entry is compiled from them, not from
// RAM. Without a session, a block runs the instructions it was compiled from for as long as it
// lives, and a game's codes write instructions without an icbi (the Gecko code handler,
// GeckoCode.h): a block compiled before such a write keeps the old ones. In a session a rollback
// drops blocks and compiles them again on one machine only, and that machine ran the new
// instructions where the others ran the old (Project+ in Other Games desynced at scene changes,
// 2026-11-02: one of its codes replaces a beq of Brawl's main loop with a nop, and the machine
// that had rolled back took the other path). The record is journaled, and a rollback's load
// undoes what came after its snapshot (RollbackManager::LoadFrame): the blocks first compiled in
// the frames it throws away are dropped, and compiled again when the game gets to them.
class JitCodeRecord
{
public:
  struct Instruction
  {
    u32 address;
    u32 physical_address;
    u32 hex;
    bool from_bat;
    bool operator==(const Instruction&) const = default;
  };
  using Block = std::vector<Instruction>;

  // Entry: the block's start address and its CPU feature flags (the JIT's key).
  static u64 Entry(u32 address, u32 feature_flags)
  {
    return static_cast<u64>(feature_flags) << 32 | address;
  }
  const Block* Find(u64 entry) const;
  void Record(u64 entry, Block block);
  // The game invalidated [physical_address, +length): the blocks that read any of it are forgotten.
  void Forget(u32 physical_address, u32 length);
  void ForgetAll();
  void Clear();
  // An entry whose record an Undo changed, with where its block starts in physical memory (a
  // record from before and after, so maybe twice): its JIT blocks must go
  // (JitBaseBlockCache::DropBlocksOfEntry). Only its own: a block is compiled from its entry's
  // record and from memory, never from another entry's record.
  struct Undone
  {
    u32 address;
    u32 feature_flags;
    u32 physical_address;
    bool operator==(const Undone&) const = default;
  };
  // A snapshot keeps the mark; Undo goes back to one (adding the entries whose blocks must go),
  // Trim forgets the changes before the oldest snapshot's.
  u64 Mark() const { return m_seq; }
  void Undo(u64 mark, std::vector<Undone>* undone);
  void Trim(u64 mark);

private:
  struct Change
  {
    u64 seq = 0;
    u64 entry = 0;
    bool all = false;             // a ForgetAll: the blocks it forgot are in `blocks`
    std::optional<Block> before;  // the entry's record before the change
    std::unordered_map<u64, Block> blocks;
  };

  void Index(u64 entry, const Block& block);
  void Drop(std::unordered_map<u64, Block>::iterator it);
  static void AddUndone(u64 entry, const Block& block, std::vector<Undone>* undone);

  std::unordered_map<u64, Block> m_blocks;
  // The entries whose blocks read each 32-byte line (an entry may have gone since).
  std::unordered_map<u32, std::vector<u64>> m_by_line;
  std::deque<Change> m_journal;
  u64 m_seq = 0;
};

struct InstructionCache : public Cache
{
  std::optional<Config::ConfigChangedCallbackID> m_config_callback_id = std::nullopt;

  JitCodeRecord jit_code;

  bool m_disable_icache = false;

  InstructionCache() = default;
  ~InstructionCache();
  u32 ReadInstruction(Memory::MemoryManager& memory, PowerPC::PowerPCState& ppc_state, u32 addr);
  void Invalidate(Memory::MemoryManager& memory, JitInterface& jit_interface, u32 addr);
  void Init(Memory::MemoryManager& memory);
  void Reset(JitInterface& jit_interface);
  void RefreshConfig();
};
}  // namespace PowerPC
