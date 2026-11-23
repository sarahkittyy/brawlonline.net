// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/FrameInterpolation.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <unordered_set>
#include <variant>

#include <fmt/format.h>
#include <xxhash.h>

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/ConfigManager.h"
#include "Core/System.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

namespace FrameInterpolation
{
// ---------------------------------------------------------------------------------------------
// Math

namespace
{
bool AllFinite(const float* values, size_t count)
{
  for (size_t i = 0; i < count; ++i)
  {
    if (!std::isfinite(values[i]))
      return false;
  }
  return true;
}

struct RigidParts
{
  std::array<float, 4> rotation{};  // quaternion x, y, z, w
  std::array<float, 3> scale{};
  std::array<float, 3> translation{};
};

// M = R * S + t, with S diagonal: the columns of the 3x3 part are the rotation's columns scaled.
bool SplitRigid(const Mat34& m, RigidParts* parts)
{
  if (!AllFinite(m.data(), m.size()))
    return false;

  float r[3][3];
  for (int col = 0; col < 3; ++col)
  {
    const float x = m[col];
    const float y = m[4 + col];
    const float z = m[8 + col];
    const float length = std::sqrt(x * x + y * y + z * z);
    if (!(length > 1e-6f) || !std::isfinite(length))
      return false;
    parts->scale[col] = length;
    r[0][col] = x / length;
    r[1][col] = y / length;
    r[2][col] = z / length;
  }

  const auto column_dot = [&r](int a, int b) {
    return r[0][a] * r[0][b] + r[1][a] * r[1][b] + r[2][a] * r[2][b];
  };
  if (std::abs(column_dot(0, 1)) > 0.05f || std::abs(column_dot(0, 2)) > 0.05f ||
      std::abs(column_dot(1, 2)) > 0.05f)
  {
    return false;  // shear
  }

  const float det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
                    r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                    r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
  if (!std::isfinite(det) || std::abs(std::abs(det) - 1.0f) > 0.1f)
    return false;
  if (det < 0.0f)
  {
    // A mirror: keep it in the scale so that the rest is a rotation.
    parts->scale[0] = -parts->scale[0];
    for (auto& row : r)
      row[0] = -row[0];
  }

  float qx, qy, qz, qw;
  const float trace = r[0][0] + r[1][1] + r[2][2];
  if (trace > 0.0f)
  {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    qw = 0.25f * s;
    qx = (r[2][1] - r[1][2]) / s;
    qy = (r[0][2] - r[2][0]) / s;
    qz = (r[1][0] - r[0][1]) / s;
  }
  else if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
  {
    const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
    qw = (r[2][1] - r[1][2]) / s;
    qx = 0.25f * s;
    qy = (r[0][1] + r[1][0]) / s;
    qz = (r[0][2] + r[2][0]) / s;
  }
  else if (r[1][1] > r[2][2])
  {
    const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
    qw = (r[0][2] - r[2][0]) / s;
    qx = (r[0][1] + r[1][0]) / s;
    qy = 0.25f * s;
    qz = (r[1][2] + r[2][1]) / s;
  }
  else
  {
    const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
    qw = (r[1][0] - r[0][1]) / s;
    qx = (r[0][2] + r[2][0]) / s;
    qy = (r[1][2] + r[2][1]) / s;
    qz = 0.25f * s;
  }
  parts->rotation = {qx, qy, qz, qw};
  parts->translation = {m[3], m[7], m[11]};
  return AllFinite(parts->rotation.data(), 4);
}

float Distance2(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
  const float dx = a[0] - b[0];
  const float dy = a[1] - b[1];
  const float dz = a[2] - b[2];
  return dx * dx + dy * dy + dz * dz;
}
}  // namespace

void BlendLinear(const float* prev, const float* cur, size_t count, float weight, float* out)
{
  for (size_t i = 0; i < count; ++i)
  {
    const bool finite = std::isfinite(prev[i]) && std::isfinite(cur[i]);
    out[i] = finite ? prev[i] + (cur[i] - prev[i]) * weight : cur[i];
  }
}

bool BlendRigid(const Mat34& prev, const Mat34& cur, float weight, Mat34* out)
{
  if (prev == cur)
  {
    *out = cur;
    return true;
  }

  RigidParts a, b;
  if (!SplitRigid(prev, &a) || !SplitRigid(cur, &b))
  {
    BlendLinear(prev.data(), cur.data(), 12, weight, out->data());
    return false;
  }

  float dot = a.rotation[0] * b.rotation[0] + a.rotation[1] * b.rotation[1] +
              a.rotation[2] * b.rotation[2] + a.rotation[3] * b.rotation[3];
  if (dot < 0.0f)
  {
    for (float& c : b.rotation)
      c = -c;
    dot = -dot;
  }
  // Two frames apart a draw does not turn more than 90 degrees; if it seems to, it is not the
  // same motion (or the pair is wrong).
  if (!(dot >= 0.70710678f))
  {
    BlendLinear(prev.data(), cur.data(), 12, weight, out->data());
    return false;
  }

  std::array<float, 4> q;
  for (int i = 0; i < 4; ++i)
    q[i] = a.rotation[i] + (b.rotation[i] - a.rotation[i]) * weight;
  const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(length > 1e-6f))
  {
    BlendLinear(prev.data(), cur.data(), 12, weight, out->data());
    return false;
  }
  for (float& c : q)
    c /= length;

  const float x = q[0], y = q[1], z = q[2], w = q[3];
  const float rot[3][3] = {
      {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - w * z), 2.0f * (x * z + w * y)},
      {2.0f * (x * y + w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - w * x)},
      {2.0f * (x * z - w * y), 2.0f * (y * z + w * x), 1.0f - 2.0f * (x * x + y * y)},
  };
  std::array<float, 3> scale, translation;
  for (int i = 0; i < 3; ++i)
  {
    scale[i] = a.scale[i] + (b.scale[i] - a.scale[i]) * weight;
    translation[i] = a.translation[i] + (b.translation[i] - a.translation[i]) * weight;
  }
  for (int row = 0; row < 3; ++row)
  {
    for (int col = 0; col < 3; ++col)
      (*out)[row * 4 + col] = rot[row][col] * scale[col];
    (*out)[row * 4 + 3] = translation[row];
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Matching

void KeyIndex::Build(const std::vector<DrawKey>& draws)
{
  Clear();
  for (u32 i = 0; i < draws.size(); ++i)
  {
    if (!draws[i].perspective)
      continue;
    exact[draws[i].exact].push_back(i);
    mesh[draws[i].mesh].push_back(i);
    stable[draws[i].stable].push_back(i);
  }
}

void KeyIndex::Clear()
{
  exact.clear();
  mesh.clear();
  stable.clear();
}

namespace
{
bool WithinGate(const DrawKey& prev, const DrawKey& cur)
{
  if (prev.per_vertex != cur.per_vertex)
    return false;
  if (prev.per_vertex && prev.used_rows != cur.used_rows)
    return false;
  // NaN fails too.
  return Distance2(prev.translation, cur.translation) <= MATCH_GATE * MATCH_GATE;
}
}  // namespace

void MatchDraws(const std::vector<DrawKey>& prev, const KeyIndex& prev_index,
               std::vector<DrawKey>* cur, std::vector<int>* partner)
{
  partner->assign(cur->size(), -1);
  std::vector<u8> claimed(prev.size(), 0);
  for (DrawKey& key : *cur)
    key.has_prediction = false;

  const auto pair = [&](u32 p, u32 c) {
    claimed[p] = 1;
    (*partner)[c] = static_cast<int>(p);
    DrawKey& key = (*cur)[c];
    for (int i = 0; i < 3; ++i)
      key.predicted[i] = 2.0f * key.translation[i] - prev[p].translation[i];
    key.has_prediction = true;
  };

  std::unordered_map<u64, std::vector<u32>> groups;
  std::vector<u32> candidates;
  std::vector<std::tuple<float, u32, u32>> edges;
  // The same draw first; then the same mesh with another material (in matches, every draw of one
  // fighter for stretches of two frames in three: a quarter of the frame's draws); then the same
  // material with other vertices.
  const std::pair<u64 DrawKey::*, const std::unordered_map<u64, std::vector<u32>>*> passes[] = {
      {&DrawKey::exact, &prev_index.exact},
      {&DrawKey::mesh, &prev_index.mesh},
      {&DrawKey::stable, &prev_index.stable},
  };
  for (const auto& [key_of, index_of] : passes)
  {
    groups.clear();
    for (size_t c = 0; c < cur->size(); ++c)
    {
      const DrawKey& key = (*cur)[c];
      if (!key.perspective || (*partner)[c] >= 0)
        continue;
      groups[key.*key_of].push_back(static_cast<u32>(c));
    }

    const auto& index = *index_of;
    for (const auto& [key, current] : groups)
    {
      const auto it = index.find(key);
      if (it == index.end())
        continue;
      candidates.clear();
      for (const u32 p : it->second)
      {
        if (!claimed[p])
          candidates.push_back(p);
      }
      if (candidates.empty())
        continue;

      if (candidates.size() == 1 && current.size() == 1)
      {
        if (WithinGate(prev[candidates[0]], (*cur)[current[0]]))
          pair(candidates[0], current[0]);
        continue;
      }

      if (candidates.size() * current.size() > 16384)
      {
        // A crowd of the same mesh (character select icons): the same order both frames.
        const size_t n = std::min(candidates.size(), current.size());
        for (size_t i = 0; i < n; ++i)
        {
          if (WithinGate(prev[candidates[i]], (*cur)[current[i]]))
            pair(candidates[i], current[i]);
        }
        continue;
      }

      // The same mesh several times: nearest first, from where each was going.
      edges.clear();
      for (const u32 p : candidates)
      {
        const DrawKey& pk = prev[p];
        const auto& from = pk.has_prediction ? pk.predicted : pk.translation;
        for (const u32 c : current)
        {
          if (WithinGate(pk, (*cur)[c]))
            edges.emplace_back(Distance2(from, (*cur)[c].translation), p, c);
        }
      }
      std::sort(edges.begin(), edges.end());
      for (const auto& [distance, p, c] : edges)
      {
        if (!claimed[p] && (*partner)[c] < 0)
          pair(p, c);
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Video thread state

namespace
{
struct DrawEvent
{
  const AbstractPipeline* pipeline = nullptr;
  PrimitiveType primitive{};
  u32 vertex_stride = 0;
  u32 num_vertices = 0;
  u32 num_indices = 0;
  u32 vertex_offset = 0;  // in FrameEvents::vertices
  u32 index_offset = 0;   // in FrameEvents::indices
  u32 key = 0;            // in FrameRecord::keys
  u32 ps = 0;             // in FrameEvents::ps
  u32 gs = 0;             // in FrameEvents::gs
  u32 used_textures = 0;
  std::array<RcTcacheEntry, 8> textures;
  std::array<SamplerState, 8> samplers{};
  AbstractGfx::ViewportAndScissor viewport;
};

struct ClearEvent
{
  MathUtil::Rectangle<int> rc;
  bool color_enable = false;
  bool alpha_enable = false;
  bool z_enable = false;
  u32 color = 0;
  u32 z = 0;
  PixelFormat pixel_format{};
};

struct CopyEvent
{
  RcTcacheEntry native;  // the copy the game made
  EFBCopyReplay copy;
};

using Event = std::variant<DrawEvent, ClearEvent, CopyEvent>;

// What a frame's matching needs of it, kept for the next frame.
struct FrameRecord
{
  std::vector<DrawKey> keys;
  std::vector<VertexShaderConstants> vs;
  void Clear()
  {
    keys.clear();
    vs.clear();
  }
};

// The frame's draws, clears and copies in order, with what replaying them needs.
struct FrameEvents
{
  std::vector<Event> events;
  std::vector<u8> vertices;
  std::vector<u16> indices;
  std::vector<PixelShaderConstants> ps;
  std::vector<GeometryShaderConstants> gs;
  std::unordered_set<const TCacheEntry*> textures;
  void Clear()
  {
    events.clear();
    vertices.clear();
    indices.clear();
    ps.clear();
    gs.clear();
    textures.clear();
  }
};

// One in-between picture's twin copies of this frame, by the native copy they stand for.
struct TwinCopies
{
  std::vector<std::pair<const TCacheEntry*, RcTcacheEntry>> copies;
  RcTcacheEntry xfb;
  void Clear()
  {
    copies.clear();
    xfb.reset();
  }
  AbstractTexture* Find(const TCacheEntry* native) const
  {
    for (const auto& [key, twin] : copies)
    {
      if (key == native)
        return twin->texture.get();
    }
    return nullptr;
  }
};

struct PresentableTwins
{
  std::weak_ptr<TCacheEntry> native;  // the XFB copy they stand for
  std::vector<RcTcacheEntry> twins;
};

FrameRecord s_prev;
FrameRecord s_cur;
KeyIndex s_prev_index;
std::vector<int> s_partner;
FrameEvents s_frame;
std::array<TwinCopies, MAX_SLOTS> s_twins;
RcTcacheEntry s_xfb_native;  // this frame's XFB copy

bool s_frame_active = false;
bool s_frame_checked = false;
bool s_frame_enabled = false;
const char* s_unsafe = nullptr;
u32 s_slots = 0;
bool s_replaying = false;
u32 s_perspective_draws = 0;
u32 s_paired_draws = 0;

std::array<PresentableTwins, 4> s_presentable;
size_t s_presentable_next = 0;
u32 s_frames_since_twins = 1000;

// Checks: PPR_FI_WEIGHT=<w> draws every in-between picture at that weight (at 1 it must equal
// the frame's own picture bit for bit). PPR_FI_DUMP=<dir> saves, as PNG, each frame's own picture
// and its in-between pictures, for PPR_FI_DUMP_COUNT frames (4) from the PPR_FI_DUMP_FROM-th
// (600th) that has in-between pictures.
struct Diagnostics
{
  float weight = -1.0f;
  std::string dump_dir;
  u64 dump_from = 0;
  u64 dump_count = 0;
  Diagnostics()
  {
    if (const char* w = std::getenv("PPR_FI_WEIGHT"); w && *w)
      weight = static_cast<float>(std::atof(w));
    if (const char* d = std::getenv("PPR_FI_DUMP"); d && *d)
    {
      dump_dir = d;
      const char* from = std::getenv("PPR_FI_DUMP_FROM");
      const char* count = std::getenv("PPR_FI_DUMP_COUNT");
      dump_from = from ? std::strtoull(from, nullptr, 10) : 600;
      dump_count = count ? std::strtoull(count, nullptr, 10) : 4;
    }
  }
};
const Diagnostics& Diag()
{
  static const Diagnostics diagnostics;
  return diagnostics;
}
// Frames with in-between pictures so far (PPR_FI_DUMP_FROM counts them).
u64 s_dumpable_frames = 0;

// What in-between pictures need of the run, besides the session's word: the setting (`rate`), and
// a setup the record and the replay work on.
bool Supported(int rate, bool graphics_mods, bool no_vram_copies)
{
  if (SlotsForRate(rate) == 0)
    return false;
  if (!Core::System::GetInstance().IsDualCoreMode())
    return false;
  if (graphics_mods)
    return false;
  if (!g_backend_info.bSupportsCopyToVram || no_vram_copies)
    return false;
  if (!g_gfx || !g_gfx->SupportsUtilityDrawing() || !g_framebuffer_manager || !g_vertex_manager ||
      !g_texture_cache)
  {
    return false;
  }
  if (!g_video_backend || g_video_backend->GetConfigName() == "Null")
    return false;
  // Measured on Brawl's (Project+'s) frames only.
  return SConfig::GetInstance().GetGameID().starts_with("RSB");
}

// The session's word (SetAllowed): no in-between pictures without it.
std::atomic<bool> s_allowed{false};

bool EnabledNow()
{
  return s_allowed.load(std::memory_order_relaxed) &&
         Supported(g_ActiveConfig.iSmoothMotion, g_ActiveConfig.bGraphicMods,
                   g_ActiveConfig.bDisableCopyToVRAM);
}

void BeginFrame()
{
  s_frame_active = true;
  s_unsafe = nullptr;
  s_slots = ConfiguredSlots();
  s_cur.Clear();
  s_partner.clear();
  s_frame.Clear();
  s_xfb_native.reset();
  s_perspective_draws = 0;
  s_paired_draws = 0;
}

void EndFrame()
{
  s_frame_active = false;
  s_frame_checked = false;
  s_frame.Clear();
  for (auto& twins : s_twins)
    twins.Clear();
  s_xfb_native.reset();
}

u64 TextureIdentity(const TCacheEntry& entry)
{
  // An EFB copy is a new texture every frame; its address is what stays the same.
  if (entry.is_efb_copy || entry.is_xfb_copy)
    return 0xEFB0'0000'0000'0000ULL | entry.addr;
  return entry.base_hash ^ (static_cast<u64>(entry.addr) << 32);
}

void BuildBlendedConstants(const DrawEvent& draw, float weight, VertexShaderConstants* out)
{
  const DrawKey& key = s_cur.keys[draw.key];
  *out = s_cur.vs[key.vs];
  const int partner = s_partner.size() > draw.key ? s_partner[draw.key] : -1;
  if (partner < 0 || !key.perspective || weight >= 1.0f)
    return;

  const VertexShaderConstants& prev = s_prev.vs[s_prev.keys[partner].vs];
  const VertexShaderConstants& cur = s_cur.vs[key.vs];
  BlendLinear(prev.projection[0].data(), cur.projection[0].data(), 16, weight,
              out->projection[0].data());

  if (!key.per_vertex)
  {
    Mat34 p, c, o;
    std::memcpy(p.data(), prev.posnormalmatrix[0].data(), sizeof(p));
    std::memcpy(c.data(), cur.posnormalmatrix[0].data(), sizeof(c));
    BlendRigid(p, c, weight, &o);
    std::memcpy(out->posnormalmatrix[0].data(), o.data(), sizeof(o));
    BlendLinear(prev.posnormalmatrix[3].data(), cur.posnormalmatrix[3].data(), 12, weight,
                out->posnormalmatrix[3].data());
    return;
  }

  // A matrix palette (skinning): the coefficients, so that the seams between bones stay closed.
  for (u64 rows = key.used_rows; rows != 0; rows &= rows - 1)
  {
    const u32 row = static_cast<u32>(std::countr_zero(rows));
    if (row + 2 >= cur.transformmatrices.size())
      continue;
    BlendLinear(prev.transformmatrices[row].data(), cur.transformmatrices[row].data(), 12, weight,
                out->transformmatrices[row].data());
    const u32 normal = row & 31;
    if (normal + 2 < cur.normalmatrices.size())
    {
      BlendLinear(prev.normalmatrices[normal].data(), cur.normalmatrices[normal].data(), 12,
                  weight, out->normalmatrices[normal].data());
    }
  }
}

float SlotWeight(u32 slot, u32 slots)
{
  if (Diag().weight >= 0.0f)
    return Diag().weight;
  return static_cast<float>(slot + 1) / static_cast<float>(slots + 1);
}

void ReplayDraw(const DrawEvent& draw, u32 slot, float weight)
{
  VertexShaderConstants blended;
  BuildBlendedConstants(draw, weight, &blended);
  for (u32 unit = 0; unit < 8; ++unit)
  {
    if (!(draw.used_textures & (1u << unit)) || !draw.textures[unit])
      continue;
    // A copy made earlier in this frame: this picture's own.
    AbstractTexture* texture = s_twins[slot].Find(draw.textures[unit].get());
    g_gfx->SetTexture(unit, texture ? texture : draw.textures[unit]->texture.get());
    g_gfx->SetSamplerState(unit, draw.samplers[unit]);
  }
  const auto& v = draw.viewport;
  g_gfx->SetViewport(v.viewport_x, v.viewport_y, v.viewport_width, v.viewport_height,
                     v.viewport_near_depth, v.viewport_far_depth);
  g_gfx->SetScissorRect(v.scissor_rect);
  g_vertex_manager->ReplayInterpolatedDraw(
      draw.pipeline, draw.primitive, s_frame.vertices.data() + draw.vertex_offset,
      draw.vertex_stride, draw.num_vertices, s_frame.indices.data() + draw.index_offset,
      draw.num_indices, blended, s_frame.ps[draw.ps], s_frame.gs[draw.gs]);
}

// Draws the frame again into every in-between picture's EFB, with its copies. Returns false if a
// copy could not be made.
bool ReplayFrame()
{
  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  auto& pixel_shader_manager = system.GetPixelShaderManager();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();

  // The native draws' constants, put back after.
  const VertexShaderConstants saved_vs = vertex_shader_manager.constants;
  const PixelShaderConstants saved_ps = pixel_shader_manager.constants;
  const GeometryShaderConstants saved_gs = geometry_shader_manager.constants;

  bool complete = true;
  s_replaying = true;
  for (u32 slot = 0; slot < s_slots && complete; ++slot)
  {
    const float weight = SlotWeight(slot, s_slots);
    s_twins[slot].Clear();
    g_framebuffer_manager->SwapInterpolationEFB(slot);
    g_framebuffer_manager->BindEFBFramebuffer();
    for (const Event& event : s_frame.events)
    {
      if (const auto* draw = std::get_if<DrawEvent>(&event))
      {
        ReplayDraw(*draw, slot, weight);
      }
      else if (const auto* clear = std::get_if<ClearEvent>(&event))
      {
        g_framebuffer_manager->ClearEFB(clear->rc, clear->color_enable, clear->alpha_enable,
                                        clear->z_enable, clear->color, clear->z,
                                        clear->pixel_format);
      }
      else
      {
        const CopyEvent& copy = std::get<CopyEvent>(event);
        RcTcacheEntry twin = g_texture_cache->MakeEFBCopyTwin(*copy.native, copy.copy);
        if (!twin)
        {
          complete = false;
          break;
        }
        if (copy.copy.is_xfb_copy)
          s_twins[slot].xfb = std::move(twin);
        else
          s_twins[slot].copies.emplace_back(copy.native.get(), std::move(twin));
      }
    }
    g_framebuffer_manager->SwapInterpolationEFB(slot);
  }
  s_replaying = false;

  vertex_shader_manager.constants = saved_vs;
  pixel_shader_manager.constants = saved_ps;
  geometry_shader_manager.constants = saved_gs;
  vertex_shader_manager.dirty = true;
  pixel_shader_manager.dirty = true;
  geometry_shader_manager.dirty = true;
  g_framebuffer_manager->BindEFBFramebuffer();
  const auto& v = g_gfx->GetStoredViewportAndScissor();
  g_gfx->SetViewport(v.viewport_x, v.viewport_y, v.viewport_width, v.viewport_height,
                     v.viewport_near_depth, v.viewport_far_depth);
  g_gfx->SetScissorRect(v.scissor_rect);
  return complete;
}

void StorePresentable(const RcTcacheEntry& native, std::vector<RcTcacheEntry> twins)
{
  PresentableTwins& slot = s_presentable[s_presentable_next];
  s_presentable_next = (s_presentable_next + 1) % s_presentable.size();
  slot.native = native;
  slot.twins = std::move(twins);
}

void DumpPictures(const RcTcacheEntry& native, const std::vector<RcTcacheEntry>& twins)
{
  const Diagnostics& diag = Diag();
  const u64 frame = s_dumpable_frames++;
  if (diag.dump_dir.empty() || frame < diag.dump_from || frame >= diag.dump_from + diag.dump_count)
    return;
  native->texture->Save(fmt::format("{}/frame{:06}_native.png", diag.dump_dir, frame), 0);
  for (size_t i = 0; i < twins.size(); ++i)
    twins[i]->texture->Save(fmt::format("{}/frame{:06}_twin{}.png", diag.dump_dir, frame, i), 0);
}
}  // namespace

bool Available()
{
  return Supported(Config::Get(Config::ONLINE_SMOOTH_MOTION), Config::Get(Config::GFX_MODS_ENABLE),
                   Config::Get(Config::GFX_HACK_DISABLE_COPY_TO_VRAM));
}

void SetAllowed(bool allowed)
{
  s_allowed.store(allowed, std::memory_order_relaxed);
}

u32 ConfiguredSlots()
{
  return SlotsForRate(g_ActiveConfig.iSmoothMotion);
}

bool Recording()
{
  if (s_replaying)
    return false;
  if (s_frame_active)
    return true;
  if (!s_frame_checked)
  {
    s_frame_enabled = EnabledNow();
    s_frame_checked = true;
    // Nothing is recorded any more (the gate, a setting changed mid-game): frames present as
    // without it at once, not a part of a frame late for want of the count below going on.
    if (!s_frame_enabled)
      s_frames_since_twins = 1000;
  }
  if (!s_frame_enabled)
    return false;
  BeginFrame();
  return true;
}

void RecordDraw(const DrawInput& input)
{
  if (!Recording())
    return;
  if (input.bounding_box)
    MarkUnsafe("bounding box");

  auto& system = Core::System::GetInstance();
  const auto& vertex_shader_manager = system.GetVertexShaderManager();
  const auto& pixel_shader_manager = system.GetPixelShaderManager();
  const auto& geometry_shader_manager = system.GetGeometryShaderManager();

  DrawEvent draw;
  draw.pipeline = input.pipeline;
  draw.primitive = input.primitive;
  draw.vertex_stride = input.vertex_stride;
  draw.num_vertices = input.num_vertices;
  draw.num_indices = input.num_indices;
  draw.viewport = g_gfx->GetStoredViewportAndScissor();
  draw.used_textures = input.used_textures;

  // The bytes the GPU reads, kept for the replay.
  const size_t vertex_bytes = static_cast<size_t>(input.vertex_stride) * input.num_vertices;
  draw.vertex_offset = static_cast<u32>(s_frame.vertices.size());
  s_frame.vertices.insert(s_frame.vertices.end(), input.vertices, input.vertices + vertex_bytes);
  draw.index_offset = static_cast<u32>(s_frame.indices.size());
  s_frame.indices.insert(s_frame.indices.end(), input.indices, input.indices + input.num_indices);

  if (pixel_shader_manager.dirty || s_frame.ps.empty())
    s_frame.ps.push_back(pixel_shader_manager.constants);
  draw.ps = static_cast<u32>(s_frame.ps.size() - 1);
  if (geometry_shader_manager.dirty || s_frame.gs.empty())
    s_frame.gs.push_back(geometry_shader_manager.constants);
  draw.gs = static_cast<u32>(s_frame.gs.size() - 1);

  DrawKey key;
  if (vertex_shader_manager.dirty || s_cur.vs.empty())
    s_cur.vs.push_back(vertex_shader_manager.constants);
  key.vs = static_cast<u32>(s_cur.vs.size() - 1);
  const VertexShaderConstants& vs = s_cur.vs[key.vs];

  // The textures it samples, kept alive for the replay.
  std::array<u64, 16> exact{};
  size_t n = 0;
  exact[n++] = input.pipeline_hash;
  for (u32 unit = 0; unit < 8; ++unit)
  {
    if (!(input.used_textures & (1u << unit)))
      continue;
    const RcTcacheEntry& entry = g_texture_cache->GetBoundTexture(unit);
    if (!entry)
      continue;
    draw.textures[unit] = entry;
    s_frame.textures.insert(entry.get());
    draw.samplers[unit] = (*input.samplers)[unit];
    exact[n++] = TextureIdentity(*entry) + unit;
  }

  key.perspective = input.perspective;
  key.per_vertex = input.posmtx_per_vertex;
  // What the draw draws, to pair it by. A 2D draw is never paired: it needs no key.
  if (key.perspective)
  {
    // The viewport's size tells a pass into a smaller target (the shadow texture) from the
    // picture's own. Not its origin or the scissor: those may move with the draw.
    const float viewport[2] = {draw.viewport.viewport_width, draw.viewport.viewport_height};
    std::memcpy(&exact[n++], viewport, sizeof(u64));

    u64 topology = 0;
    if (key.per_vertex && input.vertex_stride != 0)
    {
      // The matrix row each vertex reads (PNMTXIDX): which palette slots, in which order.
      static std::vector<u8> rows_read;
      rows_read.clear();
      for (size_t offset = input.posmtx_offset; offset < vertex_bytes;
           offset += input.vertex_stride)
      {
        const u8 row = input.vertices[offset] & 0x3f;
        key.used_rows |= 1ULL << row;
        rows_read.push_back(row);
      }
      topology = XXH3_64bits(rows_read.data(), rows_read.size());

      u32 rows = 0;
      for (u64 bits = key.used_rows; bits != 0; bits &= bits - 1)
      {
        const u32 row = static_cast<u32>(std::countr_zero(bits));
        if (row + 2 >= vs.transformmatrices.size())
          continue;
        key.translation[0] += vs.transformmatrices[row][3];
        key.translation[1] += vs.transformmatrices[row + 1][3];
        key.translation[2] += vs.transformmatrices[row + 2][3];
        ++rows;
      }
      if (rows != 0)
      {
        for (float& t : key.translation)
          t /= static_cast<float>(rows);
      }
    }
    else
    {
      key.translation = {vs.posnormalmatrix[0][3], vs.posnormalmatrix[1][3],
                         vs.posnormalmatrix[2][3]};
    }

    exact[n++] = topology;
    exact[n++] = key.used_rows;
    key.stable = XXH3_64bits(exact.data(), n * sizeof(u64));
    exact[n++] = XXH3_64bits(input.vertices, vertex_bytes);
    exact[n++] = XXH3_64bits(input.indices, input.num_indices * sizeof(u16));
    key.exact = XXH3_64bits(exact.data(), n * sizeof(u64));
    // (The pipeline's hash is the first.)
    key.mesh = XXH3_64bits(exact.data() + 1, (n - 1) * sizeof(u64));
  }

  draw.key = static_cast<u32>(s_cur.keys.size());
  s_cur.keys.push_back(key);
  s_frame.events.emplace_back(std::move(draw));
}

void RecordClear(const MathUtil::Rectangle<int>& rc, bool color_enable, bool alpha_enable,
                 bool z_enable, u32 color, u32 z, PixelFormat pixel_format)
{
  if (!Recording())
    return;
  s_frame.events.emplace_back(
      ClearEvent{rc, color_enable, alpha_enable, z_enable, color, z, pixel_format});
}

void RecordCopy(const RcTcacheEntry& native, const EFBCopyReplay& copy)
{
  if (!native || !Recording())
    return;
  if (copy.is_xfb_copy)
    s_xfb_native = native;
  s_frame.textures.insert(native.get());
  s_frame.events.emplace_back(CopyEvent{native, copy});
}

void MarkUnsafe(const char* reason)
{
  if (s_replaying || !s_frame_active || s_unsafe)
    return;
  s_unsafe = reason;
}

void TextureChanged(const TCacheEntry* entry)
{
  if (s_frame_active && !s_replaying && s_frame.textures.contains(entry))
    MarkUnsafe("recorded texture changed");
}

void BeforeEFBCopy()
{
  // The batch being built is drawn before the copy, so it is part of what the copy holds.
  if (s_frame_active && !s_replaying)
    g_vertex_manager->Flush();
}

void AfterXFBCopy(bool presented)
{
  if (!s_frame_active)
  {
    s_frame_checked = false;
    return;
  }

  // A frame nobody sees (a rollback's re-run) needs no in-between picture, and is not what the
  // next frame moves on from.
  if (!presented)
  {
    EndFrame();
    return;
  }

  std::vector<RcTcacheEntry> twins;
  bool low_match = false;
  if (!s_unsafe && s_xfb_native)
  {
    MatchDraws(s_prev.keys, s_prev_index, &s_cur.keys, &s_partner);
    for (size_t i = 0; i < s_cur.keys.size(); ++i)
    {
      if (!s_cur.keys[i].perspective)
        continue;
      ++s_perspective_draws;
      if (s_partner[i] >= 0)
        ++s_paired_draws;
    }
    // Nothing paired (a 2D screen) or mostly new draws (a scene change, a camera cut): nothing
    // to move between.
    low_match = s_paired_draws == 0 ||
                (s_perspective_draws >= 8 && s_paired_draws * 4 < s_perspective_draws * 3);
    if (!low_match)
    {
      if (!g_framebuffer_manager->EnsureInterpolationEFBs(s_slots))
        MarkUnsafe("no EFB for the in-between pictures");
      else if (!ReplayFrame())
        MarkUnsafe("no texture for a copy");
    }
    if (!s_unsafe && !low_match)
    {
      for (u32 slot = 0; slot < s_slots; ++slot)
      {
        if (!s_twins[slot].xfb)
        {
          twins.clear();
          break;
        }
        twins.push_back(s_twins[slot].xfb);
      }
    }
  }

  if (!twins.empty())
  {
    DumpPictures(s_xfb_native, twins);
    StorePresentable(s_xfb_native, std::move(twins));
    s_frames_since_twins = 0;
  }
  else if (s_frames_since_twins < 1000)
  {
    ++s_frames_since_twins;
  }

  // The frame the player sees is the next frame's previous one.
  std::swap(s_prev, s_cur);
  s_prev_index.Build(s_prev.keys);
  EndFrame();
}

bool Replaying()
{
  return s_replaying;
}

std::vector<RcTcacheEntry> TakeXFBTwins(const TCacheEntry* entry)
{
  for (PresentableTwins& p : s_presentable)
  {
    if (entry && p.native.lock().get() == entry)
    {
      std::vector<RcTcacheEntry> twins = std::move(p.twins);
      p = {};
      return twins;
    }
  }
  return {};
}

bool PresentsDeferred()
{
  return s_allowed.load(std::memory_order_relaxed) && ConfiguredSlots() != 0 &&
         s_frames_since_twins < 30;
}

void DiscardFrame()
{
  if (s_replaying)
    return;
  EndFrame();
}

void Reset()
{
  if (s_replaying)
    return;
  EndFrame();
  s_prev.Clear();
  s_cur.Clear();
  s_prev_index.Clear();
  s_partner.clear();
  for (PresentableTwins& p : s_presentable)
    p = {};
  s_frames_since_twins = 1000;
}

void Shutdown()
{
  s_allowed = false;
  s_replaying = false;
  Reset();
  s_frame = {};
}
}  // namespace FrameInterpolation
