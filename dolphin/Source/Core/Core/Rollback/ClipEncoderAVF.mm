// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ClipEncoder on macOS: AVFoundation's asset writer writes the MP4 with the system's H.264 encoder
// (VideoToolbox: the GPU's media engine) and AAC encoder. Nothing ships for it: our macOS build
// has no FFmpeg, and both encoders are part of macOS. Built with ARC (Core/CMakeLists.txt).

#include "Core/Rollback/ClipEncoder.h"

#include <chrono>
#include <cstring>
#include <thread>

#import <AVFoundation/AVFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

#include <fmt/format.h>

#include "Common/Logging/Log.h"

namespace Gprb::Clip
{
namespace
{
constexpr int AUDIO_BITRATE = 192'000;
// Timestamps come in 100 ns units.
constexpr int32_t TIMESCALE = 10'000'000;
// An input that is not ready (the writer interleaves the two) is waited for this long at most.
constexpr auto READY_WAIT = std::chrono::seconds(5);

std::string Describe(NSError* error)
{
  return error ? std::string([[error localizedDescription] UTF8String]) : std::string("unknown error");
}

class AVFEncoder final : public Encoder
{
public:
  ~AVFEncoder() override
  {
    @autoreleasepool
    {
      if (m_writer && m_writer.status == AVAssetWriterStatusWriting)
        [m_writer cancelWriting];
      if (m_audio_format)
        CFRelease(m_audio_format);
      m_adaptor = nil;
      m_video = nil;
      m_audio = nil;
      m_writer = nil;
    }
  }

  bool Init(const std::string& path, int width, int height, int audio_rate, std::string* error)
  {
    @autoreleasepool
    {
      m_width = width;
      m_height = height;
      m_audio_rate = audio_rate;
      NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
      [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
      NSError* err = nil;
      m_writer = [AVAssetWriter assetWriterWithURL:url fileType:AVFileTypeMPEG4 error:&err];
      if (!m_writer)
      {
        *error = "cannot write the video file (" + Describe(err) + ")";
        return false;
      }

      NSDictionary* video_settings = @{
        AVVideoCodecKey : AVVideoCodecTypeH264,
        AVVideoWidthKey : @(width),
        AVVideoHeightKey : @(height),
        AVVideoCompressionPropertiesKey : @{
          AVVideoAverageBitRateKey : @(VideoBitrate(height)),
          AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
          AVVideoExpectedSourceFrameRateKey : @60,
          AVVideoMaxKeyFrameIntervalKey : @120,
        },
      };
      m_video = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                   outputSettings:video_settings];
      // (Frames come as fast as the game renders them; the writer must not hold back for an
      // interleaving the encoder thread's order already gives.)
      m_video.expectsMediaDataInRealTime = YES;
      NSDictionary* pixels = @{
        (__bridge id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (__bridge id)kCVPixelBufferWidthKey : @(width),
        (__bridge id)kCVPixelBufferHeightKey : @(height),
      };
      m_adaptor = [AVAssetWriterInputPixelBufferAdaptor
          assetWriterInputPixelBufferAdaptorWithAssetWriterInput:m_video
                                     sourcePixelBufferAttributes:pixels];

      AudioChannelLayout layout = {};
      layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
      NSDictionary* audio_settings = @{
        AVFormatIDKey : @(kAudioFormatMPEG4AAC),
        AVSampleRateKey : @(audio_rate),
        AVNumberOfChannelsKey : @2,
        AVEncoderBitRateKey : @(AUDIO_BITRATE),
        AVChannelLayoutKey : [NSData dataWithBytes:&layout length:sizeof(layout)],
      };
      m_audio = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                   outputSettings:audio_settings];
      m_audio.expectsMediaDataInRealTime = YES;

      if (![m_writer canAddInput:m_video] || ![m_writer canAddInput:m_audio])
      {
        *error = "macOS cannot write H.264 and AAC to MP4 here";
        return false;
      }
      [m_writer addInput:m_video];
      [m_writer addInput:m_audio];

      // The samples handed over: 16-bit stereo PCM, interleaved.
      AudioStreamBasicDescription pcm = {};
      pcm.mSampleRate = audio_rate;
      pcm.mFormatID = kAudioFormatLinearPCM;
      pcm.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
      pcm.mBytesPerPacket = 4;
      pcm.mFramesPerPacket = 1;
      pcm.mBytesPerFrame = 4;
      pcm.mChannelsPerFrame = 2;
      pcm.mBitsPerChannel = 16;
      if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &pcm, sizeof(layout), &layout, 0,
                                         nullptr, nullptr, &m_audio_format) != noErr)
      {
        *error = "cannot describe the audio";
        return false;
      }

      if (![m_writer startWriting])
      {
        *error = "cannot start the video file (" + Describe(m_writer.error) + ")";
        return false;
      }
      [m_writer startSessionAtSourceTime:kCMTimeZero];
      INFO_LOG_FMT(BRAWLBACK, "clip: AVFoundation writer");
      return true;
    }
  }

  bool WriteVideo(const u8* bgra, int stride, s64 pts, s64) override
  {
    @autoreleasepool
    {
      if (!WaitReady(m_video, "video"))
        return false;
      CVPixelBufferRef buffer = nullptr;
      if (!m_adaptor.pixelBufferPool ||
          CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, m_adaptor.pixelBufferPool,
                                             &buffer) != kCVReturnSuccess)
      {
        return Fail("no video frame buffer");
      }
      CVPixelBufferLockBaseAddress(buffer, 0);
      u8* dst = static_cast<u8*>(CVPixelBufferGetBaseAddress(buffer));
      const size_t dst_stride = CVPixelBufferGetBytesPerRow(buffer);
      for (int y = 0; y < m_height; ++y)
        std::memcpy(dst + y * dst_stride, bgra + static_cast<size_t>(y) * stride, m_width * 4);
      CVPixelBufferUnlockBaseAddress(buffer, 0);
      // (The encoder needs increasing times.)
      const s64 t = std::max(pts, m_last_video_pts + 1);
      m_last_video_pts = t;
      const BOOL ok = [m_adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(t, TIMESCALE)];
      CVPixelBufferRelease(buffer);
      return ok ? true : Fail("writing video failed (" + Describe(m_writer.error) + ")");
    }
  }

  bool WriteAudio(const s16* samples, size_t frames, s64 pts) override
  {
    @autoreleasepool
    {
      if (!frames)
        return true;
      if (!WaitReady(m_audio, "audio"))
        return false;
      const size_t bytes = frames * 4;
      CMBlockBufferRef block = nullptr;
      if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, bytes, kCFAllocatorDefault,
                                             nullptr, 0, bytes, 0, &block) != kCMBlockBufferNoErr ||
          CMBlockBufferReplaceDataBytes(samples, block, 0, bytes) != kCMBlockBufferNoErr)
      {
        if (block)
          CFRelease(block);
        return Fail("no audio buffer");
      }
      CMSampleBufferRef sample = nullptr;
      const CMTime start = CMTimeConvertScale(CMTimeMake(pts, TIMESCALE), m_audio_rate,
                                              kCMTimeRoundingMethod_RoundHalfAwayFromZero);
      const OSStatus status = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
          kCFAllocatorDefault, block, m_audio_format, static_cast<CMItemCount>(frames), start,
          nullptr, &sample);
      CFRelease(block);
      if (status != noErr)
        return Fail(fmt::format("no audio sample ({})", status));
      const BOOL ok = [m_audio appendSampleBuffer:sample];
      CFRelease(sample);
      return ok ? true : Fail("writing audio failed (" + Describe(m_writer.error) + ")");
    }
  }

  bool Finish() override
  {
    @autoreleasepool
    {
      [m_video markAsFinished];
      [m_audio markAsFinished];
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      [m_writer finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(done);
      }];
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
      if (m_writer.status != AVAssetWriterStatusCompleted)
        return Fail("the video file could not be written out (" + Describe(m_writer.error) + ")");
      return true;
    }
  }

  std::string Error() const override { return m_error; }

private:
  bool WaitReady(AVAssetWriterInput* input, const char* what)
  {
    const auto until = std::chrono::steady_clock::now() + READY_WAIT;
    while (!input.readyForMoreMediaData)
    {
      if (m_writer.status != AVAssetWriterStatusWriting)
        return Fail(fmt::format("writing {} failed ({})", what, Describe(m_writer.error)));
      if (std::chrono::steady_clock::now() > until)
        return Fail(fmt::format("the {} encoder stopped taking data", what));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  bool Fail(std::string error)
  {
    m_error = std::move(error);
    return false;
  }

  AVAssetWriter* m_writer = nil;
  AVAssetWriterInput* m_video = nil;
  AVAssetWriterInput* m_audio = nil;
  AVAssetWriterInputPixelBufferAdaptor* m_adaptor = nil;
  CMAudioFormatDescriptionRef m_audio_format = nullptr;
  s64 m_last_video_pts = -1;
  int m_width = 0;
  int m_height = 0;
  int m_audio_rate = 0;
  std::string m_error;
};
}  // namespace

std::unique_ptr<Encoder> CreateEncoder(const std::string& path, int width, int height,
                                       int audio_rate, std::string* error)
{
  auto encoder = std::make_unique<AVFEncoder>();
  if (!encoder->Init(path, width, height, audio_rate, error))
    return nullptr;
  return encoder;
}
}  // namespace Gprb::Clip
