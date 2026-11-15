// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/ShaderCache.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <xxh3.h>

#include "Common/Assert.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "Common/Version.h"
#include "Core/ConfigManager.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FramebufferShaderGen.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PipelineUtils.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

#include <imgui.h>

std::unique_ptr<VideoCommon::ShaderCache> g_shader_cache;

namespace VideoCommon
{
ShaderCache::ShaderCache() : m_api_type{APIType::Nothing}
{
}

ShaderCache::~ShaderCache()
{
  ClearCaches();
}

bool ShaderCache::Initialize()
{
  m_api_type = g_backend_info.api_type;
  m_host_config.bits = ShaderHostConfig::GetCurrent().bits;

  if (!CompileSharedPipelines())
    return false;

  m_async_shader_compiler = g_gfx->CreateAsyncShaderCompiler();
  m_frame_end_handler = GetVideoEvents().after_frame_event.Register(
      [this](Core::System&) { RetrieveAsyncShaders(); });
  return true;
}

void ShaderCache::InitializeShaderCache()
{
  m_async_shader_compiler->ResizeWorkerThreads(g_ActiveConfig.GetShaderPrecompilerThreads());

  // Load shader and UID caches.
  if (g_ActiveConfig.bShaderCache && m_api_type != APIType::Nothing)
  {
    LoadCaches();
    LoadPipelineUIDCache();
  }

  // Queue ubershader precompiling if required.
  if (g_ActiveConfig.UsingUberShaders())
    QueueUberShaderPipelines();

  // Compile all known UIDs.
  const size_t shaders_on_disk = GetSpecializedShaderCount();
  CompileMissingPipelines();
  m_compiling_missing_shaders = GetSpecializedShaderCount() > shaders_on_disk;
  if (g_ActiveConfig.bWaitForShadersBeforeStarting)
    WaitForAsyncCompiler();

  // Switch to the runtime shader compiler thread configuration.
  m_async_shader_compiler->ResizeWorkerThreads(g_ActiveConfig.GetShaderCompilerThreads());
}

void ShaderCache::Reload()
{
  WaitForAsyncCompiler();
  ClosePipelineUIDCache();
  ClearCaches();

  if (!CompileSharedPipelines())
    PanicAlertFmt("Failed to compile shared pipelines after reload.");

  if (g_ActiveConfig.bShaderCache)
    LoadCaches();

  // Switch to the precompiling shader configuration while we rebuild.
  m_async_shader_compiler->ResizeWorkerThreads(g_ActiveConfig.GetShaderPrecompilerThreads());

  // We don't need to explicitly recompile the individual ubershaders here, as the pipelines
  // UIDs are still be in the map. Therefore, when these are rebuilt, the shaders will also
  // be recompiled.
  const size_t shaders_on_disk = GetSpecializedShaderCount();
  CompileMissingPipelines();
  m_compiling_missing_shaders = GetSpecializedShaderCount() > shaders_on_disk;
  if (g_ActiveConfig.bWaitForShadersBeforeStarting)
    WaitForAsyncCompiler();
  m_async_shader_compiler->ResizeWorkerThreads(g_ActiveConfig.GetShaderCompilerThreads());
}

void ShaderCache::RetrieveAsyncShaders()
{
  m_async_shader_compiler->RetrieveWorkItems();
}

void ShaderCache::Shutdown()
{
  // This may leave shaders uncommitted to the cache, but it's better than blocking shutdown
  // until everything has finished compiling.
  if (m_async_shader_compiler)
  {
    m_async_shader_compiler->StopWorkerThreads();
    m_async_shader_compiler->ClearAllWork();
  }

  ClosePipelineUIDCache();
}

namespace
{
std::atomic<u64> s_sync_compile_count{0};
std::atomic<u64> s_sync_compile_us_total{0};
std::atomic<u64> s_sync_compile_us_max{0};
std::atomic<u64> s_sync_utility_compile_count{0};
std::atomic<u64> s_sync_utility_compile_us_total{0};

class SyncCompileTimer
{
public:
  // utility: EFB copy / texture conversion pipelines (compiled from source on first use, never
  // kept in the on-disk shader cache), as opposed to GX pipelines.
  explicit SyncCompileTimer(bool utility = false)
      : m_start(std::chrono::steady_clock::now()), m_utility(utility)
  {
  }
  ~SyncCompileTimer()
  {
    const u64 us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - m_start)
                                        .count());
    s_sync_compile_count.fetch_add(1, std::memory_order_relaxed);
    s_sync_compile_us_total.fetch_add(us, std::memory_order_relaxed);
    if (us > s_sync_compile_us_max.load(std::memory_order_relaxed))
      s_sync_compile_us_max.store(us, std::memory_order_relaxed);
    if (m_utility)
    {
      s_sync_utility_compile_count.fetch_add(1, std::memory_order_relaxed);
      s_sync_utility_compile_us_total.fetch_add(us, std::memory_order_relaxed);
    }
  }

private:
  std::chrono::steady_clock::time_point m_start;
  bool m_utility;
};
}  // namespace

SyncPipelineCompileStats GetSyncPipelineCompileStats()
{
  SyncPipelineCompileStats stats;
  stats.count = s_sync_compile_count.load(std::memory_order_relaxed);
  stats.us_total = s_sync_compile_us_total.load(std::memory_order_relaxed);
  stats.us_max = s_sync_compile_us_max.load(std::memory_order_relaxed);
  stats.utility_count = s_sync_utility_compile_count.load(std::memory_order_relaxed);
  stats.utility_us_total = s_sync_utility_compile_us_total.load(std::memory_order_relaxed);
  return stats;
}

const AbstractPipeline* ShaderCache::GetPipelineForUid(const GXPipelineUid& uid)
{
  auto it = m_gx_pipeline_cache.find(uid);
  if (it != m_gx_pipeline_cache.end() && !it->second.second)
    return it->second.first.get();

  SyncCompileTimer timer;
  const bool exists_in_cache = it != m_gx_pipeline_cache.end();
  std::unique_ptr<AbstractPipeline> pipeline;
  std::optional<AbstractPipelineConfig> pipeline_config = GetGXPipelineConfig(uid);
  if (pipeline_config)
    pipeline = g_gfx->CreatePipeline(*pipeline_config);
  if (g_ActiveConfig.bShaderCache && !exists_in_cache)
    AppendGXPipelineUID(uid);
  return InsertGXPipeline(uid, std::move(pipeline));
}

std::optional<const AbstractPipeline*> ShaderCache::GetPipelineForUidAsync(const GXPipelineUid& uid)
{
  auto it = m_gx_pipeline_cache.find(uid);
  if (it != m_gx_pipeline_cache.end())
  {
    // .second is the pending flag, i.e. compiling in the background.
    if (!it->second.second)
      return it->second.first.get();
    HurryPendingPipeline(uid, COMPILE_PRIORITY_ONDEMAND_PIPELINE);
    return {};
  }

  AppendGXPipelineUID(uid);
  QueuePipelineCompile(uid, COMPILE_PRIORITY_ONDEMAND_PIPELINE);
  return {};
}

const AbstractPipeline* ShaderCache::WaitForPipelineForUid(const GXPipelineUid& uid)
{
  // The shaders are still compiled and linked on the compiler threads, as every other GX
  // pipeline in these modes; only the GPU thread waits. Compiling them here instead
  // (GetPipelineForUid, 0bf79323) put the same shaders on the GPU thread's GL context while the
  // compiler threads' shared contexts were compiling them, which Mesa's OpenGL did not survive:
  // "Failed to link shaders: fragment shader lacks `main'" as the name tags rendered, or the
  // playback Dolphin crashed there (bug reports #131, #132; Linux, radeonsi).
  auto found = m_gx_pipeline_cache.find(uid);
  if (found != m_gx_pipeline_cache.end() && !found->second.second)
    return found->second.first.get();
  if (found == m_gx_pipeline_cache.end())
  {
    AppendGXPipelineUID(uid);
    QueuePipelineCompile(uid, COMPILE_PRIORITY_WAITED_PIPELINE);
  }
  else
  {
    HurryPendingPipeline(uid, COMPILE_PRIORITY_WAITED_PIPELINE);
  }
  // After a wait that ran out, the next few seconds' draws don't wait (they are skipped as any
  // other), so a compile that hangs stalls the GPU thread once, not at every draw.
  if (std::chrono::steady_clock::now() < m_no_pipeline_wait_until)
    return nullptr;

  SyncCompileTimer timer;  // counted with the synchronous compiles: the GPU thread stalls alike
  constexpr auto TIMEOUT = std::chrono::seconds(2);
  const auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
  for (;;)
  {
    // Also re-queues a pipeline whose shaders were not compiled yet (PipelineWorkItem).
    m_async_shader_compiler->RetrieveWorkItems();
    auto it = m_gx_pipeline_cache.find(uid);
    if (it != m_gx_pipeline_cache.end() && !it->second.second)
      return it->second.first.get();
    if (std::chrono::steady_clock::now() >= deadline)
    {
      WARN_LOG_FMT(VIDEO, "A draw that must not be skipped waited {} s for its shaders: skipped",
                   TIMEOUT.count());
      m_no_pipeline_wait_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      return nullptr;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void ShaderCache::NoteSkippedDraw()
{
  // After an update the menus show within a second (HurryPendingPipeline), but that second is
  // white, and draws are left out here and there until the rest has compiled: the message says
  // why, and goes about 1.5 s after the last draw left out.
  if (!m_compiling_missing_shaders)
    return;
  const auto now = std::chrono::steady_clock::now();
  if (now < m_next_compiling_message)
    return;
  m_next_compiling_message = now + std::chrono::milliseconds(500);
  if (!m_async_shader_compiler->HasPendingWorkFrom(COMPILE_PRIORITY_SHADERCACHE_PIPELINE))
  {
    m_compiling_missing_shaders = false;
    return;
  }
  OSD::AddTypedMessage(OSD::MessageType::CompilingShaders, "Compiling shaders...", 1500);
}

const AbstractPipeline* ShaderCache::GetUberPipelineForUid(const GXUberPipelineUid& uid)
{
  auto it = m_gx_uber_pipeline_cache.find(uid);
  if (it != m_gx_uber_pipeline_cache.end() && !it->second.second)
    return it->second.first.get();

  SyncCompileTimer timer;
  std::unique_ptr<AbstractPipeline> pipeline;
  std::optional<AbstractPipelineConfig> pipeline_config = GetGXPipelineConfig(uid);
  if (pipeline_config)
    pipeline = g_gfx->CreatePipeline(*pipeline_config);
  return InsertGXUberPipeline(uid, std::move(pipeline));
}

void ShaderCache::WaitForAsyncCompiler()
{
  bool running = true;

  constexpr auto update_ui_progress = [](size_t completed, size_t total) {
    const float center_x = ImGui::GetIO().DisplaySize.x * 0.5f;
    const float center_y = ImGui::GetIO().DisplaySize.y * 0.5f;
    const float scale = ImGui::GetIO().DisplayFramebufferScale.x;

    ImGui::SetNextWindowSize(ImVec2(400.0f * scale, 50.0f * scale), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(center_x, center_y), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin(Common::GetStringT("Compiling Shaders").c_str(), nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing))
    {
      ImGui::Text("Compiling shaders: %zu/%zu", completed, total);
      ImGui::ProgressBar(static_cast<float>(completed) /
                             static_cast<float>(std::max(total, static_cast<size_t>(1))),
                         ImVec2(-1.0f, 0.0f), "");
    }
    ImGui::End();

    g_presenter->Present();
  };

  while (running &&
         (m_async_shader_compiler->HasPendingWork() || m_async_shader_compiler->HasCompletedWork()))
  {
    running = m_async_shader_compiler->WaitUntilCompletion(update_ui_progress);

    m_async_shader_compiler->RetrieveWorkItems();
  }

  // An extra Present to clear the screen
  g_presenter->Present();
}

// Fingerprints. Each disk cache entry starts with FINGERPRINTED_ENTRY_MAGIC and the fingerprint
// of what it was made from, then the shader binary or the pipeline's cache data. A file's header
// holds the git revision of the build that wrote it (LinearDiskCache), which every update changes,
// though most updates change no shader: the first boot after each one compiled every shader
// again (GitHub issue #50; the white menus of c8c89fd7). A file of another build is read entry by
// entry instead: a shader is kept if the source this build generates for its UID, with the same
// compiler code (Common::GetShaderCompilerRevStr: the backends, glslang, SPIRV-Cross; any change
// there compiles everything again), has the same fingerprint, and a pipeline if its shaders and
// state do. The file is then written again with this build's header and the kept entries. A file
// of this build is trusted as before. Entries without the magic (written before fingerprints)
// are never used.
namespace
{
constexpr u32 FINGERPRINTED_ENTRY_MAGIC = 0x31504653;  // "SFP1"
constexpr size_t FINGERPRINT_PREFIX_SIZE = sizeof(u32) + 2 * sizeof(u64);

std::vector<u8> MakeFingerprintedEntry(const ShaderCacheFingerprint& fingerprint, const u8* data,
                                       size_t size)
{
  std::vector<u8> entry(FINGERPRINT_PREFIX_SIZE + size);
  std::memcpy(entry.data(), &FINGERPRINTED_ENTRY_MAGIC, sizeof(u32));
  std::memcpy(entry.data() + sizeof(u32), &fingerprint.low, sizeof(u64));
  std::memcpy(entry.data() + sizeof(u32) + sizeof(u64), &fingerprint.high, sizeof(u64));
  if (size != 0)
    std::memcpy(entry.data() + FINGERPRINT_PREFIX_SIZE, data, size);
  return entry;
}

struct FingerprintedEntry
{
  ShaderCacheFingerprint fingerprint;
  const u8* data;
  u32 size;
};

std::optional<FingerprintedEntry> ParseFingerprintedEntry(const u8* value, u32 value_size)
{
  u32 magic = 0;
  if (value_size < FINGERPRINT_PREFIX_SIZE)
    return std::nullopt;
  std::memcpy(&magic, value, sizeof(u32));
  if (magic != FINGERPRINTED_ENTRY_MAGIC)
    return std::nullopt;
  FingerprintedEntry entry;
  std::memcpy(&entry.fingerprint.low, value + sizeof(u32), sizeof(u64));
  std::memcpy(&entry.fingerprint.high, value + sizeof(u32) + sizeof(u64), sizeof(u64));
  entry.data = value + FINGERPRINT_PREFIX_SIZE;
  entry.size = value_size - static_cast<u32>(FINGERPRINT_PREFIX_SIZE);
  return entry;
}

class FingerprintHasher
{
public:
  // kind names what is hashed and how: change it with what a fingerprint covers.
  FingerprintHasher(std::string_view kind, APIType api_type)
  {
    XXH3_INITSTATE(&m_state);
    XXH3_128bits_reset(&m_state);
    AddString(kind);
    AddString(Common::GetShaderCompilerRevStr());
    AddValue(api_type);
  }

  void AddString(std::string_view str)
  {
    AddValue(static_cast<u64>(str.size()));
    XXH3_128bits_update(&m_state, str.data(), str.size());
  }

  template <typename T>
  void AddValue(const T& value)
  {
    static_assert(std::is_trivially_copyable_v<T>);
    XXH3_128bits_update(&m_state, &value, sizeof(T));
  }

  ShaderCacheFingerprint Finish()
  {
    const XXH128_hash_t hash = XXH3_128bits_digest(&m_state);
    return {hash.low64, hash.high64};
  }

private:
  XXH3_state_t m_state;
};

template <typename Uid>
void AppendShader(Common::LinearDiskCache<Uid, u8>& disk_cache, const Uid& uid,
                  const AbstractShader& shader, const ShaderCacheFingerprint& fingerprint)
{
  const AbstractShader::BinaryData binary = shader.GetBinary();
  if (binary.empty())
    return;
  const std::vector<u8> entry = MakeFingerprintedEntry(fingerprint, binary.data(), binary.size());
  disk_cache.Append(uid, entry.data(), static_cast<u32>(entry.size()));
}

template <typename Cache, typename Uid>
bool HasCompiledShader(const Cache& cache, const Uid& uid)
{
  const auto it = cache.shader_map.find(uid);
  return it != cache.shader_map.end() && !it->second.pending && it->second.shader;
}

template <typename Cache, typename Uid>
std::optional<ShaderCacheFingerprint> FindShaderFingerprint(const Cache& cache, const Uid& uid)
{
  if (!HasCompiledShader(cache, uid))
    return std::nullopt;
  return cache.shader_map.find(uid)->second.fingerprint;
}

std::optional<ShaderCacheFingerprint>
HashPipeline(APIType api_type, const AbstractPipelineConfig& config,
             const std::optional<ShaderCacheFingerprint>& vs, bool needs_gs,
             const std::optional<ShaderCacheFingerprint>& gs,
             const std::optional<ShaderCacheFingerprint>& ps)
{
  if (!vs || !ps || (needs_gs && !gs))
    return std::nullopt;
  FingerprintHasher hasher("GX pipeline 1", api_type);
  hasher.AddValue(config.usage);
  hasher.AddValue(*vs);
  hasher.AddValue(needs_gs);
  if (needs_gs)
    hasher.AddValue(*gs);
  hasher.AddValue(*ps);
  hasher.AddValue(config.rasterization_state.hex);
  hasher.AddValue(config.depth_state.hex);
  hasher.AddValue(config.blending_state.hex);
  hasher.AddValue(config.framebuffer_state.hex);
  hasher.AddValue(config.vertex_format != nullptr);
  if (config.vertex_format)
    hasher.AddValue(config.vertex_format->GetVertexDeclaration());
  return hasher.Finish();
}
}  // namespace

template <typename SerializedUidType, typename UidType>
static void SerializePipelineUid(const UidType& uid, SerializedUidType& serialized_uid)
{
  // Convert to disk format. Ensure all padding bytes are zero.
  std::memset(reinterpret_cast<u8*>(&serialized_uid), 0, sizeof(serialized_uid));
  serialized_uid.vertex_decl = uid.vertex_format->GetVertexDeclaration();
  serialized_uid.vs_uid = uid.vs_uid;
  serialized_uid.gs_uid = uid.gs_uid;
  serialized_uid.ps_uid = uid.ps_uid;
  serialized_uid.rasterization_state_bits = uid.rasterization_state.hex;
  serialized_uid.depth_state_bits = uid.depth_state.hex;
  serialized_uid.blending_state_bits = uid.blending_state.hex;
}

template <typename UidType, typename SerializedUidType>
static void UnserializePipelineUid(const SerializedUidType& uid, UidType& real_uid)
{
  real_uid.vertex_format = VertexLoaderManager::GetOrCreateMatchingFormat(uid.vertex_decl);
  real_uid.vs_uid = uid.vs_uid;
  real_uid.gs_uid = uid.gs_uid;
  real_uid.ps_uid = uid.ps_uid;
  real_uid.rasterization_state.hex = uid.rasterization_state_bits;
  real_uid.depth_state.hex = uid.depth_state_bits;
  real_uid.blending_state.hex = uid.blending_state_bits;
}

template <ShaderStage stage, typename K, typename T>
void ShaderCache::LoadShaderCache(T& cache, APIType api_type, const char* type, bool include_gameid)
{
  class CacheReader : public Common::LinearDiskCacheReader<K, u8>
  {
  public:
    CacheReader(const ShaderCache* this_ptr_, T& cache_) : this_ptr(this_ptr_), cache(cache_) {}
    void Read(const K& key, const u8* value, u32 value_size) override
    {
      Load(key, value, value_size, false);
    }
    bool ReadFromOtherBuild(const K& key, const u8* value, u32 value_size) override
    {
      ++other_build_entries;
      return Load(key, value, value_size, true);
    }

    u32 other_build_entries = 0;

  private:
    bool Load(const K& key, const u8* value, u32 value_size, bool other_build)
    {
      const std::optional<FingerprintedEntry> entry = ParseFingerprintedEntry(value, value_size);
      if (!entry)
        return false;
      // Another build's: only if this build makes the same shader for the UID.
      if (other_build &&
          this_ptr->GetShaderFingerprint(this_ptr->GetShaderSource(key)) != entry->fingerprint)
      {
        return false;
      }
      const auto found = cache.shader_map.find(key);
      if (found != cache.shader_map.end() && found->second.shader)
        return false;
      auto shader = g_gfx->CreateShaderFromBinary(stage, entry->data, entry->size);
      if (!shader)
        return false;

      auto& map_entry = cache.shader_map[key];
      map_entry.shader = std::move(shader);
      map_entry.pending = false;
      map_entry.fingerprint = entry->fingerprint;

      switch (stage)
      {
      case ShaderStage::Vertex:
        INCSTAT(g_stats.num_vertex_shaders_created);
        INCSTAT(g_stats.num_vertex_shaders_alive);
        break;
      case ShaderStage::Pixel:
        INCSTAT(g_stats.num_pixel_shaders_created);
        INCSTAT(g_stats.num_pixel_shaders_alive);
        break;
      default:
        break;
      }
      return true;
    }

    const ShaderCache* this_ptr;
    T& cache;
  };

  std::string filename = GetDiskShaderCacheFileName(api_type, type, include_gameid, true);
  CacheReader reader(this, cache);
  u32 count = cache.disk_cache.OpenAndRead(filename, reader);
  if (reader.other_build_entries != 0)
  {
    INFO_LOG_FMT(VIDEO, "Kept {} of {} shaders another build cached in {}", count,
                 reader.other_build_entries, filename);
  }
  INFO_LOG_FMT(VIDEO, "Loaded {} cached shaders from {}", count, filename);
}

template <typename T>
void ShaderCache::ClearShaderCache(T& cache)
{
  cache.disk_cache.Sync();
  cache.disk_cache.Close();
  cache.shader_map.clear();
}

template <typename KeyType, typename DiskKeyType, typename T>
void ShaderCache::LoadPipelineCache(T& cache, Common::LinearDiskCache<DiskKeyType, u8>& disk_cache,
                                    APIType api_type, const char* type, bool include_gameid)
{
  class CacheReader : public Common::LinearDiskCacheReader<DiskKeyType, u8>
  {
  public:
    CacheReader(ShaderCache* this_ptr_, T& cache_) : this_ptr(this_ptr_), cache(cache_) {}
    bool AnyFailed() const { return failed; }
    void Read(const DiskKeyType& key, const u8* value, u32 value_size) override
    {
      Load(key, value, value_size, false);
    }
    bool ReadFromOtherBuild(const DiskKeyType& key, const u8* value, u32 value_size) override
    {
      ++other_build_entries;
      return Load(key, value, value_size, true);
    }

    u32 other_build_entries = 0;

  private:
    bool Load(const DiskKeyType& key, const u8* value, u32 value_size, bool other_build)
    {
      const std::optional<FingerprintedEntry> entry = ParseFingerprintedEntry(value, value_size);
      if (!entry)
        return false;

      KeyType real_uid;
      UnserializePipelineUid(key, real_uid);

      // Skip those which are already compiled.
      if (failed || cache.contains(real_uid))
        return false;

      // Another build's: not if a shader it needs wasn't kept (GetGXPipelineConfig would compile
      // it here, while the game waits to start).
      if (other_build && g_backend_info.bSupportsShaderBinaries &&
          !this_ptr->HasPipelineShaders(real_uid))
      {
        return false;
      }

      auto config = this_ptr->GetGXPipelineConfig(real_uid);
      if (!config)
        return false;

      // Another build's: only if this build makes the same shaders and state for the UID.
      if (other_build && this_ptr->GetPipelineFingerprint(real_uid, *config) != entry->fingerprint)
        return false;

      auto pipeline = g_gfx->CreatePipeline(*config, entry->data, entry->size);
      if (!pipeline)
      {
        // If any of the pipelines fail to create, consider the cache stale.
        failed = true;
        return false;
      }

      auto& map_entry = cache[real_uid];
      map_entry.first = std::move(pipeline);
      map_entry.second = false;
      return true;
    }

    ShaderCache* this_ptr;
    T& cache;
    bool failed = false;
  };

  std::string filename = GetDiskShaderCacheFileName(api_type, type, include_gameid, true);
  CacheReader reader(this, cache);
  const u32 count = disk_cache.OpenAndRead(filename, reader);
  if (reader.other_build_entries != 0)
  {
    INFO_LOG_FMT(VIDEO, "Kept {} of {} pipelines another build cached in {}", count,
                 reader.other_build_entries, filename);
  }
  INFO_LOG_FMT(VIDEO, "Loaded {} cached pipelines from {}", count, filename);

  // If any of the pipelines in the cache failed to create, it's likely because of a change of
  // driver version, or system configuration. In this case, when the UID cache picks up the pipeline
  // later on, we'll write a duplicate entry to the pipeline cache. There's also no point in keeping
  // the old cache data around, so discard and recreate the disk cache.
  if (reader.AnyFailed())
  {
    WARN_LOG_FMT(VIDEO, "Failed to load one or more pipelines from cache '{}'. Discarding.",
                 filename);
    disk_cache.Close();
    File::Delete(filename);
    disk_cache.OpenAndRead(filename, reader);
  }
}

template <typename T, typename Y>
void ShaderCache::ClearPipelineCache(T& cache, Y& disk_cache)
{
  disk_cache.Sync();
  disk_cache.Close();

  // Set the pending flag to false, and destroy the pipeline.
  for (auto& it : cache)
  {
    it.second.first.reset();
    it.second.second = false;
  }
}

void ShaderCache::LoadCaches()
{
  // Ubershader caches, if present.
  if (g_backend_info.bSupportsShaderBinaries)
  {
    LoadShaderCache<ShaderStage::Vertex, UberShader::VertexShaderUid>(m_uber_vs_cache, m_api_type,
                                                                      "uber-vs", false);
    LoadShaderCache<ShaderStage::Pixel, UberShader::PixelShaderUid>(m_uber_ps_cache, m_api_type,
                                                                    "uber-ps", false);

    // We also share geometry shaders, as there aren't many variants.
    if (m_host_config.backend_geometry_shaders)
      LoadShaderCache<ShaderStage::Geometry, GeometryShaderUid>(m_gs_cache, m_api_type, "gs",
                                                                false);

    // Specialized shaders, gameid-specific.
    LoadShaderCache<ShaderStage::Vertex, VertexShaderUid>(m_vs_cache, m_api_type, "specialized-vs",
                                                          true);
    LoadShaderCache<ShaderStage::Pixel, PixelShaderUid>(m_ps_cache, m_api_type, "specialized-ps",
                                                        true);
  }

  if (g_backend_info.bSupportsPipelineCacheData)
  {
    LoadPipelineCache<GXPipelineUid, SerializedGXPipelineUid>(
        m_gx_pipeline_cache, m_gx_pipeline_disk_cache, m_api_type, "specialized-pipeline", true);
    LoadPipelineCache<GXUberPipelineUid, SerializedGXUberPipelineUid>(
        m_gx_uber_pipeline_cache, m_gx_uber_pipeline_disk_cache, m_api_type, "uber-pipeline",
        false);
  }
}

void ShaderCache::ClearCaches()
{
  ClearPipelineCache(m_gx_pipeline_cache, m_gx_pipeline_disk_cache);
  m_gx_pipeline_queued_priority.clear();
  ClearShaderCache(m_vs_cache);
  ClearShaderCache(m_gs_cache);
  ClearShaderCache(m_ps_cache);

  ClearPipelineCache(m_gx_uber_pipeline_cache, m_gx_uber_pipeline_disk_cache);
  ClearShaderCache(m_uber_vs_cache);
  ClearShaderCache(m_uber_ps_cache);

  m_screen_quad_vertex_shader.reset();
  m_texture_copy_vertex_shader.reset();
  m_efb_copy_vertex_shader.reset();
  m_texcoord_geometry_shader.reset();
  m_color_geometry_shader.reset();
  m_texture_copy_pixel_shader.reset();
  m_color_pixel_shader.reset();

  m_efb_copy_to_vram_pipelines.clear();
  m_efb_copy_to_ram_pipelines.clear();
  m_copy_rgba8_pipeline.reset();
  m_rgba8_stereo_copy_pipeline.reset();
  for (auto& pipeline : m_palette_conversion_pipelines)
    pipeline.reset();
  m_texture_reinterpret_pipelines.clear();
  m_texture_decoding_shaders.clear();

  SETSTAT(g_stats.num_pixel_shaders_created, 0);
  SETSTAT(g_stats.num_pixel_shaders_alive, 0);
  SETSTAT(g_stats.num_vertex_shaders_created, 0);
  SETSTAT(g_stats.num_vertex_shaders_alive, 0);
}

size_t ShaderCache::GetSpecializedShaderCount() const
{
  return m_vs_cache.shader_map.size() + m_ps_cache.shader_map.size();
}

void ShaderCache::CompileMissingPipelines()
{
  // Queue all uids with a null pipeline for compilation.
  for (auto& it : m_gx_pipeline_cache)
  {
    if (!it.second.first)
      QueuePipelineCompile(it.first, COMPILE_PRIORITY_SHADERCACHE_PIPELINE);
  }
  for (auto& it : m_gx_uber_pipeline_cache)
  {
    if (!it.second.first)
      QueueUberPipelineCompile(it.first, COMPILE_PRIORITY_UBERSHADER_PIPELINE);
  }
}

ShaderCache::ShaderSource ShaderCache::GetShaderSource(const VertexShaderUid& uid) const
{
  return {ShaderStage::Vertex,
          GenerateVertexShaderCode(m_api_type, m_host_config, uid.GetUidData(), {}).GetBuffer(),
          {}};
}

ShaderCache::ShaderSource
ShaderCache::GetShaderSource(const UberShader::VertexShaderUid& uid) const
{
  return {ShaderStage::Vertex,
          UberShader::GenVertexShader(m_api_type, m_host_config, uid.GetUidData()).GetBuffer(),
          fmt::to_string(*uid.GetUidData())};
}

ShaderCache::ShaderSource ShaderCache::GetShaderSource(const PixelShaderUid& uid) const
{
  return {ShaderStage::Pixel,
          GeneratePixelShaderCode(m_api_type, m_host_config, uid.GetUidData(), {}).GetBuffer(),
          {}};
}

ShaderCache::ShaderSource ShaderCache::GetShaderSource(const UberShader::PixelShaderUid& uid) const
{
  return {ShaderStage::Pixel,
          UberShader::GenPixelShader(m_api_type, m_host_config, uid.GetUidData()).GetBuffer(),
          fmt::to_string(*uid.GetUidData())};
}

ShaderCache::ShaderSource ShaderCache::GetShaderSource(const GeometryShaderUid& uid) const
{
  return {ShaderStage::Geometry,
          GenerateGeometryShaderCode(m_api_type, m_host_config, uid.GetUidData()).GetBuffer(),
          fmt::format("Geometry shader: {}", *uid.GetUidData())};
}

ShaderCacheFingerprint ShaderCache::GetShaderFingerprint(const ShaderSource& source) const
{
  FingerprintHasher hasher("GX shader 1", m_api_type);
  hasher.AddValue(source.stage);
  hasher.AddString(source.name);
  hasher.AddString(source.code);
  return hasher.Finish();
}

ShaderCache::CompiledShader ShaderCache::CompileShader(const ShaderSource& source) const
{
  return {g_gfx->CreateShaderFromSource(source.stage, source.code, nullptr, source.name),
          GetShaderFingerprint(source)};
}

ShaderCache::CompiledShader ShaderCache::CompileVertexShader(const VertexShaderUid& uid) const
{
  return CompileShader(GetShaderSource(uid));
}

ShaderCache::CompiledShader
ShaderCache::CompileVertexUberShader(const UberShader::VertexShaderUid& uid) const
{
  return CompileShader(GetShaderSource(uid));
}

ShaderCache::CompiledShader ShaderCache::CompilePixelShader(const PixelShaderUid& uid) const
{
  return CompileShader(GetShaderSource(uid));
}

ShaderCache::CompiledShader
ShaderCache::CompilePixelUberShader(const UberShader::PixelShaderUid& uid) const
{
  return CompileShader(GetShaderSource(uid));
}

const AbstractShader* ShaderCache::InsertVertexShader(const VertexShaderUid& uid,
                                                      CompiledShader shader)
{
  auto& entry = m_vs_cache.shader_map[uid];
  entry.pending = false;

  if (shader.shader && !entry.shader)
  {
    if (g_ActiveConfig.bShaderCache && g_backend_info.bSupportsShaderBinaries)
      AppendShader(m_vs_cache.disk_cache, uid, *shader.shader, shader.fingerprint);
    INCSTAT(g_stats.num_vertex_shaders_created);
    INCSTAT(g_stats.num_vertex_shaders_alive);
    entry.shader = std::move(shader.shader);
    entry.fingerprint = shader.fingerprint;
  }

  return entry.shader.get();
}

const AbstractShader* ShaderCache::InsertVertexUberShader(const UberShader::VertexShaderUid& uid,
                                                          CompiledShader shader)
{
  auto& entry = m_uber_vs_cache.shader_map[uid];
  entry.pending = false;

  if (shader.shader && !entry.shader)
  {
    if (g_ActiveConfig.bShaderCache && g_backend_info.bSupportsShaderBinaries)
      AppendShader(m_uber_vs_cache.disk_cache, uid, *shader.shader, shader.fingerprint);
    INCSTAT(g_stats.num_vertex_shaders_created);
    INCSTAT(g_stats.num_vertex_shaders_alive);
    entry.shader = std::move(shader.shader);
    entry.fingerprint = shader.fingerprint;
  }

  return entry.shader.get();
}

const AbstractShader* ShaderCache::InsertPixelShader(const PixelShaderUid& uid,
                                                     CompiledShader shader)
{
  auto& entry = m_ps_cache.shader_map[uid];
  entry.pending = false;

  if (shader.shader && !entry.shader)
  {
    if (g_ActiveConfig.bShaderCache && g_backend_info.bSupportsShaderBinaries)
      AppendShader(m_ps_cache.disk_cache, uid, *shader.shader, shader.fingerprint);
    INCSTAT(g_stats.num_pixel_shaders_created);
    INCSTAT(g_stats.num_pixel_shaders_alive);
    entry.shader = std::move(shader.shader);
    entry.fingerprint = shader.fingerprint;
  }

  return entry.shader.get();
}

const AbstractShader* ShaderCache::InsertPixelUberShader(const UberShader::PixelShaderUid& uid,
                                                         CompiledShader shader)
{
  auto& entry = m_uber_ps_cache.shader_map[uid];
  entry.pending = false;

  if (shader.shader && !entry.shader)
  {
    if (g_ActiveConfig.bShaderCache && g_backend_info.bSupportsShaderBinaries)
      AppendShader(m_uber_ps_cache.disk_cache, uid, *shader.shader, shader.fingerprint);
    INCSTAT(g_stats.num_pixel_shaders_created);
    INCSTAT(g_stats.num_pixel_shaders_alive);
    entry.shader = std::move(shader.shader);
    entry.fingerprint = shader.fingerprint;
  }

  return entry.shader.get();
}

const AbstractShader* ShaderCache::CreateGeometryShader(const GeometryShaderUid& uid)
{
  CompiledShader shader = CompileShader(GetShaderSource(uid));

  auto& entry = m_gs_cache.shader_map[uid];
  entry.pending = false;

  if (shader.shader && !entry.shader)
  {
    if (g_ActiveConfig.bShaderCache && g_backend_info.bSupportsShaderBinaries)
      AppendShader(m_gs_cache.disk_cache, uid, *shader.shader, shader.fingerprint);
    entry.shader = std::move(shader.shader);
    entry.fingerprint = shader.fingerprint;
  }

  return entry.shader.get();
}

bool ShaderCache::NeedsGeometryShader(const GeometryShaderUid& uid) const
{
  return m_host_config.backend_geometry_shaders && !uid.GetUidData()->IsPassthrough();
}

bool ShaderCache::UseGeometryShaderForEFBCopies() const
{
  return m_host_config.backend_geometry_shaders && m_host_config.stereo;
}

AbstractPipelineConfig ShaderCache::GetGXPipelineConfig(
    const NativeVertexFormat* vertex_format, const AbstractShader* vertex_shader,
    const AbstractShader* geometry_shader, const AbstractShader* pixel_shader,
    const RasterizationState& rasterization_state, const DepthState& depth_state,
    const BlendingState& blending_state, AbstractPipelineUsage usage)
{
  AbstractPipelineConfig config = {};
  config.usage = usage;
  config.vertex_format = vertex_format;
  config.vertex_shader = vertex_shader;
  config.geometry_shader = geometry_shader;
  config.pixel_shader = pixel_shader;
  config.rasterization_state = rasterization_state;
  config.depth_state = depth_state;
  config.blending_state = blending_state;
  config.framebuffer_state = g_framebuffer_manager->GetEFBFramebufferState();
  return config;
}

std::optional<AbstractPipelineConfig>
ShaderCache::GetGXPipelineConfig(const GXPipelineUid& config_in)
{
  GXPipelineUid config = VideoCommon::ApplyDriverBugs(config_in);
  const AbstractShader* vs;
  auto vs_iter = m_vs_cache.shader_map.find(config.vs_uid);
  if (vs_iter != m_vs_cache.shader_map.end() && !vs_iter->second.pending)
    vs = vs_iter->second.shader.get();
  else
    vs = InsertVertexShader(config.vs_uid, CompileVertexShader(config.vs_uid));

  PixelShaderUid ps_uid = config.ps_uid;
  ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);

  const AbstractShader* ps;
  auto ps_iter = m_ps_cache.shader_map.find(ps_uid);
  if (ps_iter != m_ps_cache.shader_map.end() && !ps_iter->second.pending)
    ps = ps_iter->second.shader.get();
  else
    ps = InsertPixelShader(ps_uid, CompilePixelShader(ps_uid));

  if (!vs || !ps)
    return {};

  const AbstractShader* gs = nullptr;
  if (NeedsGeometryShader(config.gs_uid))
  {
    auto gs_iter = m_gs_cache.shader_map.find(config.gs_uid);
    if (gs_iter != m_gs_cache.shader_map.end() && !gs_iter->second.pending)
      gs = gs_iter->second.shader.get();
    else
      gs = CreateGeometryShader(config.gs_uid);
    if (!gs)
      return {};
  }

  return GetGXPipelineConfig(config.vertex_format, vs, gs, ps, config.rasterization_state,
                             config.depth_state, config.blending_state, AbstractPipelineUsage::GX);
}

/// Edits the UID based on driver bugs and other special configurations
static GXUberPipelineUid ApplyDriverBugs(const GXUberPipelineUid& in)
{
  GXUberPipelineUid out;
  // TODO: static_assert(std::is_trivially_copyable_v<GXUberPipelineUid>);
  // GXUberPipelineUid is not trivially copyable because RasterizationState and BlendingState aren't
  // either, but we can pretend it is for now. This will be improved after PR #10848 is finished.
  memcpy(static_cast<void*>(&out), static_cast<const void*>(&in), sizeof(out));  // Copy padding
  if (g_backend_info.bSupportsDynamicVertexLoader)
    out.vertex_format = nullptr;

  // If framebuffer fetch is available, we can emulate logic ops in the fragment shader
  // and don't need the below blend approximation
  if (out.blending_state.logic_op_enable && !g_backend_info.bSupportsLogicOp &&
      !g_backend_info.bSupportsFramebufferFetch)
  {
    if (!out.blending_state.LogicOpApproximationIsExact())
      WARN_LOG_FMT(VIDEO,
                   "Approximating logic op with blending, this will produce incorrect rendering.");
    out.blending_state.ApproximateLogicOpWithBlending();
  }

  if (g_backend_info.bSupportsFramebufferFetch)
  {
    // Always blend in shader
    out.blending_state.hex = 0;
    out.blending_state.color_update = in.blending_state.color_update.Value();
    out.blending_state.alpha_update = in.blending_state.alpha_update.Value();
    out.ps_uid.GetUidData()->no_dual_src = true;
  }
  else if (!g_backend_info.bSupportsDualSourceBlend ||
           (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DUAL_SOURCE_BLENDING) &&
            !out.blending_state.RequiresDualSrc()))
  {
    out.blending_state.use_dual_src = false;
    out.ps_uid.GetUidData()->no_dual_src = true;
  }

  if (g_ActiveConfig.UseVSForLinePointExpand())
  {
    // All primitives are expanded to triangles in the vertex shader
    PrimitiveType prim = g_backend_info.bSupportsPrimitiveRestart ? PrimitiveType::TriangleStrip :
                                                                    PrimitiveType::Triangles;
    out.rasterization_state.primitive = prim;
    out.gs_uid.GetUidData()->primitive_type = static_cast<u32>(prim);
  }

  return out;
}

std::optional<AbstractPipelineConfig>
ShaderCache::GetGXPipelineConfig(const GXUberPipelineUid& config_in)
{
  GXUberPipelineUid config = ApplyDriverBugs(config_in);
  const AbstractShader* vs;
  auto vs_iter = m_uber_vs_cache.shader_map.find(config.vs_uid);
  if (vs_iter != m_uber_vs_cache.shader_map.end() && !vs_iter->second.pending)
    vs = vs_iter->second.shader.get();
  else
    vs = InsertVertexUberShader(config.vs_uid, CompileVertexUberShader(config.vs_uid));

  UberShader::PixelShaderUid ps_uid = config.ps_uid;
  UberShader::ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);

  const AbstractShader* ps;
  auto ps_iter = m_uber_ps_cache.shader_map.find(ps_uid);
  if (ps_iter != m_uber_ps_cache.shader_map.end() && !ps_iter->second.pending)
    ps = ps_iter->second.shader.get();
  else
    ps = InsertPixelUberShader(ps_uid, CompilePixelUberShader(ps_uid));

  if (!vs || !ps)
    return {};

  const AbstractShader* gs = nullptr;
  if (NeedsGeometryShader(config.gs_uid))
  {
    auto gs_iter = m_gs_cache.shader_map.find(config.gs_uid);
    if (gs_iter != m_gs_cache.shader_map.end() && !gs_iter->second.pending)
      gs = gs_iter->second.shader.get();
    else
      gs = CreateGeometryShader(config.gs_uid);
    if (!gs)
      return {};
  }

  return GetGXPipelineConfig(config.vertex_format, vs, gs, ps, config.rasterization_state,
                             config.depth_state, config.blending_state,
                             AbstractPipelineUsage::GXUber);
}

std::optional<ShaderCacheFingerprint>
ShaderCache::GetPipelineFingerprint(const GXPipelineUid& uid,
                                    const AbstractPipelineConfig& config) const
{
  // The shaders GetGXPipelineConfig uses for the UID.
  const GXPipelineUid actual_uid = VideoCommon::ApplyDriverBugs(uid);
  PixelShaderUid ps_uid = actual_uid.ps_uid;
  ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);
  const bool needs_gs = NeedsGeometryShader(actual_uid.gs_uid);
  return HashPipeline(m_api_type, config, FindShaderFingerprint(m_vs_cache, actual_uid.vs_uid),
                      needs_gs,
                      needs_gs ? FindShaderFingerprint(m_gs_cache, actual_uid.gs_uid) : std::nullopt,
                      FindShaderFingerprint(m_ps_cache, ps_uid));
}

std::optional<ShaderCacheFingerprint>
ShaderCache::GetPipelineFingerprint(const GXUberPipelineUid& uid,
                                    const AbstractPipelineConfig& config) const
{
  const GXUberPipelineUid actual_uid = ApplyDriverBugs(uid);
  UberShader::PixelShaderUid ps_uid = actual_uid.ps_uid;
  UberShader::ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);
  const bool needs_gs = NeedsGeometryShader(actual_uid.gs_uid);
  return HashPipeline(m_api_type, config,
                      FindShaderFingerprint(m_uber_vs_cache, actual_uid.vs_uid), needs_gs,
                      needs_gs ? FindShaderFingerprint(m_gs_cache, actual_uid.gs_uid) : std::nullopt,
                      FindShaderFingerprint(m_uber_ps_cache, ps_uid));
}

bool ShaderCache::HasPipelineShaders(const GXPipelineUid& uid) const
{
  const GXPipelineUid actual_uid = VideoCommon::ApplyDriverBugs(uid);
  PixelShaderUid ps_uid = actual_uid.ps_uid;
  ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);
  return HasCompiledShader(m_vs_cache, actual_uid.vs_uid) &&
         HasCompiledShader(m_ps_cache, ps_uid) &&
         (!NeedsGeometryShader(actual_uid.gs_uid) ||
          HasCompiledShader(m_gs_cache, actual_uid.gs_uid));
}

bool ShaderCache::HasPipelineShaders(const GXUberPipelineUid& uid) const
{
  const GXUberPipelineUid actual_uid = ApplyDriverBugs(uid);
  UberShader::PixelShaderUid ps_uid = actual_uid.ps_uid;
  UberShader::ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);
  return HasCompiledShader(m_uber_vs_cache, actual_uid.vs_uid) &&
         HasCompiledShader(m_uber_ps_cache, ps_uid) &&
         (!NeedsGeometryShader(actual_uid.gs_uid) ||
          HasCompiledShader(m_gs_cache, actual_uid.gs_uid));
}

const AbstractPipeline* ShaderCache::InsertGXPipeline(const GXPipelineUid& config,
                                                      std::unique_ptr<AbstractPipeline> pipeline)
{
  auto& entry = m_gx_pipeline_cache[config];
  entry.second = false;
  m_gx_pipeline_queued_priority.erase(config);
  if (!entry.first && pipeline)
  {
    entry.first = std::move(pipeline);

    if (g_ActiveConfig.bShaderCache)
    {
      auto cache_data = entry.first->GetCacheData();
      const std::optional<ShaderCacheFingerprint> fingerprint =
          cache_data.empty() ? std::nullopt : GetPipelineFingerprint(config, entry.first->m_config);
      if (fingerprint)
      {
        SerializedGXPipelineUid disk_uid;
        SerializePipelineUid(config, disk_uid);
        const std::vector<u8> disk_entry =
            MakeFingerprintedEntry(*fingerprint, cache_data.data(), cache_data.size());
        m_gx_pipeline_disk_cache.Append(disk_uid, disk_entry.data(),
                                        static_cast<u32>(disk_entry.size()));
      }
    }
  }

  return entry.first.get();
}

const AbstractPipeline*
ShaderCache::InsertGXUberPipeline(const GXUberPipelineUid& config,
                                  std::unique_ptr<AbstractPipeline> pipeline)
{
  auto& entry = m_gx_uber_pipeline_cache[config];
  entry.second = false;
  if (!entry.first && pipeline)
  {
    entry.first = std::move(pipeline);

    if (g_ActiveConfig.bShaderCache)
    {
      auto cache_data = entry.first->GetCacheData();
      const std::optional<ShaderCacheFingerprint> fingerprint =
          cache_data.empty() ? std::nullopt : GetPipelineFingerprint(config, entry.first->m_config);
      if (fingerprint)
      {
        SerializedGXUberPipelineUid disk_uid;
        SerializePipelineUid(config, disk_uid);
        const std::vector<u8> disk_entry =
            MakeFingerprintedEntry(*fingerprint, cache_data.data(), cache_data.size());
        m_gx_uber_pipeline_disk_cache.Append(disk_uid, disk_entry.data(),
                                             static_cast<u32>(disk_entry.size()));
      }
    }
  }

  return entry.first.get();
}

void ShaderCache::LoadPipelineUIDCache()
{
  constexpr u32 CACHE_FILE_MAGIC = 0x44495550;  // PUID
  constexpr size_t CACHE_HEADER_SIZE = sizeof(u32) + sizeof(u32);
  std::string filename =
      File::GetUserPath(D_CACHE_IDX) + SConfig::GetInstance().GetGameID() + ".uidcache";
  if (m_gx_pipeline_uid_cache_file.Open(filename, "rb+"))
  {
    // If an existing case exists, validate the version before reading entries.
    u32 existing_magic;
    u32 existing_version;
    bool uid_file_valid = false;
    if (m_gx_pipeline_uid_cache_file.ReadBytes(&existing_magic, sizeof(existing_magic)) &&
        m_gx_pipeline_uid_cache_file.ReadBytes(&existing_version, sizeof(existing_version)) &&
        existing_magic == CACHE_FILE_MAGIC && existing_version == GX_PIPELINE_UID_VERSION)
    {
      // Ensure the expected size matches the actual size of the file. If it doesn't, it means
      // the cache file may be corrupted, and we should not proceed with loading potentially
      // garbage or invalid UIDs.
      const u64 file_size = m_gx_pipeline_uid_cache_file.GetSize();
      const size_t uid_count =
          static_cast<size_t>(file_size - CACHE_HEADER_SIZE) / sizeof(SerializedGXPipelineUid);
      const size_t expected_size = uid_count * sizeof(SerializedGXPipelineUid) + CACHE_HEADER_SIZE;
      uid_file_valid = file_size == expected_size;
      if (uid_file_valid)
      {
        for (size_t i = 0; i < uid_count; i++)
        {
          SerializedGXPipelineUid serialized_uid;
          if (m_gx_pipeline_uid_cache_file.ReadBytes(&serialized_uid, sizeof(serialized_uid)))
          {
            // This just adds the pipeline to the map, it is compiled later.
            AddSerializedGXPipelineUID(serialized_uid);
          }
          else
          {
            uid_file_valid = false;
            break;
          }
        }
      }

      // We open the file for reading and writing, so we must seek to the end before writing.
      if (uid_file_valid)
        uid_file_valid = m_gx_pipeline_uid_cache_file.Seek(expected_size, File::SeekOrigin::Begin);
    }

    // If the file is invalid, close it. We re-open and truncate it below.
    if (!uid_file_valid)
      m_gx_pipeline_uid_cache_file.Close();
  }

  // If the file is not open, it means it was either corrupted or didn't exist.
  if (!m_gx_pipeline_uid_cache_file.IsOpen())
  {
    if (m_gx_pipeline_uid_cache_file.Open(filename, "wb"))
    {
      // Write the version identifier.
      m_gx_pipeline_uid_cache_file.WriteBytes(&CACHE_FILE_MAGIC, sizeof(GX_PIPELINE_UID_VERSION));
      m_gx_pipeline_uid_cache_file.WriteBytes(&GX_PIPELINE_UID_VERSION,
                                              sizeof(GX_PIPELINE_UID_VERSION));

      // Write any current UIDs out to the file.
      // This way, if we load a UID cache where the data was incomplete (e.g. Dolphin crashed),
      // we don't lose the existing UIDs which were previously at the beginning.
      for (const auto& it : m_gx_pipeline_cache)
        AppendGXPipelineUID(it.first);
    }
  }

  INFO_LOG_FMT(VIDEO, "Read {} pipeline UIDs from {}", m_gx_pipeline_cache.size(), filename);
}

void ShaderCache::ClosePipelineUIDCache()
{
  // This is left as a method in case we need to append extra data to the file in the future.
  m_gx_pipeline_uid_cache_file.Close();
}

void ShaderCache::AddSerializedGXPipelineUID(const SerializedGXPipelineUid& uid)
{
  GXPipelineUid real_uid;
  UnserializePipelineUid(uid, real_uid);

  auto iter = m_gx_pipeline_cache.find(real_uid);
  if (iter != m_gx_pipeline_cache.end())
    return;

  // Flag it as empty with a null pipeline object, for later compilation.
  auto& entry = m_gx_pipeline_cache[real_uid];
  entry.second = false;
}

void ShaderCache::AppendGXPipelineUID(const GXPipelineUid& config)
{
  if (!m_gx_pipeline_uid_cache_file.IsOpen())
    return;

  SerializedGXPipelineUid disk_uid;
  SerializePipelineUid(config, disk_uid);
  if (!m_gx_pipeline_uid_cache_file.WriteBytes(&disk_uid, sizeof(disk_uid)))
  {
    WARN_LOG_FMT(VIDEO, "Writing pipeline UID to cache failed, closing file.");
    m_gx_pipeline_uid_cache_file.Close();
  }
}

void ShaderCache::QueueVertexShaderCompile(const VertexShaderUid& uid, u32 priority)
{
  class VertexShaderWorkItem final : public AsyncShaderCompiler::WorkItem
  {
  public:
    VertexShaderWorkItem(ShaderCache* shader_cache_, const VertexShaderUid& uid_)
        : shader_cache(shader_cache_), uid(uid_)
    {
    }

    bool Compile() override
    {
      shader = shader_cache->CompileVertexShader(uid);
      return true;
    }

    void Retrieve() override { shader_cache->InsertVertexShader(uid, std::move(shader)); }

  private:
    ShaderCache* shader_cache;
    CompiledShader shader;
    VertexShaderUid uid;
  };

  auto& entry = m_vs_cache.shader_map[uid];
  entry.pending = true;
  entry.priority = priority;
  auto wi = m_async_shader_compiler->CreateWorkItem<VertexShaderWorkItem>(this, uid);
  m_async_shader_compiler->QueueWorkItem(std::move(wi), priority);
}

void ShaderCache::QueueVertexUberShaderCompile(const UberShader::VertexShaderUid& uid, u32 priority)
{
  class VertexUberShaderWorkItem final : public AsyncShaderCompiler::WorkItem
  {
  public:
    VertexUberShaderWorkItem(ShaderCache* shader_cache_, const UberShader::VertexShaderUid& uid_)
        : shader_cache(shader_cache_), uid(uid_)
    {
    }

    bool Compile() override
    {
      shader = shader_cache->CompileVertexUberShader(uid);
      return true;
    }

    void Retrieve() override { shader_cache->InsertVertexUberShader(uid, std::move(shader)); }

  private:
    ShaderCache* shader_cache;
    CompiledShader shader;
    UberShader::VertexShaderUid uid;
  };

  m_uber_vs_cache.shader_map[uid].pending = true;
  auto wi = m_async_shader_compiler->CreateWorkItem<VertexUberShaderWorkItem>(this, uid);
  m_async_shader_compiler->QueueWorkItem(std::move(wi), priority);
}

void ShaderCache::QueuePixelShaderCompile(const PixelShaderUid& uid, u32 priority)
{
  class PixelShaderWorkItem final : public AsyncShaderCompiler::WorkItem
  {
  public:
    PixelShaderWorkItem(ShaderCache* shader_cache_, const PixelShaderUid& uid_)
        : shader_cache(shader_cache_), uid(uid_)
    {
    }

    bool Compile() override
    {
      shader = shader_cache->CompilePixelShader(uid);
      return true;
    }

    void Retrieve() override { shader_cache->InsertPixelShader(uid, std::move(shader)); }

  private:
    ShaderCache* shader_cache;
    CompiledShader shader;
    PixelShaderUid uid;
  };

  auto& entry = m_ps_cache.shader_map[uid];
  entry.pending = true;
  entry.priority = priority;
  auto wi = m_async_shader_compiler->CreateWorkItem<PixelShaderWorkItem>(this, uid);
  m_async_shader_compiler->QueueWorkItem(std::move(wi), priority);
}

void ShaderCache::QueuePixelUberShaderCompile(const UberShader::PixelShaderUid& uid, u32 priority)
{
  class PixelUberShaderWorkItem final : public AsyncShaderCompiler::WorkItem
  {
  public:
    PixelUberShaderWorkItem(ShaderCache* shader_cache_, const UberShader::PixelShaderUid& uid_)
        : shader_cache(shader_cache_), uid(uid_)
    {
    }

    bool Compile() override
    {
      shader = shader_cache->CompilePixelUberShader(uid);
      return true;
    }

    void Retrieve() override { shader_cache->InsertPixelUberShader(uid, std::move(shader)); }

  private:
    ShaderCache* shader_cache;
    CompiledShader shader;
    UberShader::PixelShaderUid uid;
  };

  m_uber_ps_cache.shader_map[uid].pending = true;
  auto wi = m_async_shader_compiler->CreateWorkItem<PixelUberShaderWorkItem>(this, uid);
  m_async_shader_compiler->QueueWorkItem(std::move(wi), priority);
}

void ShaderCache::QueuePipelineCompile(const GXPipelineUid& uid, u32 priority)
{
  class PipelineWorkItem final : public AsyncShaderCompiler::WorkItem
  {
  public:
    PipelineWorkItem(ShaderCache* shader_cache_, GXPipelineUid uid_, u32 priority_)
        : shader_cache(shader_cache_), uid(std::move(uid_)), priority(priority_)
    {
      // A copy queued at another priority (HurryPendingPipeline) may have compiled it already:
      // then this one does nothing.
      if (IsCompiled())
      {
        stages_ready = true;
        return;
      }
      // Check if all the stages required for this pipeline have been compiled.
      // If not, this work item becomes a no-op, and re-queues the pipeline for the next frame.
      if (SetStagesReady())
        config = shader_cache->GetGXPipelineConfig(uid);
    }

    bool IsCompiled() const
    {
      auto it = shader_cache->m_gx_pipeline_cache.find(uid);
      return it != shader_cache->m_gx_pipeline_cache.end() && it->second.first;
    }

    bool SetStagesReady()
    {
      stages_ready = true;

      GXPipelineUid actual_uid = ApplyDriverBugs(uid);

      // A stage still pending at a lower priority (the shader cache's) is queued again at this
      // pipeline's: the copy compiled first is kept (InsertVertexShader / InsertPixelShader).
      auto vs_it = shader_cache->m_vs_cache.shader_map.find(actual_uid.vs_uid);
      stages_ready &= vs_it != shader_cache->m_vs_cache.shader_map.end() && !vs_it->second.pending;
      if (vs_it == shader_cache->m_vs_cache.shader_map.end() ||
          (vs_it->second.pending && priority < vs_it->second.priority))
      {
        shader_cache->QueueVertexShaderCompile(actual_uid.vs_uid, priority);
      }

      PixelShaderUid ps_uid = actual_uid.ps_uid;
      ClearUnusedPixelShaderUidBits(shader_cache->m_api_type, shader_cache->m_host_config, &ps_uid);

      auto ps_it = shader_cache->m_ps_cache.shader_map.find(ps_uid);
      stages_ready &= ps_it != shader_cache->m_ps_cache.shader_map.end() && !ps_it->second.pending;
      if (ps_it == shader_cache->m_ps_cache.shader_map.end() ||
          (ps_it->second.pending && priority < ps_it->second.priority))
      {
        shader_cache->QueuePixelShaderCompile(ps_uid, priority);
      }

      return stages_ready;
    }

    bool Compile() override
    {
      if (config)
        pipeline = g_gfx->CreatePipeline(*config);
      return true;
    }

    void Retrieve() override
    {
      if (stages_ready)
      {
        shader_cache->InsertGXPipeline(uid, std::move(pipeline));
      }
      else if (!IsCompiled())
      {
        // Re-queue for next frame.
        auto wi = shader_cache->m_async_shader_compiler->CreateWorkItem<PipelineWorkItem>(
            shader_cache, uid, priority);
        shader_cache->m_async_shader_compiler->QueueWorkItem(std::move(wi), priority);
      }
    }

  private:
    ShaderCache* shader_cache;
    std::unique_ptr<AbstractPipeline> pipeline;
    GXPipelineUid uid;
    u32 priority;
    std::optional<AbstractPipelineConfig> config;
    bool stages_ready;
  };

  auto wi = m_async_shader_compiler->CreateWorkItem<PipelineWorkItem>(this, uid, priority);
  m_async_shader_compiler->QueueWorkItem(std::move(wi), priority);
  m_gx_pipeline_cache[uid].second = true;
  auto [queued, inserted] = m_gx_pipeline_queued_priority.try_emplace(uid, priority);
  if (!inserted)
    queued->second = std::min(queued->second, priority);
}

void ShaderCache::HurryPendingPipeline(const GXPipelineUid& uid, u32 priority)
{
  // The pipelines of the UID cache are all queued at the start (CompileMissingPipelines) at the
  // lowest priority, and with one compiler thread they compile one after another. With the shader
  // binaries kept (the same build) that is fast; after an update (a new build: LinearDiskCache's
  // header holds the git revision) every shader of every game played compiles again, many seconds,
  // and a draw whose pipeline was among them was skipped until all were done: Brawl's menus stayed
  // white 3-10 s at the first boot after each update, with sound and a working cursor.
  auto it = m_gx_pipeline_queued_priority.find(uid);
  if (it != m_gx_pipeline_queued_priority.end() && it->second <= priority)
    return;
  QueuePipelineCompile(uid, priority);
}

void ShaderCache::QueueUberPipelineCompile(const GXUberPipelineUid& uid, u32 priority)
{
  class UberPipelineWorkItem final : public AsyncShaderCompiler::WorkItem
  {
  public:
    UberPipelineWorkItem(ShaderCache* shader_cache_, GXUberPipelineUid uid_, u32 priority_)
        : shader_cache(shader_cache_), uid(std::move(uid_)), priority(priority_)
    {
      // Check if all the stages required for this UberPipeline have been compiled.
      // If not, this work item becomes a no-op, and re-queues the UberPipeline for the next frame.
      if (SetStagesReady())
        config = shader_cache->GetGXPipelineConfig(uid);
    }

    bool SetStagesReady()
    {
      stages_ready = true;

      GXUberPipelineUid actual_uid = ApplyDriverBugs(uid);

      auto vs_it = shader_cache->m_uber_vs_cache.shader_map.find(actual_uid.vs_uid);
      stages_ready &=
          vs_it != shader_cache->m_uber_vs_cache.shader_map.end() && !vs_it->second.pending;
      if (vs_it == shader_cache->m_uber_vs_cache.shader_map.end())
        shader_cache->QueueVertexUberShaderCompile(actual_uid.vs_uid, priority);

      UberShader::PixelShaderUid ps_uid = actual_uid.ps_uid;
      UberShader::ClearUnusedPixelShaderUidBits(shader_cache->m_api_type,
                                                shader_cache->m_host_config, &ps_uid);

      auto ps_it = shader_cache->m_uber_ps_cache.shader_map.find(ps_uid);
      stages_ready &=
          ps_it != shader_cache->m_uber_ps_cache.shader_map.end() && !ps_it->second.pending;
      if (ps_it == shader_cache->m_uber_ps_cache.shader_map.end())
        shader_cache->QueuePixelUberShaderCompile(ps_uid, priority);

      return stages_ready;
    }

    bool Compile() override
    {
      if (config)
        UberPipeline = g_gfx->CreatePipeline(*config);
      return true;
    }

    void Retrieve() override
    {
      if (stages_ready)
      {
        shader_cache->InsertGXUberPipeline(uid, std::move(UberPipeline));
      }
      else
      {
        // Re-queue for next frame.
        auto wi = shader_cache->m_async_shader_compiler->CreateWorkItem<UberPipelineWorkItem>(
            shader_cache, uid, priority);
        shader_cache->m_async_shader_compiler->QueueWorkItem(std::move(wi), priority);
      }
    }

  private:
    ShaderCache* shader_cache;
    std::unique_ptr<AbstractPipeline> UberPipeline;
    GXUberPipelineUid uid;
    u32 priority;
    std::optional<AbstractPipelineConfig> config;
    bool stages_ready;
  };

  auto wi = m_async_shader_compiler->CreateWorkItem<UberPipelineWorkItem>(this, uid, priority);
  m_async_shader_compiler->QueueWorkItem(std::move(wi), priority);
  m_gx_uber_pipeline_cache[uid].second = true;
}

void ShaderCache::QueueUberShaderPipelines()
{
  // Create a dummy vertex format with no attributes.
  // All attributes will be enabled in GetUberVertexFormat.
  PortableVertexDeclaration dummy_vertex_decl = {};
  dummy_vertex_decl.position.components = 4;
  dummy_vertex_decl.position.type = ComponentFormat::Float;
  dummy_vertex_decl.position.enable = true;
  dummy_vertex_decl.stride = sizeof(float) * 4;
  NativeVertexFormat* dummy_vertex_format =
      VertexLoaderManager::GetUberVertexFormat(dummy_vertex_decl);
  auto QueueDummyPipeline =
      [&](const UberShader::VertexShaderUid& vs_uid, const GeometryShaderUid& gs_uid,
          const UberShader::PixelShaderUid& ps_uid, const BlendingState& blend) {
        GXUberPipelineUid config;
        config.vertex_format = dummy_vertex_format;
        config.vs_uid = vs_uid;
        config.gs_uid = gs_uid;
        config.ps_uid = ps_uid;
        config.rasterization_state = RenderState::GetCullBackFaceRasterizationState(
            static_cast<PrimitiveType>(gs_uid.GetUidData()->primitive_type));
        config.depth_state = RenderState::GetNoDepthTestingDepthState();
        config.blending_state = blend;
        if (ps_uid.GetUidData()->uint_output)
        {
          // uint_output is only ever enabled when logic ops are enabled.
          config.blending_state.logic_op_enable = true;
          config.blending_state.logic_mode = LogicOp::And;
        }

        auto iter = m_gx_uber_pipeline_cache.find(config);
        if (iter != m_gx_uber_pipeline_cache.end())
          return;

        auto& entry = m_gx_uber_pipeline_cache[config];
        entry.second = false;
      };

  // Populate the pipeline configs with empty entries, these will be compiled afterwards.
  UberShader::EnumerateVertexShaderUids([&](const UberShader::VertexShaderUid& vuid) {
    UberShader::EnumeratePixelShaderUids([&](const UberShader::PixelShaderUid& puid) {
      // UIDs must have compatible texgens, a mismatching combination will never be queried.
      if (vuid.GetUidData()->num_texgens != puid.GetUidData()->num_texgens)
        return;

      UberShader::PixelShaderUid cleared_puid = puid;
      UberShader::ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &cleared_puid);
      EnumerateGeometryShaderUids([&](const GeometryShaderUid& guid) {
        if (guid.GetUidData()->numTexGens != vuid.GetUidData()->num_texgens ||
            (!guid.GetUidData()->IsPassthrough() && !m_host_config.backend_geometry_shaders))
        {
          return;
        }
        BlendingState blend = RenderState::GetNoBlendingBlendState();
        QueueDummyPipeline(vuid, guid, cleared_puid, blend);
        if (g_backend_info.bSupportsDynamicVertexLoader)
        {
          // Not all GPUs need all the pipeline state compiled into shaders, so they tend to key
          // compiled shaders based on some subset of the pipeline state.
          // Some test results:
          //   (GPUs tested: AMD Radeon Pro 5600M, Nvidia GT 750M, Intel UHD 630,
          //    Intel Iris Pro 5200, Apple M1)
          // MacOS Metal:
          //  - AMD, Nvidia, Intel GPUs: Shaders are keyed on vertex layout and whether or not
          //    dual source blend is enabled.  That's it.
          //  - Apple GPUs: Shaders are keyed on vertex layout and all blending settings.  We use
          //    framebuffer fetch here, so the only blending settings used by ubershaders are the
          //    alpha_update and color_update ones.  Also keyed on primitive type, but Metal
          //    supports setting it to "unknown" and we do for ubershaders (but MoltenVK won't).
          // Windows Vulkan:
          //  - AMD, Nvidia: Definitely keyed on dual source blend, but the others seem more random
          //    Changing a setting on one shader will require a recompile, but changing the same
          //    setting on another won't.  Compiling a copy with alpha_update off, color_update off,
          //    and one with DSB on seems to get pretty good coverage though.
          // Windows D3D12:
          //  - AMD: Keyed on dual source blend and vertex layout
          //  - Nvidia Kepler: No recompiles for changes to vertex layout or blend
          blend.alpha_update = false;
          QueueDummyPipeline(vuid, guid, cleared_puid, blend);
          blend.alpha_update = true;
          blend.color_update = false;
          QueueDummyPipeline(vuid, guid, cleared_puid, blend);
          blend.color_update = true;
          if (!cleared_puid.GetUidData()->no_dual_src && !cleared_puid.GetUidData()->uint_output)
          {
            blend.blend_enable = true;
            blend.use_dual_src = true;
            blend.src_factor = SrcBlendFactor::SrcAlpha;
            blend.dst_factor = DstBlendFactor::InvSrcAlpha;
            QueueDummyPipeline(vuid, guid, cleared_puid, blend);
          }
        }
      });
    });
  });
}

const AbstractPipeline*
ShaderCache::GetEFBCopyToVRAMPipeline(const TextureConversionShaderGen::TCShaderUid& uid)
{
  auto iter = m_efb_copy_to_vram_pipelines.find(uid);
  if (iter != m_efb_copy_to_vram_pipelines.end())
    return iter->second.get();

  SyncCompileTimer timer(true);

  auto shader_code = TextureConversionShaderGen::GeneratePixelShader(m_api_type, uid.GetUidData());
  auto shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, shader_code.GetBuffer(), nullptr,
      fmt::format("EFB copy to VRAM pixel shader: {}", *uid.GetUidData()));
  if (!shader)
  {
    m_efb_copy_to_vram_pipelines.emplace(uid, nullptr);
    return nullptr;
  }

  AbstractPipelineConfig config = {};
  config.vertex_format = nullptr;
  config.vertex_shader = m_efb_copy_vertex_shader.get();
  config.geometry_shader =
      UseGeometryShaderForEFBCopies() ? m_texcoord_geometry_shader.get() : nullptr;
  config.pixel_shader = shader.get();
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetRGBA8FramebufferState();
  config.usage = AbstractPipelineUsage::Utility;
  auto iiter = m_efb_copy_to_vram_pipelines.emplace(uid, g_gfx->CreatePipeline(config));
  return iiter.first->second.get();
}

const AbstractPipeline* ShaderCache::GetEFBCopyToRAMPipeline(const EFBCopyParams& uid)
{
  auto iter = m_efb_copy_to_ram_pipelines.find(uid);
  if (iter != m_efb_copy_to_ram_pipelines.end())
    return iter->second.get();

  SyncCompileTimer timer(true);

  const std::string shader_code =
      TextureConversionShaderTiled::GenerateEncodingShader(uid, m_api_type);
  const auto shader =
      g_gfx->CreateShaderFromSource(ShaderStage::Pixel, shader_code, nullptr,
                                    fmt::format("EFB copy to RAM pixel shader: {}", uid));
  if (!shader)
  {
    m_efb_copy_to_ram_pipelines.emplace(uid, nullptr);
    return nullptr;
  }

  AbstractPipelineConfig config = {};
  config.vertex_shader = m_screen_quad_vertex_shader.get();
  config.pixel_shader = shader.get();
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetColorFramebufferState(AbstractTextureFormat::BGRA8);
  config.usage = AbstractPipelineUsage::Utility;
  auto iiter = m_efb_copy_to_ram_pipelines.emplace(uid, g_gfx->CreatePipeline(config));
  return iiter.first->second.get();
}

bool ShaderCache::CompileSharedPipelines()
{
  m_screen_quad_vertex_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Vertex, FramebufferShaderGen::GenerateScreenQuadVertexShader(), nullptr,
      "Screen quad vertex shader");
  m_texture_copy_vertex_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Vertex, FramebufferShaderGen::GenerateTextureCopyVertexShader(), nullptr,
      "Texture copy vertex shader");
  m_efb_copy_vertex_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Vertex, TextureConversionShaderGen::GenerateVertexShader(m_api_type).GetBuffer(),
      nullptr, "EFB copy vertex shader");
  if (!m_screen_quad_vertex_shader || !m_texture_copy_vertex_shader || !m_efb_copy_vertex_shader)
    return false;

  if (UseGeometryShaderForEFBCopies())
  {
    m_texcoord_geometry_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Geometry, FramebufferShaderGen::GeneratePassthroughGeometryShader(1, 0),
        nullptr, "Texcoord passthrough geometry shader");
    m_color_geometry_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Geometry, FramebufferShaderGen::GeneratePassthroughGeometryShader(0, 1),
        nullptr, "Color passthrough geometry shader");
    if (!m_texcoord_geometry_shader || !m_color_geometry_shader)
      return false;
  }

  m_texture_copy_pixel_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, FramebufferShaderGen::GenerateTextureCopyPixelShader(), nullptr,
      "Texture copy pixel shader");
  m_color_pixel_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, FramebufferShaderGen::GenerateColorPixelShader(), nullptr,
      "Color pixel shader");
  if (!m_texture_copy_pixel_shader || !m_color_pixel_shader)
    return false;

  AbstractPipelineConfig config;
  config.vertex_format = nullptr;
  config.vertex_shader = m_texture_copy_vertex_shader.get();
  config.geometry_shader = nullptr;
  config.pixel_shader = m_texture_copy_pixel_shader.get();
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetRGBA8FramebufferState();
  config.usage = AbstractPipelineUsage::Utility;
  m_copy_rgba8_pipeline = g_gfx->CreatePipeline(config);
  if (!m_copy_rgba8_pipeline)
    return false;

  if (UseGeometryShaderForEFBCopies())
  {
    config.geometry_shader = m_texcoord_geometry_shader.get();
    m_rgba8_stereo_copy_pipeline = g_gfx->CreatePipeline(config);
    if (!m_rgba8_stereo_copy_pipeline)
      return false;
  }

  if (m_host_config.backend_palette_conversion)
  {
    config.vertex_shader = m_screen_quad_vertex_shader.get();
    config.geometry_shader = nullptr;

    for (size_t i = 0; i < NUM_PALETTE_CONVERSION_SHADERS; i++)
    {
      TLUTFormat format = static_cast<TLUTFormat>(i);
      auto shader = g_gfx->CreateShaderFromSource(
          ShaderStage::Pixel,
          TextureConversionShaderTiled::GeneratePaletteConversionShader(format, m_api_type),
          nullptr, fmt::format("Palette conversion pixel shader: {}", format));
      if (!shader)
        return false;

      config.pixel_shader = shader.get();
      m_palette_conversion_pipelines[i] = g_gfx->CreatePipeline(config);
      if (!m_palette_conversion_pipelines[i])
        return false;
    }
  }

  return true;
}

const AbstractPipeline* ShaderCache::GetPaletteConversionPipeline(TLUTFormat format)
{
  ASSERT(static_cast<size_t>(format) < NUM_PALETTE_CONVERSION_SHADERS);
  return m_palette_conversion_pipelines[static_cast<size_t>(format)].get();
}

const AbstractPipeline* ShaderCache::GetTextureReinterpretPipeline(TextureFormat from_format,
                                                                   TextureFormat to_format)
{
  const auto key = std::make_pair(from_format, to_format);
  const auto [iter, inserted] = m_texture_reinterpret_pipelines.emplace(key, nullptr);

  if (!inserted)
    return iter->second.get();

  SyncCompileTimer timer(true);
  std::string shader_source =
      FramebufferShaderGen::GenerateTextureReinterpretShader(from_format, to_format);
  if (shader_source.empty())
    return nullptr;

  std::unique_ptr<AbstractShader> shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, shader_source, nullptr,
      fmt::format("Texture reinterpret pixel shader: {} to {}", from_format, to_format));
  if (!shader)
    return nullptr;

  AbstractPipelineConfig config;
  config.vertex_format = nullptr;
  config.vertex_shader = m_screen_quad_vertex_shader.get();
  config.geometry_shader = nullptr;
  config.pixel_shader = shader.get();
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetRGBA8FramebufferState();
  config.usage = AbstractPipelineUsage::Utility;
  iter->second = g_gfx->CreatePipeline(config);
  return iter->second.get();
}

const AbstractShader*
ShaderCache::GetTextureDecodingShader(TextureFormat format,
                                      std::optional<TLUTFormat> palette_format)
{
  const auto key = std::make_pair(static_cast<u32>(format),
                                  static_cast<u32>(palette_format.value_or(TLUTFormat::IA8)));
  const auto [iter, inserted] = m_texture_decoding_shaders.emplace(key, nullptr);
  if (!inserted)
    return iter->second.get();

  const std::string shader_source =
      TextureConversionShaderTiled::GenerateDecodingShader(format, palette_format, APIType::OpenGL);
  if (shader_source.empty())
    return nullptr;

  const std::string name =
      palette_format.has_value() ?
          fmt::format("Texture decoding compute shader: {}, {}", format, *palette_format) :
          fmt::format("Texture decoding compute shader: {}", format);

  std::unique_ptr<AbstractShader> shader =
      g_gfx->CreateShaderFromSource(ShaderStage::Compute, shader_source, nullptr, name);
  if (!shader)
    return nullptr;

  iter->second = std::move(shader);
  return iter->second.get();
}
}  // namespace VideoCommon
