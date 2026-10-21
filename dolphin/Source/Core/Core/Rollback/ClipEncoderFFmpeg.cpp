// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ClipEncoder on macOS and Linux: FFmpeg as Dolphin links it there (the system's or the bundled
// libraries), H.264 with the first encoder it has (VideoToolbox on macOS, libx264 or OpenH264 on
// Linux; MPEG-4 Part 2 when none), AAC audio, MP4. Windows uses Media Foundation
// (ClipEncoderMF.cpp): Dolphin's own Windows FFmpeg has neither H.264 nor MP4.

#if !defined(_WIN32)

#include "Core/Rollback/ClipEncoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <fmt/format.h>

#include "Common/Logging/Log.h"

#if defined(HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace Gprb::Clip
{
#if defined(HAVE_FFMPEG)
namespace
{
constexpr int VIDEO_BITRATE = 12'000'000;
constexpr int AUDIO_BITRATE = 192'000;
// Video timestamps: 90 kHz (MP4's usual), from 100 ns units.
constexpr AVRational VIDEO_TB{1, 90000};

std::string AvError(int err)
{
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(err, buf, sizeof(buf));
  return buf;
}

class FFmpegEncoder final : public Encoder
{
public:
  ~FFmpegEncoder() override
  {
    if (m_format && m_format->pb && !m_finished)
      avio_closep(&m_format->pb);
    av_frame_free(&m_vframe);
    av_frame_free(&m_aframe);
    av_packet_free(&m_packet);
    avcodec_free_context(&m_vctx);
    avcodec_free_context(&m_actx);
    sws_freeContext(m_sws);
    avformat_free_context(m_format);
  }

  bool Init(const std::string& path, int width, int height, int audio_rate, std::string* error)
  {
    m_width = width;
    m_height = height;
    m_audio_rate = audio_rate;
    int err = avformat_alloc_output_context2(&m_format, nullptr, "mp4", path.c_str());
    if (err < 0 || !m_format)
    {
      *error = "this build's FFmpeg cannot write MP4 (" + AvError(err) + ")";
      return false;
    }
    if (!OpenVideo(error) || !OpenAudio(error))
      return false;
    err = avio_open(&m_format->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (err < 0)
    {
      *error = "cannot write " + path + " (" + AvError(err) + ")";
      return false;
    }
    err = avformat_write_header(m_format, nullptr);
    if (err < 0)
    {
      *error = "cannot start the video file (" + AvError(err) + ")";
      return false;
    }
    m_packet = av_packet_alloc();
    return true;
  }

  bool WriteVideo(const u8* bgra, int stride, s64 pts, s64) override
  {
    if (av_frame_make_writable(m_vframe) < 0)
      return Fail("video frame");
    const u8* src[1] = {bgra};
    const int src_stride[1] = {stride};
    sws_scale(m_sws, src, src_stride, 0, m_height, m_vframe->data, m_vframe->linesize);
    const s64 t = av_rescale_q(pts, AVRational{1, 10'000'000}, m_vctx->time_base);
    // (A frame at the same time as the one before: the encoder needs increasing times.)
    m_vframe->pts = std::max(t, m_last_vpts + 1);
    m_last_vpts = m_vframe->pts;
    return Send(m_vctx, m_vstream, m_vframe);
  }

  bool WriteAudio(const s16* samples, size_t frames, s64 pts) override
  {
    if (m_audio_next < 0)
      m_audio_next = av_rescale_q(pts, AVRational{1, 10'000'000}, AVRational{1, m_audio_rate});
    for (size_t i = 0; i < frames; ++i)
    {
      m_audio_l.push_back(samples[i * 2] / 32768.0f);
      m_audio_r.push_back(samples[i * 2 + 1] / 32768.0f);
    }
    return SendAudio(false);
  }

  bool Finish() override
  {
    if (!SendAudio(true) || !Send(m_vctx, m_vstream, nullptr) || !Send(m_actx, m_astream, nullptr))
      return false;
    const int err = av_write_trailer(m_format);
    avio_closep(&m_format->pb);
    m_finished = true;
    if (err < 0)
    {
      m_error = "the video file could not be written out (" + AvError(err) + ")";
      return false;
    }
    return true;
  }

  std::string Error() const override { return m_error; }

private:
  bool OpenVideo(std::string* error)
  {
    const AVCodec* codec = nullptr;
    for (const char* name : {"h264_videotoolbox", "libx264", "libopenh264", "h264_mf", "mpeg4"})
    {
      if ((codec = avcodec_find_encoder_by_name(name)))
        break;
    }
    if (!codec)
    {
      *error = "this build's FFmpeg has no video encoder for MP4";
      return false;
    }
    m_vstream = avformat_new_stream(m_format, nullptr);
    m_vctx = avcodec_alloc_context3(codec);
    m_vctx->width = m_width;
    m_vctx->height = m_height;
    m_vctx->time_base = VIDEO_TB;
    m_vctx->framerate = AVRational{60000, 1001};
    m_vctx->bit_rate = VIDEO_BITRATE;
    m_vctx->gop_size = 120;
    m_vctx->pix_fmt = codec->pix_fmts ? codec->pix_fmts[0] : AV_PIX_FMT_YUV420P;
    for (const AVPixelFormat* f = codec->pix_fmts; f && *f != AV_PIX_FMT_NONE; ++f)
    {
      if (*f == AV_PIX_FMT_YUV420P || *f == AV_PIX_FMT_NV12)
      {
        m_vctx->pix_fmt = *f;
        break;
      }
    }
    if (m_format->oformat->flags & AVFMT_GLOBALHEADER)
      m_vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (std::strcmp(codec->name, "libx264") == 0)
      av_opt_set(m_vctx->priv_data, "preset", "veryfast", 0);
    int err = avcodec_open2(m_vctx, codec, nullptr);
    if (err < 0)
    {
      *error = fmt::format("cannot open the {} encoder ({})", codec->name, AvError(err));
      return false;
    }
    avcodec_parameters_from_context(m_vstream->codecpar, m_vctx);
    m_vstream->time_base = m_vctx->time_base;
    m_vframe = av_frame_alloc();
    m_vframe->format = m_vctx->pix_fmt;
    m_vframe->width = m_width;
    m_vframe->height = m_height;
    av_frame_get_buffer(m_vframe, 0);
    m_sws = sws_getContext(m_width, m_height, AV_PIX_FMT_BGRA, m_width, m_height, m_vctx->pix_fmt,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
    INFO_LOG_FMT(BRAWLBACK, "clip: FFmpeg video encoder {}", codec->name);
    return m_sws != nullptr;
  }

  bool OpenAudio(std::string* error)
  {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec)
    {
      *error = "this build's FFmpeg has no AAC encoder";
      return false;
    }
    m_astream = avformat_new_stream(m_format, nullptr);
    m_actx = avcodec_alloc_context3(codec);
    m_actx->sample_rate = m_audio_rate;
    m_actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    m_actx->bit_rate = AUDIO_BITRATE;
    m_actx->time_base = AVRational{1, m_audio_rate};
    av_channel_layout_default(&m_actx->ch_layout, 2);
    if (m_format->oformat->flags & AVFMT_GLOBALHEADER)
      m_actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    const int err = avcodec_open2(m_actx, codec, nullptr);
    if (err < 0)
    {
      *error = "cannot open the AAC encoder (" + AvError(err) + ")";
      return false;
    }
    avcodec_parameters_from_context(m_astream->codecpar, m_actx);
    m_astream->time_base = m_actx->time_base;
    m_aframe = av_frame_alloc();
    m_aframe->format = AV_SAMPLE_FMT_FLTP;
    m_aframe->sample_rate = m_audio_rate;
    m_aframe->nb_samples = m_actx->frame_size > 0 ? m_actx->frame_size : 1024;
    av_channel_layout_copy(&m_aframe->ch_layout, &m_actx->ch_layout);
    av_frame_get_buffer(m_aframe, 0);
    return true;
  }

  bool SendAudio(bool all)
  {
    const size_t n = static_cast<size_t>(m_aframe->nb_samples);
    while (m_audio_l.size() >= n || (all && !m_audio_l.empty()))
    {
      if (av_frame_make_writable(m_aframe) < 0)
        return Fail("audio frame");
      const size_t take = std::min(n, m_audio_l.size());
      auto* l = reinterpret_cast<float*>(m_aframe->data[0]);
      auto* r = reinterpret_cast<float*>(m_aframe->data[1]);
      std::fill(l, l + n, 0.0f);
      std::fill(r, r + n, 0.0f);
      std::copy_n(m_audio_l.begin(), take, l);
      std::copy_n(m_audio_r.begin(), take, r);
      m_audio_l.erase(m_audio_l.begin(), m_audio_l.begin() + take);
      m_audio_r.erase(m_audio_r.begin(), m_audio_r.begin() + take);
      m_aframe->pts = m_audio_next;
      m_audio_next += static_cast<s64>(n);
      if (!Send(m_actx, m_astream, m_aframe))
        return false;
    }
    return true;
  }

  bool Send(AVCodecContext* ctx, AVStream* stream, AVFrame* frame)
  {
    int err = avcodec_send_frame(ctx, frame);
    if (err < 0 && err != AVERROR_EOF)
      return Fail("encoding", err);
    for (;;)
    {
      err = avcodec_receive_packet(ctx, m_packet);
      if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
        return true;
      if (err < 0)
        return Fail("encoding", err);
      av_packet_rescale_ts(m_packet, ctx->time_base, stream->time_base);
      m_packet->stream_index = stream->index;
      err = av_interleaved_write_frame(m_format, m_packet);
      if (err < 0)
        return Fail("writing", err);
    }
  }

  bool Fail(const char* what, int err = 0)
  {
    m_error = err ? fmt::format("{} failed ({})", what, AvError(err)) : fmt::format("{} failed", what);
    return false;
  }

  AVFormatContext* m_format = nullptr;
  AVStream* m_vstream = nullptr;
  AVStream* m_astream = nullptr;
  AVCodecContext* m_vctx = nullptr;
  AVCodecContext* m_actx = nullptr;
  AVFrame* m_vframe = nullptr;
  AVFrame* m_aframe = nullptr;
  AVPacket* m_packet = nullptr;
  SwsContext* m_sws = nullptr;
  std::vector<float> m_audio_l, m_audio_r;
  s64 m_audio_next = -1;
  s64 m_last_vpts = -1;
  int m_width = 0, m_height = 0, m_audio_rate = 0;
  bool m_finished = false;
  std::string m_error;
};
}  // namespace

std::unique_ptr<Encoder> CreateEncoder(const std::string& path, int width, int height,
                                       int audio_rate, std::string* error)
{
  auto encoder = std::make_unique<FFmpegEncoder>();
  if (!encoder->Init(path, width, height, audio_rate, error))
    return nullptr;
  return encoder;
}
#else
std::unique_ptr<Encoder> CreateEncoder(const std::string&, int, int, int, std::string* error)
{
  *error = "this Dolphin was built without FFmpeg, so it cannot write video";
  return nullptr;
}
#endif
}  // namespace Gprb::Clip

#endif
