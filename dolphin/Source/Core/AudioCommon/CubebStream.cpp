// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/CubebStream.h"

#include <chrono>
#include <string>

#include <cubeb/cubeb.h>
#include <fmt/format.h>

#include "AudioCommon/CubebUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/System.h"

#ifdef _WIN32
#include <objbase.h>
#endif

// ~10 ms - needs to be at least 240 for surround
constexpr u32 BUFFER_SAMPLES = 512;

// A running stream that cubeb has not asked for samples for this long has stopped.
constexpr s64 OUTPUT_STALL_US = 3000000;
// How long to wait before trying again when the stream could not be opened again.
constexpr s64 REOPEN_RETRY_US = 5000000;

static s64 NowUs()
{
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

long CubebStream::DataCallback(cubeb_stream* stream, void* user_data, const void* /*input_buffer*/,
                               void* output_buffer, long num_frames)
{
  auto* const self = static_cast<CubebStream*>(user_data);
  self->m_last_callback_us.store(NowUs(), std::memory_order_relaxed);

  if (self->m_stereo)
    self->m_mixer->Mix(static_cast<short*>(output_buffer), num_frames);
  else
    self->m_mixer->MixSurround(static_cast<float*>(output_buffer), num_frames);

  return num_frames;
}

void CubebStream::StateCallback(cubeb_stream* stream, void* user_data, cubeb_state state)
{
  if (state == CUBEB_STATE_ERROR)
    static_cast<CubebStream*>(user_data)->m_failed.store(true, std::memory_order_relaxed);
}

long CubebStream::WiimoteDataCallback(cubeb_stream* stream, void* user_data,
                                      const void* /*input_buffer*/, void* output_buffer,
                                      long num_frames)
{
  const auto* data = static_cast<const WiimoteStreamData*>(user_data);
  data->self->m_mixer->MixWiimoteSpeaker(data->wiimote_index, static_cast<short*>(output_buffer),
                                         num_frames);
  return num_frames;
}

void CubebStream::WiimoteStateCallback(cubeb_stream* stream, void* user_data, cubeb_state state)
{
}

CubebStream::CubebStream()
#ifdef _WIN32
    : m_work_queue("Cubeb Worker")
{
  m_work_queue.PushBlocking([this] {
    const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    m_coinit_success = result == S_OK;
    m_should_couninit = result == S_OK || result == S_FALSE;
  });
}
#else
    = default;
#endif

bool CubebStream::Init()
{
  bool return_value = false;

#ifdef _WIN32
  if (!m_coinit_success)
    return false;
  m_work_queue.PushBlocking([this, &return_value] {
#endif
    m_ctx = CubebUtils::GetContext();
    if (m_ctx)
    {
      m_stereo = !Config::ShouldUseDPL2Decoder();

      cubeb_stream_params& params = m_params;
      params.rate = m_mixer->GetSampleRate();
      if (m_stereo)
      {
        params.channels = 2;
        params.format = CUBEB_SAMPLE_S16NE;
        params.layout = CUBEB_LAYOUT_STEREO;
      }
      else
      {
        params.channels = 6;
        params.format = CUBEB_SAMPLE_FLOAT32NE;
        params.layout = CUBEB_LAYOUT_3F2_LFE;
      }

      u32 minimum_latency = 0;
      if (cubeb_get_min_latency(m_ctx.get(), &params, &minimum_latency) != CUBEB_OK)
        ERROR_LOG_FMT(AUDIO, "Error getting minimum latency");
      INFO_LOG_FMT(AUDIO, "Minimum latency: {} frames", minimum_latency);
      m_latency = std::max(BUFFER_SAMPLES, minimum_latency);

      return_value = OpenStream();
      NOTICE_LOG_FMT(CORE, "Audio: cubeb ({}), {} Hz, {}{}", cubeb_get_backend_id(m_ctx.get()),
                     params.rate, m_stereo ? "stereo" : "surround",
                     return_value ? "" : ": could not open the output");

      // Create per-wiimote streams for audio routing (Wii games only, when enabled)
      if (return_value && Core::System::GetInstance().IsWii() &&
          Config::Get(Config::MAIN_WIIMOTE_AUDIO_ROUTING_ENABLED))
      {
        static const char* const WIIMOTE_STREAM_NAMES[4] = {
            "Dolphin Wiimote 1 Audio", "Dolphin Wiimote 2 Audio", "Dolphin Wiimote 3 Audio",
            "Dolphin Wiimote 4 Audio"};

        cubeb_stream_params wiimote_params{};
        wiimote_params.rate = m_mixer->GetSampleRate();
        wiimote_params.channels = 2;
        wiimote_params.format = CUBEB_SAMPLE_S16NE;
        wiimote_params.layout = CUBEB_LAYOUT_STEREO;

        for (std::size_t i = 0; i < m_wiimote_streams.size(); ++i)
        {
          if (!Config::Get(Config::MAIN_WIIMOTE_AUDIO_OUTPUT_ENABLED[i]))
            continue;

          const std::string device_id_str =
              Config::Get(Config::MAIN_WIIMOTE_AUDIO_OUTPUT_DEVICE[i]);
          const cubeb_devid output_devid =
              device_id_str.empty() ?
                  nullptr :
                  static_cast<cubeb_devid>(CubebUtils::GetOutputDeviceById(device_id_str));

          u32 wiimote_min_latency = 0;
          cubeb_get_min_latency(m_ctx.get(), &wiimote_params, &wiimote_min_latency);

          m_wiimote_stream_data[i] = {this, i};
          const int result = cubeb_stream_init(
              m_ctx.get(), &m_wiimote_streams[i], WIIMOTE_STREAM_NAMES[i], nullptr, nullptr,
              output_devid, &wiimote_params, std::max(BUFFER_SAMPLES, wiimote_min_latency),
              WiimoteDataCallback, WiimoteStateCallback, &m_wiimote_stream_data[i]);

          if (result != CUBEB_OK)
          {
            ERROR_LOG_FMT(AUDIO, "Failed to create Cubeb stream for Wiimote {} audio routing",
                          i + 1);
            m_wiimote_streams[i] = nullptr;
          }
        }
      }
    }

#ifdef _WIN32
  });
#endif

  return return_value;
}

bool CubebStream::OpenStream()
{
  m_failed.store(false, std::memory_order_relaxed);
  m_last_callback_us.store(NowUs(), std::memory_order_relaxed);
  if (cubeb_stream_init(m_ctx.get(), &m_stream, "Dolphin Audio Output", nullptr, nullptr, nullptr,
                        &m_params, m_latency, DataCallback, StateCallback, this) == CUBEB_OK)
  {
    return true;
  }
  m_stream = nullptr;
  return false;
}

// On the worker (Windows), as every other call on the stream: closes the stream and opens a new
// one on the system's output device of now, at the same volume, running.
void CubebStream::ReopenStream()
{
  const std::string why =
      m_failed.load(std::memory_order_relaxed) ?
          std::string("cubeb reported an error") :
          fmt::format("no samples asked for in {} ms",
                      (NowUs() - m_last_callback_us.load(std::memory_order_relaxed)) / 1000);
  if (m_stream)
  {
    cubeb_stream_stop(m_stream);
    cubeb_stream_destroy(m_stream);
    m_stream = nullptr;
  }
  const bool opened = OpenStream();
  if (opened)
    cubeb_stream_set_volume(m_stream, m_volume / 100.0f);
  if (opened && cubeb_stream_start(m_stream) == CUBEB_OK)
  {
    m_reopen_failures = 0;
    WARN_LOG_FMT(CORE, "Audio: the sound output stopped ({}); opened it again", why);
    return;
  }
  // No output device now (unplugged, disabled): try again later; said the first times only.
  ++m_reopen_failures;
  if (m_reopen_failures <= 3)
  {
    WARN_LOG_FMT(CORE, "Audio: the sound output stopped ({}) and could not be {} again{}", why,
                 opened ? "started" : "opened",
                 m_reopen_failures == 3 ? "; trying every 5 s, not logged further" : "");
  }
  m_next_reopen_us.store(NowUs() + REOPEN_RETRY_US, std::memory_order_relaxed);
}

// From the CPU thread as the game's audio comes (AudioCommon::SendAIBuffer): cheap unless the
// output has stopped.
void CubebStream::CheckOutput()
{
#ifdef _WIN32
  if (!m_output_running.load(std::memory_order_relaxed) ||
      m_reopen_queued.load(std::memory_order_relaxed))
  {
    return;
  }
  const s64 now = NowUs();
  if (!m_failed.load(std::memory_order_relaxed) &&
      now - m_last_callback_us.load(std::memory_order_relaxed) < OUTPUT_STALL_US)
  {
    return;
  }
  if (now < m_next_reopen_us.load(std::memory_order_relaxed))
    return;
  m_reopen_queued.store(true, std::memory_order_relaxed);
  m_work_queue.Push([this] {
    // Stopped in the meantime: SetRunning(true) starts the stream and CheckOutput looks again.
    if (m_running)
      ReopenStream();
    m_reopen_queued.store(false, std::memory_order_relaxed);
  });
#endif
}

bool CubebStream::SetRunning(bool running)
{
  bool return_value = false;

#ifdef _WIN32
  if (!m_coinit_success)
    return false;
  m_work_queue.PushBlocking([this, running, &return_value] {
#endif
    m_running = running;
    m_output_running.store(running, std::memory_order_relaxed);
    if (!m_stream)
    {
      // Not open (ReopenStream failed): CheckOutput tries again while running.
      return_value = true;
    }
    else if (running)
    {
      // Not asked for samples while stopped: the quiet time counts from now.
      m_last_callback_us.store(NowUs(), std::memory_order_relaxed);
      return_value = cubeb_stream_start(m_stream) == CUBEB_OK;
      for (auto& ws : m_wiimote_streams)
      {
        if (ws)
          cubeb_stream_start(ws);
      }
    }
    else
    {
      return_value = cubeb_stream_stop(m_stream) == CUBEB_OK;
      for (auto& ws : m_wiimote_streams)
      {
        if (ws)
          cubeb_stream_stop(ws);
      }
    }
#ifdef _WIN32
  });
#endif

  return return_value;
}

CubebStream::~CubebStream()
{
#ifdef _WIN32
  m_work_queue.PushBlocking([this] {
#endif
    if (m_stream)
    {
      cubeb_stream_stop(m_stream);
      cubeb_stream_destroy(m_stream);
      m_stream = nullptr;
    }
    for (auto& ws : m_wiimote_streams)
    {
      if (ws)
      {
        cubeb_stream_stop(ws);
        cubeb_stream_destroy(ws);
        ws = nullptr;
      }
    }
#ifdef _WIN32
    if (m_should_couninit)
    {
      m_should_couninit = false;
      CoUninitialize();
    }
    m_coinit_success = false;
  });
#endif
  m_ctx.reset();
}

void CubebStream::SetVolume(int volume)
{
#ifdef _WIN32
  if (!m_coinit_success)
    return;
  m_work_queue.PushBlocking([this, volume] {
#endif
    m_volume = volume;
    if (m_stream)
      cubeb_stream_set_volume(m_stream, volume / 100.0f);
#ifdef _WIN32
  });
#endif
}
