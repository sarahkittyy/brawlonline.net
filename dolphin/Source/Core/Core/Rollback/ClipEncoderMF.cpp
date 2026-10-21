// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ClipEncoder on Windows: Media Foundation's sink writer writes the MP4 with the system's H.264
// encoder (the graphics card's when it has one, else Microsoft's software one) and AAC encoder,
// converting the BGRA frames to the encoder's format itself. Nothing ships for it: the encoders
// are part of Windows (an "N" edition needs the Media Feature Pack).

#ifdef _WIN32

#include "Core/Rollback/ClipEncoder.h"

#include <cstring>

#include <Windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

using Microsoft::WRL::ComPtr;

namespace Gprb::Clip
{
namespace
{
constexpr UINT32 VIDEO_BITRATE = 12'000'000;  // 720p60 that stays sharp in motion
constexpr UINT32 AUDIO_BYTES_PER_SECOND = 24'000;  // 192 kbit/s AAC

std::string HrText(HRESULT hr)
{
  return fmt::format("0x{:08X}", static_cast<u32>(hr));
}

class MFEncoder final : public Encoder
{
public:
  ~MFEncoder() override
  {
    m_writer.Reset();
    if (m_started)
      MFShutdown();
    if (m_com)
      CoUninitialize();
  }

  bool Init(const std::string& path, int width, int height, int audio_rate, std::string* error)
  {
    m_width = width;
    m_height = height;
    m_audio_rate = audio_rate;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_com = SUCCEEDED(co);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr))
    {
      *error = "Media Foundation is not available (" + HrText(hr) +
               "; a Windows \"N\" edition needs the Media Feature Pack)";
      return false;
    }
    m_started = true;
    // The graphics card's encoder first, then Windows' software one.
    for (const bool hardware : {true, false})
    {
      hr = Create(path, hardware);
      if (SUCCEEDED(hr))
      {
        INFO_LOG_FMT(BRAWLBACK, "clip: Media Foundation writer ({} encoders)",
                     hardware ? "hardware" : "software");
        return true;
      }
      WARN_LOG_FMT(BRAWLBACK, "clip: Media Foundation {} writer failed: {}",
                   hardware ? "hardware" : "software", HrText(hr));
      m_writer.Reset();
    }
    *error = "Windows could not make an H.264 video writer (" + HrText(hr) + ")";
    return false;
  }

  bool WriteVideo(const u8* bgra, int stride, s64 pts, s64 duration) override
  {
    const DWORD size = static_cast<DWORD>(m_width * m_height * 4);
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(size, &buffer);
    BYTE* data = nullptr;
    if (SUCCEEDED(hr))
      hr = buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr))
    {
      for (int y = 0; y < m_height; ++y)
        std::memcpy(data + static_cast<size_t>(y) * m_width * 4, bgra + static_cast<size_t>(y) * stride, m_width * 4);
      buffer->Unlock();
      hr = buffer->SetCurrentLength(size);
    }
    return Write(hr, buffer, m_video, pts, duration, "video");
  }

  bool WriteAudio(const s16* samples, size_t frames, s64 pts) override
  {
    const DWORD size = static_cast<DWORD>(frames * 4);
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(size, &buffer);
    BYTE* data = nullptr;
    if (SUCCEEDED(hr))
      hr = buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr))
    {
      std::memcpy(data, samples, size);
      buffer->Unlock();
      hr = buffer->SetCurrentLength(size);
    }
    const s64 duration = static_cast<s64>(frames) * 10'000'000 / m_audio_rate;
    return Write(hr, buffer, m_audio, pts, duration, "audio");
  }

  bool Finish() override
  {
    if (!m_writer)
      return false;
    const HRESULT hr = m_writer->Finalize();
    m_writer.Reset();
    if (FAILED(hr))
    {
      m_error = "the video file could not be written out (" + HrText(hr) + ")";
      return false;
    }
    return true;
  }

  std::string Error() const override { return m_error; }

private:
  HRESULT Create(const std::string& path, bool hardware)
  {
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 3);
    if (FAILED(hr))
      return hr;
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    hr = MFCreateSinkWriterFromURL(UTF8ToWString(path).c_str(), nullptr, attrs.Get(), &m_writer);
    if (FAILED(hr))
      return hr;

    // Video: H.264 High, progressive, 60000/1001 frames a second; fed BGRA (RGB32), top-down.
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, VIDEO_BITRATE);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, 60000, 1001);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = m_writer->AddStream(out.Get(), &m_video);
    if (FAILED(hr))
      return hr;
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(m_width * 4));
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, 60000, 1001);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = m_writer->SetInputMediaType(m_video, in.Get(), nullptr);
    if (FAILED(hr))
      return hr;

    // Audio: AAC-LC stereo at the given rate; fed 16-bit PCM.
    ComPtr<IMFMediaType> aout;
    MFCreateMediaType(&aout);
    aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(m_audio_rate));
    aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AUDIO_BYTES_PER_SECOND);
    aout->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    hr = m_writer->AddStream(aout.Get(), &m_audio);
    if (FAILED(hr))
      return hr;
    ComPtr<IMFMediaType> ain;
    MFCreateMediaType(&ain);
    ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(m_audio_rate));
    ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(m_audio_rate * 4));
    hr = m_writer->SetInputMediaType(m_audio, ain.Get(), nullptr);
    if (FAILED(hr))
      return hr;
    return m_writer->BeginWriting();
  }

  bool Write(HRESULT hr, const ComPtr<IMFMediaBuffer>& buffer, DWORD stream, s64 pts, s64 duration,
             const char* what)
  {
    ComPtr<IMFSample> sample;
    if (SUCCEEDED(hr))
      hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr))
      hr = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(hr))
      hr = sample->SetSampleTime(pts);
    if (SUCCEEDED(hr))
      hr = sample->SetSampleDuration(duration);
    if (SUCCEEDED(hr))
      hr = m_writer->WriteSample(stream, sample.Get());
    if (FAILED(hr))
    {
      m_error = fmt::format("writing {} failed ({})", what, HrText(hr));
      return false;
    }
    return true;
  }

  ComPtr<IMFSinkWriter> m_writer;
  DWORD m_video = 0;
  DWORD m_audio = 0;
  int m_width = 0;
  int m_height = 0;
  int m_audio_rate = 0;
  bool m_com = false;
  bool m_started = false;
  std::string m_error;
};
}  // namespace

std::unique_ptr<Encoder> CreateEncoder(const std::string& path, int width, int height,
                                       int audio_rate, std::string* error)
{
  auto encoder = std::make_unique<MFEncoder>();
  if (!encoder->Init(path, width, height, audio_rate, error))
    return nullptr;
  return encoder;
}
}  // namespace Gprb::Clip

#endif
