// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Smooth motion: in-between pictures by geometry-based frame interpolation (Brawl Online).
//
// The game still runs at 60 frames a second. The video thread records every GX draw, EFB clear and
// EFB copy of a frame (pipeline, textures, samplers, viewport, vertex and index bytes, shader
// constants). At the XFB copy of a frame that is presented, it pairs each perspective draw with the
// same draw of the previous presented frame by what it draws (pipeline, textures, vertex data,
// matrix-index layout; then without the pipeline, then without the vertex data) and draws the
// frame again, into an EFB of its own per in-between picture, with the position matrices and the
// projection blended between the two frames; each EFB copy is made again from that EFB into a twin
// texture, which the later draws sample. The XFB copy's twin is the in-between picture: the
// presenter shows it where the frame's own picture went before, and the frame's own picture a part
// of a frame later (Present.cpp). Draws without a partner and 2D draws are drawn as they are. A
// frame that is not presented (a rollback's re-run) is only recorded.
//
// Everything stays on the host: guest memory, the EFB the game can read, PE tokens and timing are
// untouched, so netplay, replays and rollback see no difference. Video thread only, except the
// config.

#include <array>
#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"

class AbstractPipeline;
struct EFBCopyReplay;
struct TCacheEntry;
using RcTcacheEntry = std::shared_ptr<TCacheEntry>;
enum class PixelFormat : u32;
enum class PrimitiveType : u32;
struct SamplerState;

namespace FrameInterpolation
{
// A rate is a multiple of the game's 60 frames a second, from 120 to MAX_RATE: 120, 180, 240, 300
// or 360 Hz, with one to MAX_SLOTS in-between pictures a frame. Anything else is off (0).
constexpr int MAX_RATE = 360;
constexpr u32 MAX_SLOTS = MAX_RATE / 60 - 1;
constexpr u32 SlotsForRate(int rate)
{
  return rate >= 120 && rate <= MAX_RATE && rate % 60 == 0 ? static_cast<u32>(rate / 60 - 1) : 0;
}
static_assert(SlotsForRate(120) == 1 && SlotsForRate(240) == 3 && SlotsForRate(360) == MAX_SLOTS &&
              SlotsForRate(0) == 0 && SlotsForRate(60) == 0 && SlotsForRate(144) == 0 &&
              SlotsForRate(420) == 0 && SlotsForRate(-120) == 0);

// ---- Math (no state; FrameInterpolationTest) ----

// A 3x4 row-major affine matrix, as XF memory and the vertex shader constants keep it.
using Mat34 = std::array<float, 12>;

// Blends two rigid transforms (rotation, per-axis scale, translation): normalized quaternion lerp
// for the rotation, linear for the rest. Returns false (and `out` = linear blend) when either
// matrix is not a rotation times a scale (shear, zero scale, non-finite) or the rotation turns more
// than 90 degrees between them.
bool BlendRigid(const Mat34& prev, const Mat34& cur, float weight, Mat34* out);
// Coefficient-wise blend of `count` floats, keeping `cur` where either side is not finite.
void BlendLinear(const float* prev, const float* cur, size_t count, float weight, float* out);

// ---- Matching (no GPU state; FrameInterpolationTest) ----

struct DrawKey
{
  // Keys of perspective draws only (2D draws are never paired).
  u64 exact = 0;   // pipeline, textures, vertex and index bytes, matrix rows, viewport size
  u64 mesh = 0;    // the same without the pipeline (a mesh whose material changes)
  u64 stable = 0;  // the same without the vertex and index bytes (meshes the CPU deforms)
  u64 used_rows = 0;  // per-vertex matrix draws: the matrix rows the vertices use
  bool per_vertex = false;
  bool perspective = false;
  std::array<float, 3> translation{};  // of the draw's (mean) position matrix
  std::array<float, 3> predicted{};    // where it would be next frame at the same speed
  bool has_prediction = false;
  u32 vs = 0;  // the draw's vertex shader constants (FrameInterpolation.cpp)
};

// The previous frame's draws by key.
struct KeyIndex
{
  std::unordered_map<u64, std::vector<u32>> exact;
  std::unordered_map<u64, std::vector<u32>> mesh;
  std::unordered_map<u64, std::vector<u32>> stable;
  void Build(const std::vector<DrawKey>& draws);
  void Clear();
};

// Translation (game units) a draw may move between two frames and still be paired.
constexpr float MATCH_GATE = 1500.0f;

// Pairs `cur` draws with `prev` draws: exact keys first, then mesh keys, then stable keys, within
// each key by the smallest distance to where the previous draw was (or was predicted to be), never
// across the translation gate. Each previous draw is used at most once. Writes the partner index
// into `partner` (one per cur draw, -1 for none) and sets the predictions of the paired `cur`
// draws.
void MatchDraws(const std::vector<DrawKey>& prev, const KeyIndex& prev_index,
               std::vector<DrawKey>* cur, std::vector<int>* partner);

// ---- Any thread ----

// Whether this run can draw in-between pictures: the setting is on, and the setup is one they work
// on (dual core, a backend with VRAM copies, no graphics mods, Brawl).
bool Available();
// In-between pictures are drawn only while the session allows them (Gprb::Session, which does in a
// match, where it plays a frame of input delay lower for them, and in a replay). Off until told
// otherwise, so nothing else gets them (menus, offline play, Other Games).
void SetAllowed(bool allowed);

// ---- Video thread ----

// The configured rate as in-between pictures per frame (SlotsForRate: 0-MAX_SLOTS).
u32 ConfiguredSlots();

// Recording: VertexManagerBase::RenderDrawCall, before the draw's uniforms are uploaded.
struct DrawInput
{
  const AbstractPipeline* pipeline = nullptr;
  u64 pipeline_hash = 0;
  PrimitiveType primitive{};
  const u8* vertices = nullptr;
  u32 vertex_stride = 0;
  u32 num_vertices = 0;
  const u16* indices = nullptr;
  u32 num_indices = 0;
  bool posmtx_per_vertex = false;
  u32 posmtx_offset = 0;
  bool perspective = false;
  u32 used_textures = 0;  // bit per texture unit
  const std::array<SamplerState, 8>* samplers = nullptr;
  bool bounding_box = false;
};
bool Recording();
void RecordDraw(const DrawInput& input);

// FramebufferManager::ClearEFB (not called while replaying).
void RecordClear(const MathUtil::Rectangle<int>& rc, bool color_enable, bool alpha_enable,
                 bool z_enable, u32 color, u32 z, PixelFormat pixel_format);

// Something in this frame is not replayed (an EFB format change, pokes, bounding box, an EFB copy
// that made no texture) or the replay failed (no texture for a copy): the frame gets no in-between
// pictures.
void MarkUnsafe(const char* reason);
// The texture cache is about to change or release a texture. A frame that recorded it cannot
// replay its original contents and gets no in-between pictures.
void TextureChanged(const TCacheEntry* entry);

// TextureCacheBase::CopyRenderTargetToTexture: before the copy (the batch being built goes into the
// record), and after it made `native` (the copy, to make again from each in-between picture's EFB).
void BeforeEFBCopy();
void RecordCopy(const RcTcacheEntry& native, const EFBCopyReplay& copy);
// BPStructs, after the XFB copy (the end of a frame), before it is presented: the in-between
// pictures, if it is `presented` (PresentStats::OnEFBCopy's decision; a rollback's re-run is not).
void AfterXFBCopy(bool presented);

// Inside a replay (draws, clears, copies): hooks record nothing and presents wait.
bool Replaying();

// Presenter: the in-between pictures of the XFB copy `entry` (empty if none), first to last.
std::vector<RcTcacheEntry> TakeXFBTwins(const TCacheEntry* entry);
// Whether the presenter shows each frame's own picture a part of a frame late: on, allowed, and
// in-between pictures were made recently.
bool PresentsDeferred();

// A rollback load: discard the recording, keeping the last presented frame to blend from.
void DiscardFrame();
// A savestate load: forget the recording and all presentation history.
void Reset();
// Releases every texture before the texture cache goes (VideoBackendBase::ShutdownShared).
void Shutdown();
}  // namespace FrameInterpolation
