// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Jit64Common/TrampolineInfo.h"

// What BackPatch needs to turn a fastmem load or store into a trampoline call, by the address of
// the access's MOV. The JIT adds an entry for every such access it compiles (several a block), and
// only a fault reads one, so this is made for adding: the entries sit in the order they were added,
// in chunks that never move, and an open-addressing table of 8-byte slots finds them. Adding one
// touches one slot and the end of the newest chunk, and allocates nothing but a chunk every
// CHUNK_SIZE entries (a std::unordered_map allocated a node and walked a bucket for each).
class BackPatchTable
{
public:
  // The entry for the MOV at `key`: a new one, or the one already there when the code that was at
  // that address has been freed and the space compiled into again (as with a map's operator[]).
  // The reference stays valid until Clear.
  TrampolineInfo& operator[](const u8* key)
  {
    if (m_count * 2 >= m_slots.size())
      Grow();

    const u32 tag = Tag(key);
    for (size_t i = Hash(key);; i = (i + 1) & (m_slots.size() - 1))
    {
      Slot& slot = m_slots[i];
      if (slot.index_plus_one == 0)
      {
        if (m_count == m_chunks.size() * CHUNK_SIZE)
          m_chunks.push_back(std::make_unique<Entry[]>(CHUNK_SIZE));
        Entry& entry = EntryAt(m_count);
        entry.key = key;
        entry.info = TrampolineInfo{};
        slot.tag = tag;
        slot.index_plus_one = static_cast<u32>(++m_count);
        return entry.info;
      }
      if (slot.tag == tag)
      {
        Entry& entry = EntryAt(slot.index_plus_one - 1);
        if (entry.key == key)
          return entry.info;
      }
    }
  }

  // The entry for the MOV at `key`, or nullptr.
  TrampolineInfo* Find(const u8* key)
  {
    if (m_slots.empty())
      return nullptr;

    const u32 tag = Tag(key);
    for (size_t i = Hash(key);; i = (i + 1) & (m_slots.size() - 1))
    {
      const Slot& slot = m_slots[i];
      if (slot.index_plus_one == 0)
        return nullptr;
      if (slot.tag == tag)
      {
        Entry& entry = EntryAt(slot.index_plus_one - 1);
        if (entry.key == key)
          return &entry.info;
      }
    }
  }

  void Clear()
  {
    m_slots = {};
    m_chunks.clear();
    m_count = 0;
    m_shift = 64;
  }

  size_t Size() const { return m_count; }

private:
  static constexpr size_t CHUNK_SIZE = 4096;
  static constexpr size_t INITIAL_SLOTS_LOG2 = 16;

  struct Entry
  {
    const u8* key;
    TrampolineInfo info;
  };

  // Empty when index_plus_one is 0. The tag is the low half of the key: enough to tell nearly
  // every other key apart without reading its entry.
  struct Slot
  {
    u32 tag;
    u32 index_plus_one;
  };

  static u32 Tag(const u8* key) { return static_cast<u32>(reinterpret_cast<uintptr_t>(key)); }

  // MOVs are a few bytes apart: the multiplication spreads neighbouring addresses over the table.
  size_t Hash(const u8* key) const
  {
    return static_cast<size_t>(
        (static_cast<u64>(reinterpret_cast<uintptr_t>(key)) * 0x9E3779B97F4A7C15ULL) >> m_shift);
  }

  Entry& EntryAt(size_t index) { return m_chunks[index / CHUNK_SIZE][index % CHUNK_SIZE]; }

  // Twice the slots (at most half of them are ever used), with every entry placed again.
  void Grow()
  {
    const size_t log2 = m_slots.empty() ? INITIAL_SLOTS_LOG2 : 64 - m_shift + 1;
    m_slots.assign(size_t{1} << log2, Slot{});
    m_shift = static_cast<u32>(64 - log2);
    for (size_t index = 0; index < m_count; ++index)
    {
      const u8* const key = EntryAt(index).key;
      size_t i = Hash(key);
      while (m_slots[i].index_plus_one != 0)
        i = (i + 1) & (m_slots.size() - 1);
      m_slots[i] = Slot{Tag(key), static_cast<u32>(index + 1)};
    }
  }

  std::vector<Slot> m_slots;
  std::vector<std::unique_ptr<Entry[]>> m_chunks;
  size_t m_count = 0;
  u32 m_shift = 64;
};
