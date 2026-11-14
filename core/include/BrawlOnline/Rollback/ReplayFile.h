// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Brawl Online replay files (.brep; docs/design/replays.md §4).
//
// A chunked container: a 32-byte header, then self-delimiting chunks with a CRC each, then a
// 16-byte trailer once the game has ended. Every complete chunk is valid on its own, so a file
// cut short (a crash, a full disk) still reads up to its last complete chunk.
//
//   header   magic "\x89BRP\r\n\x1A\n", u16 major, u16 minor, u32 header size, u8 game_uid[16]
//   chunk    u32 type (FourCC), u32 flags, u32 stored length, u32 raw length, u32 CRC-32 of the
//            stored bytes, then the stored bytes (deflate-compressed when flags has FLAG_DEFLATE)
//   trailer  "BRPT", u32 offset of the ENDG chunk, u32 last frame, u32 CRC-32 of these 12 bytes
//
// Container integers are little-endian; guest bytes (pad slots, fighter memory) keep the guest's
// big-endian order. A file comes from a stranger once it is shared: everything here is checked
// with hard limits before use (docs/design/replays.md §8), and nothing read from a file reaches
// the game except through the checks a live joiner applies to its host (PeerData.h).

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

namespace Gprb::Replay
{
constexpr std::array<u8, 8> MAGIC{0x89, 'B', 'R', 'P', '\r', '\n', 0x1A, '\n'};
constexpr u16 FORMAT_MAJOR = 1;
// A reader accepts a higher minor and skips the chunks it does not know.
constexpr u16 FORMAT_MINOR = 0;
constexpr u32 HEADER_SIZE = 32;
constexpr u32 CHUNK_HEADER_SIZE = 20;
constexpr u32 TRAILER_SIZE = 16;
constexpr char EXTENSION[] = ".brep";

constexpr u32 FourCC(const char (&s)[5])
{
  return u32(u8(s[0])) | u32(u8(s[1])) << 8 | u32(u8(s[2])) << 16 | u32(u8(s[3])) << 24;
}

// Chunk types, in file order.
constexpr u32 CHUNK_INFO = FourCC("INFO");  // UTF-8 JSON: the metadata the launcher indexes
constexpr u32 CHUNK_SETP = FourCC("SETP");  // UTF-8 JSON: what playback needs to rebuild the match
constexpr u32 CHUNK_FTSP = FourCC("FTSP");  // UTF-8 JSON: the layout of the FTRM blocks
constexpr u32 CHUNK_FRMS = FourCC("FRMS");  // frames: inputs and checksums (binary, deflate)
constexpr u32 CHUNK_FTRM = FourCC("FTRM");  // frames: raw fighter memory (binary, deflate)
constexpr u32 CHUNK_EVNT = FourCC("EVNT");  // UTF-8 JSON: events (live desyncs, players leaving)
constexpr u32 CHUNK_ENDG = FourCC("ENDG");  // UTF-8 JSON: the result
constexpr u32 TRAILER_MAGIC = FourCC("BRPT");

constexpr u32 FLAG_DEFLATE = 1;
// A reader that does not know a critical chunk refuses the file instead of skipping it.
constexpr u32 FLAG_CRITICAL = 2;

// Limits (docs/design/replays.md §8.1).
constexpr u64 MAX_FILE_SIZE = 64ull << 20;
constexpr u32 MAX_CHUNKS = 20000;
constexpr u32 MAX_INFO = 64 << 10;
constexpr u32 MAX_SETP = 256 << 10;
constexpr u32 MAX_FTSP = 16 << 10;
constexpr u32 MAX_FRAME_CHUNK = 1 << 20;   // raw FRMS
constexpr u32 MAX_FTRM_CHUNK = 8 << 20;    // raw FTRM (60 frames of 4 fighters' memory)
constexpr u32 MAX_EVNT = 64 << 10;
constexpr u32 MAX_ENDG = 16 << 10;
constexpr u32 MAX_OTHER = 1 << 20;         // a chunk type this reader does not know
constexpr u32 FRAMES_PER_CHUNK = 60;
constexpr u32 MAX_FRAMES = 60 * 60 * 60;   // an hour
constexpr u32 MAX_FIGHTER_BLOCK = 16 << 10;

constexpr int PORTS = 4;
constexpr u32 PAD_SIZE = 0x40;
using Slots = std::array<u8, PORTS * PAD_SIZE>;

// Every PARTS_EVERY frames, the checksum's parts are recorded too (desync reports).
constexpr u32 PARTS_EVERY = 60;
struct Parts
{
  u32 frame = 0;
  std::array<u32, 3> rng{};
  u32 fighters_crc = 0;
};

// One FRMS chunk: inputs and checksums of `count` consecutive session frames.
struct FrameChunk
{
  u32 first_frame = 0;
  u8 ports = 0;                 // the ports with input in the chunk (a bit per in-game port)
  std::vector<Slots> pads;     // per frame; ports outside `ports` read "no controller"
  std::vector<u32> checksums;   // per frame
  std::vector<Parts> parts;
};
std::vector<u8> EncodeFrames(const FrameChunk& c);
std::optional<FrameChunk> DecodeFrames(std::span<const u8> raw, std::string* error);

// One FTRM chunk: `block` bytes of raw fighter memory per frame and port (FTSP says what they are),
// each frame XORed with the previous frame of the chunk, so that what did not change compresses
// to almost nothing.
struct FighterChunk
{
  u32 first_frame = 0;
  u32 count = 0;
  u8 ports = 0;
  u32 block = 0;
  std::vector<std::vector<u8>> data;  // per port in `ports` (ascending): count * block bytes, plain
};
std::vector<u8> EncodeFighters(const FighterChunk& c);
std::optional<FighterChunk> DecodeFighters(std::span<const u8> raw, std::string* error);

// The no-controller slot of a port nobody plays (gfPadError NO_CONTROLLER at +0x38).
void SetNoController(Slots* pads, int port);

// ---- Writing (the recorder's writer thread) ----

// One whole chunk as the file stores it: the 20-byte header, then the stored bytes (deflated when
// `flags` has FLAG_DEFLATE). nullopt if compression fails.
std::optional<std::vector<u8>> EncodeChunk(u32 type, u32 flags, std::span<const u8> raw);

class Writer
{
public:
  Writer() = default;
  ~Writer();
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  // Creates `path` (it must not exist) and writes the header.
  bool Open(const std::string& path, const std::array<u8, 16>& uid, std::string* error);
  bool IsOpen() const { return m_file.IsOpen(); }
  // Appends one chunk (compressed with FLAG_DEFLATE) and flushes the file to the OS.
  bool Append(u32 type, u32 flags, std::span<const u8> raw);
  // Appends a chunk EncodeChunk made (the same bytes go to spectators: Online/Spectate.h).
  bool AppendChunk(std::span<const u8> chunk);
  // ENDG, then the trailer; flushes to the disk and closes.
  bool Finish(std::span<const u8> endg, u32 last_frame);
  void Close();
  u64 BytesWritten() const { return m_offset; }

private:
  bool WriteAll(const void* data, size_t size);
  ::File::IOFile m_file;
  u64 m_offset = 0;
};

// ---- Reading ----

struct Event
{
  u32 frame = 0;
  std::string text;  // JSON object (as written)
};

struct File
{
  std::array<u8, 16> uid{};
  u16 minor = 0;
  std::string info;   // JSON
  std::string setup;  // JSON
  std::string fighter_spec;  // JSON ("" without FTSP)
  std::string endg;   // JSON ("" while the game is in progress or after a crash)
  // Frames from the first FRMS chunk on, contiguous: session frame first_frame + i.
  u32 first_frame = 0;
  u8 ports = 0;
  std::vector<Slots> pads;
  std::vector<u32> checksums;
  std::vector<Parts> parts;
  // Raw fighter memory (only with ReadOptions::fighters).
  u32 fighter_block = 0;
  u8 fighter_ports = 0;
  std::vector<std::vector<u8>> fighters;  // per port in fighter_ports: frames * fighter_block
  u32 fighter_frames = 0;
  std::vector<Event> events;
  bool trailer = false;    // ended normally (the trailer and ENDG are valid)
  bool truncated = false;  // a torn or damaged chunk ended the read early
  std::string truncated_why;
  u64 size = 0;
};

struct ReadOptions
{
  bool frames = true;     // decode FRMS
  bool fighters = false;  // decode FTRM
};

// Reads and checks a whole file. nullopt (with `error`) when it cannot be played or indexed at
// all: not a replay, another major version, too large, a damaged header or first chunks, an
// unknown critical chunk, frames that do not follow each other.
std::optional<File> Parse(std::span<const u8> bytes, const ReadOptions& options, std::string* error);
std::optional<File> ReadFile(const std::string& path, const ReadOptions& options, std::string* error);

// ---- Helpers ----

// The bytes of a JSON text that are safe to give picojson (PeerData::JsonSafeToParse), parsed into
// an object, else nullopt.
std::optional<std::string> SafeJson(std::span<const u8> bytes, size_t max_size);

// A file name part made from text another player chose (a display name): printable characters
// only, none of < > : " / \ | ? * and no leading or trailing dots and spaces, at most `max_chars`
// UTF-8 characters. Empty when nothing is left.
std::string FileNamePart(const std::string& text, size_t max_chars);

}  // namespace Gprb::Replay
