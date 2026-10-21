// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/ClipRecorder.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"
#include "Core/Rollback/ClipEncoder.h"

namespace Gprb::Clip
{
namespace
{
// 100 ns units (Media Foundation's; FFmpeg's encoder converts).
constexpr s64 UNITS = 10'000'000;
// Brawl's frame (NTSC, 60000/1001 a second), for a frame's nominal duration.
constexpr s64 FRAME_UNITS = UNITS * 1001 / 60000;
// Frames queued for the encoder at most: then the frame dumper waits (no frame is dropped).
constexpr size_t MAX_QUEUED_FRAMES = 6;
// Audio is handed to the encoder in pieces of this many sample frames.
constexpr size_t AUDIO_CHUNK = 1024;

struct Job
{
  enum class Kind
  {
    Video,
    Audio,
  } kind;
  std::vector<u8> video;  // the video's width x height, BGRA, top-down
  std::vector<s16> audio;  // left, right, ... at AUDIO_RATE
  s64 pts = 0;
};

class Writer
{
public:
  // Starts the encoder thread, which makes the encoder for `path`; the error when it could not.
  std::optional<std::string> Start(const std::string& path, int width, int height)
  {
    m_path = path;
    m_width = width;
    m_height = height;
    std::promise<std::optional<std::string>> made;
    auto result = made.get_future();
    m_thread = std::thread([this, made = std::move(made)]() mutable { Run(std::move(made)); });
    auto error = result.get();
    if (error && m_thread.joinable())
      m_thread.join();
    return error;
  }

  void Push(Job job)
  {
    std::unique_lock lk(m_mutex);
    if (job.kind == Job::Kind::Video)
      m_space.wait(lk, [&] { return m_queued_frames < MAX_QUEUED_FRAMES || m_stop || m_failed; });
    if (m_failed || m_stop)
      return;
    if (job.kind == Job::Kind::Video)
      ++m_queued_frames;
    m_jobs.push_back(std::move(job));
    m_work.notify_one();
  }

  // Writes out what is queued and finishes the file.
  std::optional<std::string> Finish(bool discard)
  {
    {
      std::lock_guard lk(m_mutex);
      m_stop = true;
      m_discard = discard;
      m_work.notify_one();
      m_space.notify_all();
    }
    if (m_thread.joinable())
      m_thread.join();
    std::lock_guard lk(m_mutex);
    if (!m_error.empty())
      return m_error;
    return std::nullopt;
  }

  u64 FramesWritten() const { return m_frames_written.load(std::memory_order_relaxed); }

  std::string Error()
  {
    std::lock_guard lk(m_mutex);
    return m_error;
  }

private:
  void Run(std::promise<std::optional<std::string>> made)
  {
    Common::SetCurrentThreadName("Clip encoder");
    // Written as <path>.part and named <path> once complete: a render cut short (Dolphin ended
    // with it) leaves no half-written video under the real name.
    const std::string part = m_path + ".part";
    std::string error;
    std::unique_ptr<Encoder> encoder = CreateEncoder(part, m_width, m_height, AUDIO_RATE, &error);
    if (!encoder)
    {
      File::Delete(part);
      made.set_value(error.empty() ? std::string("no video encoder") : error);
      return;
    }
    made.set_value(std::nullopt);
    bool ok = true;
    for (;;)
    {
      Job job;
      bool discard;
      {
        std::unique_lock lk(m_mutex);
        m_work.wait(lk, [&] { return !m_jobs.empty() || m_stop; });
        if (m_jobs.empty())
          break;
        job = std::move(m_jobs.front());
        m_jobs.pop_front();
        if (job.kind == Job::Kind::Video)
        {
          --m_queued_frames;
          m_space.notify_all();
        }
        discard = m_discard;
      }
      if (!ok || discard)
        continue;
      if (job.kind == Job::Kind::Video)
      {
        ok = encoder->WriteVideo(job.video.data(), m_width * 4, job.pts, FRAME_UNITS);
        if (ok)
          m_frames_written.fetch_add(1, std::memory_order_relaxed);
      }
      else
      {
        ok = encoder->WriteAudio(job.audio.data(), job.audio.size() / 2, job.pts);
      }
      if (!ok)
      {
        std::lock_guard lk(m_mutex);
        m_error = encoder->Error();
        m_failed = true;
        m_space.notify_all();
        ERROR_LOG_FMT(BRAWLBACK, "clip: encoding {} failed: {}", m_path, m_error);
      }
    }
    bool discard;
    {
      std::lock_guard lk(m_mutex);
      discard = m_discard;
    }
    std::string finish_error;
    if (ok && !discard && !encoder->Finish())
      finish_error = encoder->Error();
    encoder.reset();
    if (ok && !discard && finish_error.empty() && !File::Rename(part, m_path))
      finish_error = "the finished video could not be named " + m_path;
    std::lock_guard lk(m_mutex);
    if (!finish_error.empty())
      m_error = finish_error;
    if (discard || !m_error.empty())
      File::Delete(part);
  }

  std::string m_path;
  int m_width = 0, m_height = 0;
  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_work;
  std::condition_variable m_space;
  std::deque<Job> m_jobs;
  size_t m_queued_frames = 0;
  bool m_stop = false;
  bool m_discard = false;
  bool m_failed = false;
  std::string m_error;
  std::atomic<u64> m_frames_written{0};
};

std::mutex s_mutex;
std::unique_ptr<Writer> s_writer;
std::atomic<bool> s_open{false};
// The open file's size (set before its first frame can come).
std::atomic<int> s_width{1280};
std::atomic<int> s_height{DEFAULT_HEIGHT};

// The open segment (under s_mutex).
struct Segment
{
  bool open = false;
  u64 start = 0;
  u64 end = UINT64_MAX;
  u64 last_video_ticks = 0;
  u64 frames = 0;
  u32 ticks_per_second = 1;
  // Audio: resampling state, what is waiting to be handed over, and frames handed over.
  s16 prev_l = 0, prev_r = 0;
  double pos = 0;  // position of the next output sample, in input samples since `prev`
  bool have_prev = false;
  std::vector<s16> pending;
  u64 audio_frames = 0;
  // Frames that came and were left out: before or after it, or not later than the last.
  u64 early = 0, late = 0, repeated = 0;
};
Segment s_seg;
std::atomic<bool> s_segment_open{false};
// Where the file's timeline is (100 ns units): the segments before the open one.
s64 s_offset = 0;

s64 TicksToUnits(u64 ticks, u32 tps)
{
  // (Split: ticks x 10^7 overflows 64 bits after about 20 minutes.)
  return static_cast<s64>((ticks / tps) * UNITS + (ticks % tps) * UNITS / tps);
}

// Hands the pending audio over (under s_mutex); `all` also a last piece shorter than a chunk.
void FlushAudio(bool all)
{
  while (s_writer && (s_seg.pending.size() >= AUDIO_CHUNK * 2 || (all && !s_seg.pending.empty())))
  {
    const size_t n = std::min(s_seg.pending.size(), AUDIO_CHUNK * 2);
    Job job{Job::Kind::Audio};
    job.audio.assign(s_seg.pending.begin(), s_seg.pending.begin() + n);
    s_seg.pending.erase(s_seg.pending.begin(), s_seg.pending.begin() + n);
    job.pts = s_offset + static_cast<s64>(s_seg.audio_frames) * UNITS / AUDIO_RATE;
    s_seg.audio_frames += n / 2;
    s_writer->Push(std::move(job));
  }
}
}  // namespace

int WidthFor(int height)
{
  return (height * 16 / 9 + 1) & ~1;
}

std::optional<std::string> Open(const std::string& path, int height)
{
  Close(true);
  if (height != 480 && height != 720 && height != 1080 && height != 1440)
    height = DEFAULT_HEIGHT;
  const int width = WidthFor(height);
  auto writer = std::make_unique<Writer>();
  if (auto error = writer->Start(path, width, height))
    return error;
  s_width = width;
  s_height = height;
  std::lock_guard lk(s_mutex);
  s_writer = std::move(writer);
  s_offset = 0;
  s_seg = {};
  s_segment_open = false;
  s_open = true;
  NOTICE_LOG_FMT(BRAWLBACK, "clip: writing {}", path);
  return std::nullopt;
}

std::optional<std::string> Close(bool discard)
{
  std::unique_ptr<Writer> writer;
  {
    std::lock_guard lk(s_mutex);
    if (!s_writer)
      return std::nullopt;
    if (s_seg.open && !discard)
      FlushAudio(true);
    s_seg = {};
    s_segment_open = false;
    s_open = false;
    writer = std::move(s_writer);
  }
  auto error = writer->Finish(discard);
  NOTICE_LOG_FMT(BRAWLBACK, "clip: {} ({} frames){}", discard ? "discarded" : "written",
                 writer->FramesWritten(), error ? ": " + *error : std::string());
  return error;
}

bool IsOpen()
{
  return s_open.load(std::memory_order_relaxed);
}

void BeginSegment(u64 start_ticks)
{
  std::lock_guard lk(s_mutex);
  if (!s_writer)
    return;
  s_seg = {};
  s_seg.open = true;
  s_seg.start = start_ticks;
  s_seg.ticks_per_second = Core::System::GetInstance().GetSystemTimers().GetTicksPerSecond();
  s_segment_open = true;
  INFO_LOG_FMT(BRAWLBACK, "clip: segment from tick {} (file at {:.3f} s)", start_ticks,
               static_cast<double>(s_offset) / UNITS);
}

void MarkSegmentEnd(u64 end_ticks)
{
  std::lock_guard lk(s_mutex);
  if (s_seg.open)
    s_seg.end = std::max(end_ticks, s_seg.start);
}

u64 SegmentFrames()
{
  std::lock_guard lk(s_mutex);
  return s_seg.frames;
}

u64 LastFrameTicks()
{
  std::lock_guard lk(s_mutex);
  return s_seg.last_video_ticks;
}

bool EndSegment()
{
  std::lock_guard lk(s_mutex);
  if (!s_seg.open || !s_writer)
    return false;
  const u64 end = s_seg.end != UINT64_MAX ? s_seg.end : std::max(s_seg.last_video_ticks, s_seg.start);
  const s64 length = TicksToUnits(end - s_seg.start, s_seg.ticks_per_second);
  // The audio as long as the segment: silence added, or what is left over dropped.
  const u64 want = static_cast<u64>(length) * AUDIO_RATE / UNITS;
  const u64 have = s_seg.audio_frames + s_seg.pending.size() / 2;
  if (have < want)
    s_seg.pending.insert(s_seg.pending.end(), static_cast<size_t>(want - have) * 2, 0);
  else if (have > want)
    s_seg.pending.resize(static_cast<size_t>(std::max<u64>(want, s_seg.audio_frames) - s_seg.audio_frames) * 2);
  FlushAudio(true);
  INFO_LOG_FMT(BRAWLBACK, "clip: segment of {:.3f} s, {} frames (left out: {} before, {} after, {} repeated)",
               static_cast<double>(length) / UNITS, s_seg.frames, s_seg.early, s_seg.late, s_seg.repeated);
  s_offset += length;
  s_seg = {};
  s_segment_open = false;
  return true;
}

bool WantsFrames()
{
  return s_segment_open.load(std::memory_order_relaxed);
}

void FitSize(int src_width, int src_height, int* width, int* height)
{
  if (src_width <= 0 || src_height <= 0)
  {
    *width = s_width;
    *height = s_height;
    return;
  }
  const int out_w = s_width, out_h = s_height;
  const double scale = std::min(static_cast<double>(out_w) / src_width, static_cast<double>(out_h) / src_height);
  *width = std::clamp(static_cast<int>(src_width * scale + 0.5) & ~1, 2, out_w);
  *height = std::clamp(static_cast<int>(src_height * scale + 0.5) & ~1, 2, out_h);
}

void AddVideoFrame(const u8* rgba, int width, int height, int stride, u64 ticks)
{
  s64 pts;
  {
    std::lock_guard lk(s_mutex);
    if (!s_seg.open || !s_writer)
      return;
    if (ticks < s_seg.start || ticks >= s_seg.end || (s_seg.frames > 0 && ticks <= s_seg.last_video_ticks))
    {
      ++(ticks < s_seg.start ? s_seg.early : ticks >= s_seg.end ? s_seg.late : s_seg.repeated);
      return;
    }
    s_seg.last_video_ticks = ticks;
    ++s_seg.frames;
    pts = s_offset + TicksToUnits(ticks - s_seg.start, s_seg.ticks_per_second);
  }
  // Centred on black, RGBA -> BGRA.
  Job job{Job::Kind::Video};
  const int out_w = s_width, out_h = s_height;
  job.video.assign(static_cast<size_t>(out_w) * out_h * 4, 0);
  const int w = std::min(width, out_w), h = std::min(height, out_h);
  const int x0 = (out_w - w) / 2, y0 = (out_h - h) / 2;
  for (int y = 0; y < h; ++y)
  {
    const u8* src = rgba + static_cast<size_t>(y) * stride;
    u8* dst = job.video.data() + (static_cast<size_t>(y0 + y) * out_w + x0) * 4;
    for (int x = 0; x < w; ++x, src += 4, dst += 4)
    {
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = 255;
    }
  }
  job.pts = pts;
  // (Outside s_mutex: this may wait for the encoder.)
  Writer* writer;
  {
    std::lock_guard lk(s_mutex);
    writer = s_writer.get();
  }
  if (writer)
    writer->Push(std::move(job));
}

void AddAudioSamples(const s16* samples, size_t num_samples, u32 sample_rate_divisor, u64 ticks)
{
  if (!s_segment_open.load(std::memory_order_relaxed) || sample_rate_divisor == 0)
    return;
  std::lock_guard lk(s_mutex);
  if (!s_seg.open || !s_writer || ticks < s_seg.start || ticks >= s_seg.end)
    return;
  // The input rate: the mixer's dividend (54 MHz x 2) over its divisor (32 kHz for the DSP).
  const double in_rate = 108'000'000.0 / sample_rate_divisor;
  const double step = in_rate / AUDIO_RATE;
  for (size_t i = 0; i < num_samples; ++i)
  {
    const s16 r = static_cast<s16>(Common::swap16(static_cast<u16>(samples[i * 2])));
    const s16 l = static_cast<s16>(Common::swap16(static_cast<u16>(samples[i * 2 + 1])));
    if (!s_seg.have_prev)
    {
      s_seg.prev_l = l;
      s_seg.prev_r = r;
      s_seg.have_prev = true;
      continue;
    }
    // Output samples between the previous input sample and this one.
    while (s_seg.pos < 1.0)
    {
      const double t = s_seg.pos;
      s_seg.pending.push_back(static_cast<s16>(s_seg.prev_l + (l - s_seg.prev_l) * t));
      s_seg.pending.push_back(static_cast<s16>(s_seg.prev_r + (r - s_seg.prev_r) * t));
      s_seg.pos += step;
    }
    s_seg.pos -= 1.0;
    s_seg.prev_l = l;
    s_seg.prev_r = r;
  }
  FlushAudio(false);
}

u64 FramesWritten()
{
  std::lock_guard lk(s_mutex);
  return s_writer ? s_writer->FramesWritten() : 0;
}

std::string LastError()
{
  std::lock_guard lk(s_mutex);
  return s_writer ? s_writer->Error() : std::string();
}
}  // namespace Gprb::Clip
