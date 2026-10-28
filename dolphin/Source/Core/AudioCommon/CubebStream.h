// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "AudioCommon/SoundStream.h"
#ifdef _WIN32
#include "Common/WorkQueueThread.h"
#endif

#ifdef HAVE_CUBEB
#include <cubeb/cubeb.h>
#endif

class CubebStream final : public SoundStream
{
#ifdef HAVE_CUBEB
public:
  CubebStream();
  CubebStream(const CubebStream& other) = delete;
  CubebStream(CubebStream&& other) = delete;
  CubebStream& operator=(const CubebStream& other) = delete;
  CubebStream& operator=(CubebStream&& other) = delete;
  ~CubebStream() override;
  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int) override;
  void CheckOutput() override;
  static bool IsValid() { return true; }

private:
  bool OpenStream();
  void ReopenStream();
  struct WiimoteStreamData
  {
    CubebStream* self;
    std::size_t wiimote_index;
  };

  bool m_stereo = false;
  std::shared_ptr<cubeb> m_ctx;
  cubeb_stream* m_stream = nullptr;
  cubeb_stream_params m_params{};
  u32 m_latency = 0;
  int m_volume = 100;
  bool m_running = false;

  // The output's health (CheckOutput): when cubeb last asked for samples (steady clock, us), and
  // whether it said the stream failed. Its WASAPI backend ends the stream when the device goes
  // away and it cannot move to another one, or when the device stops answering, sometimes without
  // telling: the game then played on in silence until Dolphin was restarted (bug report #176).
  std::atomic<s64> m_last_callback_us{0};
  std::atomic<bool> m_failed{false};
  std::atomic<bool> m_output_running{false};
  std::atomic<bool> m_reopen_queued{false};
  std::atomic<s64> m_next_reopen_us{0};
  int m_reopen_failures = 0;
  std::array<WiimoteStreamData, 4> m_wiimote_stream_data{};
  std::array<cubeb_stream*, 4> m_wiimote_streams{};

  std::vector<short> m_short_buffer;
  std::vector<float> m_floatstereo_buffer;

#ifdef _WIN32
  Common::AsyncWorkThread m_work_queue;
  bool m_coinit_success = false;
  bool m_should_couninit = false;
#endif

  static long DataCallback(cubeb_stream* stream, void* user_data, const void* /*input_buffer*/,
                           void* output_buffer, long num_frames);
  static void StateCallback(cubeb_stream* stream, void* user_data, cubeb_state state);
  static long WiimoteDataCallback(cubeb_stream* stream, void* user_data,
                                  const void* /*input_buffer*/, void* output_buffer,
                                  long num_frames);
  static void WiimoteStateCallback(cubeb_stream* stream, void* user_data, cubeb_state state);
#endif
};
