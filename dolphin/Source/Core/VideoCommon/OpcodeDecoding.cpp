// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// DL facts:
//  Ikaruga uses (nearly) NO display lists!
//  Zelda WW uses TONS of display lists
//  Zelda TP uses almost 100% display lists except menus (we like this!)
//  Super Mario Galaxy has nearly all geometry and more than half of the state in DLs (great!)

// Note that it IS NOT GENERALLY POSSIBLE to precompile display lists! You can compile them as they
// are while interpreting them, and hope that the vertex format doesn't change, though, if you do
// it right when they are called. The reason is that the vertex format affects the sizes of the
// vertices.

#include "VideoCommon/OpcodeDecoding.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

namespace OpcodeDecoder
{
bool g_record_fifo_data = false;

// Display-list cache for the deterministic GPU thread (docs/gameplay-rollback-status.md, "Display
// lists are preprocessed once"). With the deterministic GPU thread, the CPU thread preprocesses
// every display list a frame calls: it copies the list into the aux FIFO for the GPU thread and
// parses it for what preprocessing needs (vertex sizes, CP state, the BP writes that set tokens,
// draw done and EFB copies, indexed XF loads, the GPU time). A game calls mostly the same lists
// every frame (models, materials), so the cache keeps a copy of each list with the side effects
// its parse had, in order. When a list's bytes are the same as the copy (compared exactly) and,
// if it draws, the vertex format it was parsed under is the same, the parse is replayed instead
// of run and the GPU thread reads the cached copy: the GPU thread gets the same bytes, and the
// preprocessing state and its cycles come out the same as a parse's. PPR_DL_CACHE=0 turns it off.
namespace
{
struct DlEffect
{
  u8 kind;  // 0 CP write, 1 BP write, 2 indexed XF load
  u8 a;     // CP/BP register, or CPArray
  u16 address;
  u32 value;  // CP/BP value, or index
  u32 size;   // indexed load size
  u32 gap;    // cycles of the commands not replayed since the previous effect
};

struct DlEntry
{
  std::vector<u8> bytes;
  // The vertex format at the list's start: only primitives read it (their vertex sizes), so a list
  // without primitives parses the same under any format, and one with them depends on the vertex
  // descriptor and the VATs they use.
  std::array<u8, sizeof(TVtxDesc)> vtx_desc;
  std::array<u8, sizeof(std::array<VAT, CP_NUM_VAT_REG>)> vtx_attr;
  bool uses_format = false;
  u8 vat_mask = 0;
  std::vector<DlEffect> effects;
  u32 tail = 0;            // cycles after the last effect
  bool cacheable = true;   // false: an unknown opcode (its handling is not replayed)
};

struct DlSlot
{
  std::unique_ptr<DlEntry> entry;
  u32 changes = 0;
  bool dynamic = false;  // changed too often: preprocessed as before
};

// What the aux FIFO carries for a display list while the cache is on: a pointer to the cached copy,
// or null and the list's bytes after it.
struct DlHeader
{
  const u8* ptr;
  u64 size;
};

constexpr u32 DL_MAX_CHANGES = 4;
constexpr size_t DL_CACHE_MAX_BYTES = 32 * 1024 * 1024;

std::unordered_map<u64, DlSlot> s_dl_cache;
// Replaced copies, and those of a cache emptied for its size: the GPU thread may still read them
// through headers in the aux FIFO, so they are freed only when the aux FIFO is empty
// (DisplayListCacheOnGpuIdle).
std::vector<std::unique_ptr<DlEntry>> s_dl_retired;
size_t s_dl_bytes = 0;  // the copies in s_dl_cache

bool DlCacheEnabled()
{
  static const bool on = [] {
    const char* v = std::getenv("PPR_DL_CACHE");
    return !v || std::strcmp(v, "0") != 0;
  }();
  return on;
}

bool SameFormat(const DlEntry& e, const CPState& cp)
{
  if (!e.uses_format)
    return true;
  if (std::memcmp(e.vtx_desc.data(), &cp.vtx_desc, sizeof(e.vtx_desc)) != 0)
    return false;
  const u8* const cached = e.vtx_attr.data();
  const u8* const live = reinterpret_cast<const u8*>(&cp.vtx_attr);
  for (u32 i = 0; i < CP_NUM_VAT_REG; ++i)
  {
    if ((e.vat_mask & (1u << i)) != 0 &&
        std::memcmp(cached + i * sizeof(VAT), live + i * sizeof(VAT), sizeof(VAT)) != 0)
    {
      return false;
    }
  }
  return true;
}

// The BP registers LoadBPRegPreprocess acts on; the others only take cycles.
bool PreprocessedBP(u8 reg)
{
  return reg == BPMEM_EFB_WH || reg == BPMEM_EFB_ADDR || reg == BPMEM_EFB_STRIDE ||
         reg == BPMEM_SETDRAWDONE || reg == BPMEM_PE_TOKEN_ID || reg == BPMEM_PE_TOKEN_INT_ID ||
         reg == BPMEM_TRIGGER_EFB_COPY;
}
}  // namespace

void DisplayListCacheOnGpuIdle()
{
  // Only retired copies: SyncGPU also runs inside PushFifoAuxBuffer (AuxSpace), so this can be
  // called in the middle of PreprocessCachedList, which still uses its slot and entry.
  s_dl_retired.clear();
}

template <bool is_preprocess>
class RunCallback final : public Callback
{
public:
  OPCODE_CALLBACK(void OnXF(u16 address, u8 count, const u8* data))
  {
    m_cycles += 18 + 6 * count;

    if constexpr (!is_preprocess)
    {
      LoadXFReg(address, count, data);

      INCSTAT(g_stats.this_frame.num_xf_loads);
    }
  }
  OPCODE_CALLBACK(void OnCP(u8 command, u32 value))
  {
    const u32 rec_gap = m_cycles - m_rec_last;
    m_cycles += 12;
    const u8 sub_command = command & CP_COMMAND_MASK;
    if constexpr (!is_preprocess)
    {
      if (sub_command == MATINDEX_A)
      {
        VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
        auto& system = Core::System::GetInstance();
        system.GetXFStateManager().SetTexMatrixChangedA(value);
      }
      else if (sub_command == MATINDEX_B)
      {
        VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
        auto& system = Core::System::GetInstance();
        system.GetXFStateManager().SetTexMatrixChangedB(value);
      }
      else if (sub_command == VCD_LO || sub_command == VCD_HI)
      {
        VertexLoaderManager::g_main_vat_dirty = BitSet8::AllTrue(CP_NUM_VAT_REG);
        VertexLoaderManager::g_bases_dirty = true;
        VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
      }
      else if (sub_command == CP_VAT_REG_A || sub_command == CP_VAT_REG_B ||
               sub_command == CP_VAT_REG_C)
      {
        VertexLoaderManager::g_main_vat_dirty[command & CP_VAT_MASK] = true;
        VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
      }
      else if (sub_command == ARRAY_BASE)
      {
        VertexLoaderManager::g_bases_dirty = true;
      }

      INCSTAT(g_stats.this_frame.num_cp_loads);
    }
    else if constexpr (is_preprocess)
    {
      if (sub_command == VCD_LO || sub_command == VCD_HI)
      {
        VertexLoaderManager::g_preprocess_vat_dirty = BitSet8::AllTrue(CP_NUM_VAT_REG);
      }
      else if (sub_command == CP_VAT_REG_A || sub_command == CP_VAT_REG_B ||
               sub_command == CP_VAT_REG_C)
      {
        VertexLoaderManager::g_preprocess_vat_dirty[command & CP_VAT_MASK] = true;
      }
    }
    GetCPState().LoadCPReg(command, value);
    if constexpr (is_preprocess)
    {
      if (m_rec)
      {
        m_rec->effects.push_back({0, command, 0, value, 0, rec_gap});
        m_rec_last = m_cycles;
      }
    }
  }
  OPCODE_CALLBACK(void OnBP(u8 command, u32 value))
  {
    const u32 rec_gap = m_cycles - m_rec_last;
    m_cycles += 12;

    if constexpr (is_preprocess)
    {
      LoadBPRegPreprocess(command, value, m_cycles);
      if (m_rec && PreprocessedBP(command))
      {
        m_rec->effects.push_back({1, command, 0, value, 0, rec_gap});
        m_rec_last = m_cycles;
      }
    }
    else
    {
      LoadBPReg(command, value, m_cycles);
      INCSTAT(g_stats.this_frame.num_bp_loads);
    }
  }
  OPCODE_CALLBACK(void OnIndexedLoad(CPArray array, u32 index, u16 address, u8 size))
  {
    const u32 rec_gap = m_cycles - m_rec_last;
    m_cycles += 6;

    if constexpr (is_preprocess)
    {
      PreprocessIndexedXF(array, index, address, size);
      if (m_rec)
      {
        m_rec->effects.push_back({2, static_cast<u8>(array), address, index, size, rec_gap});
        m_rec_last = m_cycles;
      }
    }
    else
      LoadIndexedXF(array, index, address, size);
  }
  OPCODE_CALLBACK(void OnPrimitiveCommand(OpcodeDecoder::Primitive primitive, u8 vat,
                                          u32 vertex_size, u16 num_vertices, const u8* vertex_data))
  {
    if constexpr (is_preprocess)
    {
      if (m_rec)
      {
        m_rec->uses_format = true;
        m_rec->vat_mask |= static_cast<u8>(1u << (vat & 7));
      }
    }
    // load vertices
    const u32 size = vertex_size * num_vertices;

    const u32 bytes =
        VertexLoaderManager::RunVertices<is_preprocess>(vat, primitive, num_vertices, vertex_data);

    ASSERT(bytes == size);

    // 4 GPU ticks per vertex, 3 CPU ticks per GPU tick
    m_cycles += num_vertices * 4 * 3 + 6;
  }
  // This can't be inlined since it calls Run, which makes it recursive
  // m_in_display_list prevents it from actually recursing infinitely, but there's no real benefit
  // to inlining Run for the display list directly.
  OPCODE_CALLBACK_NOINLINE(void OnDisplayList(u32 address, u32 size))
  {
    m_cycles += 6;

    if (m_in_display_list)
    {
      WARN_LOG_FMT(VIDEO, "recursive display list detected");
    }
    else
    {
      m_in_display_list = true;

      auto& system = Core::System::GetInstance();

      if constexpr (is_preprocess)
      {
        auto& memory = system.GetMemory();
        const u8* const start_address = memory.GetPointerForRange(address, size);

        if (DlCacheEnabled())
        {
          PreprocessCachedList(system, address, size, start_address);
        }
        else
        {
          system.GetFifo().PushFifoAuxBuffer(start_address, size);

          if (start_address != nullptr)
          {
            Run(start_address, size, *this);
          }
        }
      }
      else
      {
        const u8* start_address;

        auto& fifo = system.GetFifo();
        if (fifo.UseDeterministicGPUThread())
        {
          if (DlCacheEnabled())
          {
            DlHeader header;
            std::memcpy(&header, fifo.PopFifoAuxBuffer(sizeof(header)), sizeof(header));
            start_address = header.ptr ? header.ptr : static_cast<u8*>(fifo.PopFifoAuxBuffer(size));
          }
          else
          {
            start_address = static_cast<u8*>(fifo.PopFifoAuxBuffer(size));
          }
        }
        else
        {
          auto& memory = system.GetMemory();
          start_address = memory.GetPointerForRange(address, size);
        }

        // Avoid the crash if memory.GetPointerForRange failed ..
        if (start_address != nullptr)
        {
          // temporarily swap dl and non-dl (small "hack" for the stats)
          g_stats.SwapDL();

          Run(start_address, size, *this);
          INCSTAT(g_stats.this_frame.num_dlists_called);

          // un-swap
          g_stats.SwapDL();
        }
      }

      m_in_display_list = false;
    }
  }
  OPCODE_CALLBACK(void OnNop(u32 count))
  {
    m_cycles += 6 * count;  // Hm, this means that we scan over nop streams pretty slowly...
  }
  OPCODE_CALLBACK(void OnUnknown(u8 opcode, const u8* data))
  {
    if (static_cast<Opcode>(opcode) == Opcode::GX_CMD_UNKNOWN_METRICS)
    {
      // 'Zelda Four Swords' calls it and checks the metrics registers after that
      m_cycles += 6;
      DEBUG_LOG_FMT(VIDEO, "GX 0x44");
    }
    else if (static_cast<Opcode>(opcode) == Opcode::GX_CMD_INVL_VC)
    {
      // Invalidate Vertex Cache
      m_cycles += 6;
      DEBUG_LOG_FMT(VIDEO, "Invalidate (vertex cache?)");
    }
    else
    {
      auto& system = Core::System::GetInstance();
      system.GetCommandProcessor().HandleUnknownOpcode(opcode, data, is_preprocess);
      m_cycles += 1;
      if constexpr (is_preprocess)
      {
        if (m_rec)
          m_rec->cacheable = false;
      }
    }
  }

  OPCODE_CALLBACK(void OnCommand(const u8* data, u32 size))
  {
    ASSERT(size >= 1);
    if constexpr (!is_preprocess)
    {
      // Display lists get added directly into the FIFO stream since this same callback is used to
      // process them.
      if (g_record_fifo_data && static_cast<Opcode>(data[0]) != Opcode::GX_CMD_CALL_DL)
      {
        Core::System::GetInstance().GetFifoRecorder().WriteGPCommand(data, size);
      }
    }
  }

  OPCODE_CALLBACK(CPState& GetCPState())
  {
    if constexpr (is_preprocess)
      return g_preprocess_cp_state;
    else
      return g_main_cp_state;
  }

  OPCODE_CALLBACK(u32 GetVertexSize(u8 vat))
  {
    if constexpr (is_preprocess)
    {
      if (m_rec)
      {
        m_rec->uses_format = true;
        m_rec->vat_mask |= static_cast<u8>(1u << (vat & 7));
      }
    }
    VertexLoaderBase* loader = VertexLoaderManager::RefreshLoader<is_preprocess>(vat);
    return loader->m_vertex_size;
  }

  // Preprocesses a display list with the cache (see DlEntry). The aux FIFO gets a DlHeader per
  // list, and after it the list's bytes when the header has no pointer.
  void PreprocessCachedList(Core::System& system, u32 address, u32 size, const u8* start_address)
  {
    auto& fifo = system.GetFifo();
    const auto push_inline = [&] {
      const DlHeader header{nullptr, size};
      fifo.PushFifoAuxBuffer(&header, sizeof(header));
      fifo.PushFifoAuxBuffer(start_address, size);
    };
    if (start_address == nullptr || size == 0)
    {
      push_inline();
      return;
    }

    if (s_dl_bytes > DL_CACHE_MAX_BYTES)
    {
      // Start over. Headers in the aux FIFO may still point at these copies: they are retired.
      for (auto& kv : s_dl_cache)
      {
        if (kv.second.entry)
          s_dl_retired.push_back(std::move(kv.second.entry));
      }
      s_dl_cache.clear();
      s_dl_bytes = 0;
    }

    DlSlot& slot = s_dl_cache[(static_cast<u64>(address) << 32) | size];
    if (slot.dynamic)
    {
      push_inline();
      Run(start_address, size, *this);
      return;
    }

    const CPState& cp = g_preprocess_cp_state;
    DlEntry* const cached = slot.entry.get();
    if (cached && cached->cacheable && SameFormat(*cached, cp) &&
        std::memcmp(cached->bytes.data(), start_address, size) == 0)
    {
      // The GPU thread reads the cached copy; the parse's side effects are replayed in order, each
      // after the cycles of the commands before it, so BP writes see the same cycle counts.
      const DlHeader header{cached->bytes.data(), size};
      fifo.PushFifoAuxBuffer(&header, sizeof(header));
      for (const DlEffect& effect : cached->effects)
      {
        m_cycles += effect.gap;
        if (effect.kind == 0)
          OnCP(effect.a, effect.value);
        else if (effect.kind == 1)
          OnBP(effect.a, effect.value);
        else
          OnIndexedLoad(static_cast<CPArray>(effect.a), effect.value, effect.address,
                        static_cast<u8>(effect.size));
      }
      m_cycles += cached->tail;
      return;
    }

    // New or changed (another list at the same address and size, other bytes, another format).
    if (cached)
    {
      s_dl_bytes -= cached->bytes.size();
      s_dl_retired.push_back(std::move(slot.entry));
      if (++slot.changes > DL_MAX_CHANGES)
      {
        slot.dynamic = true;
        push_inline();
        Run(start_address, size, *this);
        return;
      }
    }
    auto entry = std::make_unique<DlEntry>();
    entry->bytes.assign(start_address, start_address + size);
    std::memcpy(entry->vtx_desc.data(), &cp.vtx_desc, sizeof(entry->vtx_desc));
    std::memcpy(entry->vtx_attr.data(), &cp.vtx_attr, sizeof(entry->vtx_attr));
    s_dl_bytes += size;
    const DlHeader header{entry->bytes.data(), size};
    fifo.PushFifoAuxBuffer(&header, sizeof(header));
    m_rec = entry.get();
    m_rec_last = m_cycles;
    Run(entry->bytes.data(), size, *this);
    entry->tail = m_cycles - m_rec_last;
    m_rec = nullptr;
    slot.entry = std::move(entry);
  }

  u32 m_cycles = 0;
  bool m_in_display_list = false;
  DlEntry* m_rec = nullptr;  // the display list whose parse is being recorded
  u32 m_rec_last = 0;        // m_cycles after the last recorded effect
};

template <bool is_preprocess>
u8* RunFifo(DataReader src, u32* cycles)
{
  using CallbackT = RunCallback<is_preprocess>;
  auto callback = CallbackT{};
  u32 size = Run(src.GetPointer(), static_cast<u32>(src.size()), callback);

  if (cycles != nullptr)
    *cycles = callback.m_cycles;

  src.Skip(size);
  return src.GetPointer();
}

template u8* RunFifo<true>(DataReader src, u32* cycles);
template u8* RunFifo<false>(DataReader src, u32* cycles);

}  // namespace OpcodeDecoder
