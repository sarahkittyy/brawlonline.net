// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ClipEncoder on Windows: Media Foundation's sink writer writes the MP4 with the system's H.264
// encoder (the graphics card's when it has one, else Microsoft's software one) and AAC encoder,
// converting the BGRA frames to the encoder's format itself. Nothing ships for it: the encoders
// are part of Windows (an "N" edition needs the Media Feature Pack). mfplat.dll and mfreadwrite.dll
// are delay-loaded (Core/CMakeLists.txt): an N edition without the pack has neither, and as plain
// imports they kept Dolphin from starting at all ("Required DLLs ... are missing").

#ifdef _WIN32

#include "Core/Rollback/ClipEncoder.h"

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
    // Before the first MF call, which would load them: a missing delay-loaded DLL is an exception.
    for (const wchar_t* dll : {L"mfplat.dll", L"mfreadwrite.dll"})
    {
      if (!LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
      {
        *error = "Media Foundation is not installed (a Windows \"N\" edition needs the Media "
                 "Feature Pack)";
        return false;
      }
    }
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
    // The graphics card's encoder first, if its test picture comes back right; then Windows'
    // software one.
    for (const bool hardware : {true, false})
    {
      if (hardware && !HardwareEncodesRight())
        continue;
      hr = Create(UTF8ToWString(path).c_str(), hardware, true);
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
  // Whether the graphics card's encoder, set up as for the video, writes a picture that decodes
  // back to it: bug report #305 (an AMD Radeon RX 9070 XT beside a Ryzen's graphics) got videos
  // of a flickering green screen from it, with the sound right. Writes TEST_FRAMES frames of
  // white and black quadrants to a temporary file, reads the last one back with Windows' software
  // decoder and compares the quadrants' brightness. Tested once per size a process.
  bool HardwareEncodesRight()
  {
    static std::mutex s_mutex;
    static std::map<std::pair<int, int>, bool> s_results;
    std::lock_guard lk(s_mutex);
    const auto key = std::make_pair(m_width, m_height);
    if (const auto it = s_results.find(key); it != s_results.end())
      return it->second;
    std::string why;
    const ProbeResult result = ProbeHardware(&why);
    if (result == ProbeResult::Wrong)
      WARN_LOG_FMT(BRAWLBACK,
                   "clip: the hardware encoder's test picture came back wrong ({}); using the "
                   "software encoder",
                   why);
    else if (result == ProbeResult::Unknown)
      WARN_LOG_FMT(BRAWLBACK, "clip: could not test the hardware encoder ({})", why);
    else
      INFO_LOG_FMT(BRAWLBACK, "clip: the hardware encoder's test picture came back right ({})",
                   why);
    const bool ok = result != ProbeResult::Wrong;
    s_results.emplace(key, ok);
    return ok;
  }

  enum class ProbeResult
  {
    Right,
    Wrong,
    Unknown,  // the test itself could not run (no decoder, ...): the encoder is used as before
  };

  ProbeResult ProbeHardware(std::string* why)
  {
    constexpr int TEST_FRAMES = 30;
    // A file in the temporary folder, not beside the video (a byte stream on memory from
    // MFCreateMFByteStreamOnStream is not writable).
    wchar_t dir[MAX_PATH + 1];
    const DWORD dir_length = GetTempPathW(MAX_PATH + 1, dir);
    if (dir_length == 0 || dir_length > MAX_PATH)
    {
      *why = "no temporary folder";
      return ProbeResult::Unknown;
    }
    const std::wstring test_path = std::wstring(dir) + L"brawl-online-clip-test-" +
                                   std::to_wstring(GetCurrentProcessId()) + L".mp4";
    const ProbeResult result = ProbeHardwareInto(test_path, TEST_FRAMES, why);
    DeleteFileW(test_path.c_str());
    return result;
  }

  ProbeResult ProbeHardwareInto(const std::wstring& test_path, int frames, std::string* why)
  {
    // No writer at all: Init's own attempt fails the same way and goes on to the software one.
    HRESULT hr = Create(test_path.c_str(), true, false);
    if (FAILED(hr))
    {
      m_writer.Reset();
      *why = "no writer " + HrText(hr);
      return ProbeResult::Unknown;
    }
    std::vector<u8> picture(static_cast<size_t>(m_width) * m_height * 4);
    for (int y = 0; y < m_height; ++y)
    {
      for (int x = 0; x < m_width; ++x)
      {
        const u8 v = QuadrantIsWhite(x, y) ? 255 : 0;
        u8* p = picture.data() + (static_cast<size_t>(y) * m_width + x) * 4;
        p[0] = p[1] = p[2] = v;
        p[3] = 255;
      }
    }
    constexpr s64 FRAME = s64{10'000'000} * 1001 / 60000;
    for (int i = 0; i < frames && SUCCEEDED(hr); ++i)
    {
      if (!WriteVideo(picture.data(), m_width * 4, i * FRAME, FRAME))
        hr = E_FAIL;
    }
    if (SUCCEEDED(hr))
      hr = m_writer->Finalize();
    m_writer.Reset();
    m_error.clear();
    if (FAILED(hr))
    {
      *why = "writing " + HrText(hr);
      return ProbeResult::Wrong;
    }

    // Read back with Windows' software decoder (no MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS).
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromURL(test_path.c_str(), nullptr, &reader);
    ComPtr<IMFMediaType> nv12;
    if (SUCCEEDED(hr))
      hr = MFCreateMediaType(&nv12);
    if (SUCCEEDED(hr))
    {
      nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
      hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nv12.Get());
    }
    if (FAILED(hr))
    {
      *why = "decoder " + HrText(hr);
      return ProbeResult::Unknown;
    }
    ComPtr<IMFSample> last;
    int decoded = 0;
    for (;;)
    {
      DWORD flags = 0;
      ComPtr<IMFSample> sample;
      hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr,
                              &sample);
      if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)))
        break;
      if (sample)
      {
        last = sample;
        ++decoded;
      }
    }
    if (!last)
    {
      *why = fmt::format("no frame decoded {}", HrText(hr));
      return ProbeResult::Wrong;
    }
    ComPtr<IMFMediaType> type;
    UINT32 width = 0, height = 0;
    if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type)))
      MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (static_cast<int>(width) < m_width || static_cast<int>(height) < m_height)
    {
      *why = fmt::format("decoded {}x{}", width, height);
      return ProbeResult::Wrong;
    }
    ComPtr<IMFMediaBuffer> buffer;
    hr = last->GetBufferByIndex(0, &buffer);
    if (FAILED(hr))
    {
      *why = "frame buffer " + HrText(hr);
      return ProbeResult::Unknown;
    }
    ComPtr<IMF2DBuffer> buffer_2d;
    BYTE* luma = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    const bool is_2d = SUCCEEDED(buffer.As(&buffer_2d));
    if (is_2d)
      hr = buffer_2d->Lock2D(&luma, &pitch);
    else
    {
      hr = buffer->Lock(&luma, nullptr, &length);
      pitch = static_cast<LONG>(width);
      if (SUCCEEDED(hr) && length < static_cast<DWORD>(width) * height)
      {
        buffer->Unlock();
        hr = E_UNEXPECTED;
      }
    }
    if (FAILED(hr))
    {
      *why = "frame lock " + HrText(hr);
      return ProbeResult::Unknown;
    }
    // Each quadrant's middle half: limited-range white is 235, black 16.
    int means[4] = {};
    bool right = true;
    for (int q = 0; q < 4; ++q)
    {
      const int qx = (q & 1) * m_width / 2, qy = (q >> 1) * m_height / 2;
      const int qw = m_width / 2, qh = m_height / 2;
      u64 sum = 0, count = 0;
      for (int y = qy + qh / 4; y < qy + qh * 3 / 4; ++y)
      {
        const BYTE* row = luma + static_cast<ptrdiff_t>(y) * pitch;
        for (int x = qx + qw / 4; x < qx + qw * 3 / 4; ++x, ++count)
          sum += row[x];
      }
      means[q] = count ? static_cast<int>(sum / count) : 0;
      const bool white = QuadrantIsWhite(qx, qy);
      if (white ? means[q] < 160 : means[q] > 90)
        right = false;
    }
    if (is_2d)
      buffer_2d->Unlock2D();
    else
      buffer->Unlock();
    *why = fmt::format("{} frames back, quadrant brightness {} {} / {} {}", decoded, means[0],
                       means[1], means[2], means[3]);
    return right ? ProbeResult::Right : ProbeResult::Wrong;
  }

  // The test picture: top left and bottom right white, the others black.
  bool QuadrantIsWhite(int x, int y) const { return (x < m_width / 2) == (y < m_height / 2); }

  // The writer to the file `path`; `audio` adds the sound stream.
  HRESULT Create(const wchar_t* path, bool hardware, bool audio)
  {
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 3);
    if (FAILED(hr))
      return hr;
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    hr = MFCreateSinkWriterFromURL(path, nullptr, attrs.Get(), &m_writer);
    if (FAILED(hr))
      return hr;

    // Video: H.264 High, progressive, 60000/1001 frames a second; fed BGRA (RGB32), top-down.
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(VideoBitrate(m_height)));
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
    // No B-frames: Microsoft's software encoder makes them otherwise, and the files have no edit
    // list for the delay they add (mp4_concat.ts counts on that, docs/design/highlights.md).
    ComPtr<IMFAttributes> params;
    hr = MFCreateAttributes(&params, 1);
    if (FAILED(hr))
      return hr;
    params->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    hr = m_writer->SetInputMediaType(m_video, in.Get(), params.Get());
    if (FAILED(hr))
      return hr;
    if (!audio)
      return m_writer->BeginWriting();

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
