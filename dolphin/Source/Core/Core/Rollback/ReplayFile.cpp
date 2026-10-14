// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/ReplayFile.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>
#include <picojson.h>
#include <zlib.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/Rollback/PeerData.h"

namespace Gprb::Replay
{
namespace
{
void PutU16(std::vector<u8>& out, u16 v)
{
  out.push_back(static_cast<u8>(v));
  out.push_back(static_cast<u8>(v >> 8));
}

void PutU32(std::vector<u8>& out, u32 v)
{
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>(v >> (8 * i)));
}

u16 GetU16(const u8* p)
{
  return static_cast<u16>(p[0] | p[1] << 8);
}

u32 GetU32(const u8* p)
{
  return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24;
}

u32 Crc(std::span<const u8> data)
{
  uLong crc = crc32(0L, Z_NULL, 0);
  // zlib takes a uInt length: feed large spans in pieces.
  size_t off = 0;
  while (off < data.size())
  {
    const uInt n = static_cast<uInt>(std::min<size_t>(data.size() - off, 1u << 30));
    crc = crc32(crc, data.data() + off, n);
    off += n;
  }
  return static_cast<u32>(crc);
}

std::optional<std::vector<u8>> Deflate(std::span<const u8> raw)
{
  uLongf size = compressBound(static_cast<uLong>(raw.size()));
  std::vector<u8> out(size);
  if (compress2(out.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
    return std::nullopt;
  out.resize(size);
  return out;
}

// Exactly `raw_len` bytes out, or nothing: the output buffer bounds the work (no bombs).
std::optional<std::vector<u8>> Inflate(std::span<const u8> stored, u32 raw_len)
{
  std::vector<u8> out(raw_len);
  uLongf size = raw_len;
  uLong in = static_cast<uLong>(stored.size());
  const int r = uncompress2(out.data(), &size, stored.data(), &in);
  if (r != Z_OK || size != raw_len || in != stored.size())
    return std::nullopt;
  return out;
}

u32 MaxRaw(u32 type)
{
  switch (type)
  {
  case CHUNK_INFO:
    return MAX_INFO;
  case CHUNK_SETP:
    return MAX_SETP;
  case CHUNK_FTSP:
    return MAX_FTSP;
  case CHUNK_FRMS:
    return MAX_FRAME_CHUNK;
  case CHUNK_FTRM:
    return MAX_FTRM_CHUNK;
  case CHUNK_EVNT:
    return MAX_EVNT;
  case CHUNK_ENDG:
    return MAX_ENDG;
  default:
    return MAX_OTHER;
  }
}

std::string TypeName(u32 type)
{
  std::string s;
  for (int i = 0; i < 4; ++i)
  {
    const char c = static_cast<char>(type >> (8 * i));
    s.push_back(c >= 0x20 && c < 0x7F ? c : '?');
  }
  return s;
}

int PortCount(u8 ports)
{
  int n = 0;
  for (int p = 0; p < PORTS; ++p)
    n += (ports >> p) & 1;
  return n;
}
}  // namespace

void SetNoController(Slots* slots, int port)
{
  std::fill(slots->begin() + port * PAD_SIZE, slots->begin() + (port + 1) * PAD_SIZE, u8(0));
  (*slots)[port * PAD_SIZE + 0x38] = 0xFF;  // gfPadError NO_CONTROLLER
}

// ---- FRMS ----
// u32 first_frame, u16 count, u8 ports, u8 0; per port in `ports` (ascending) count * PAD_SIZE pad
// bytes, each XORed with the previous frame's; count * u32 checksums; u16 parts, then per part
// u32 frame, u32 rng[3], u32 fighters_crc.

std::vector<u8> EncodeFrames(const FrameChunk& c)
{
  std::vector<u8> out;
  const u16 count = static_cast<u16>(c.pads.size());
  PutU32(out, c.first_frame);
  PutU16(out, count);
  out.push_back(c.ports & 0xF);
  out.push_back(0);
  for (int port = 0; port < PORTS; ++port)
  {
    if (!((c.ports >> port) & 1))
      continue;
    for (size_t f = 0; f < c.pads.size(); ++f)
    {
      for (u32 k = 0; k < PAD_SIZE; ++k)
      {
        const u8 b = c.pads[f][port * PAD_SIZE + k];
        out.push_back(f ? static_cast<u8>(b ^ c.pads[f - 1][port * PAD_SIZE + k]) : b);
      }
    }
  }
  for (size_t f = 0; f < c.pads.size(); ++f)
    PutU32(out, f < c.checksums.size() ? c.checksums[f] : 0);
  PutU16(out, static_cast<u16>(c.parts.size()));
  for (const Parts& p : c.parts)
  {
    PutU32(out, p.frame);
    for (u32 r : p.rng)
      PutU32(out, r);
    PutU32(out, p.fighters_crc);
  }
  return out;
}

std::optional<FrameChunk> DecodeFrames(std::span<const u8> raw, std::string* error)
{
  if (raw.size() < 8)
  {
    *error = "frame chunk too short";
    return std::nullopt;
  }
  FrameChunk c;
  c.first_frame = GetU32(raw.data());
  const u16 count = GetU16(raw.data() + 4);
  c.ports = raw[6];
  const int n = PortCount(c.ports);
  if (count == 0 || count > 10 * FRAMES_PER_CHUNK || (c.ports & 0xF0) || n == 0 || raw[7] != 0 ||
      c.first_frame > MAX_FRAMES)
  {
    *error = "bad frame chunk header";
    return std::nullopt;
  }
  size_t off = 8;
  const size_t pads = static_cast<size_t>(n) * count * PAD_SIZE;
  const size_t sums = static_cast<size_t>(count) * 4;
  if (raw.size() < off + pads + sums + 2)
  {
    *error = "frame chunk too short";
    return std::nullopt;
  }
  c.pads.assign(count, Slots{});
  for (int port = 0; port < PORTS; ++port)
  {
    if (!((c.ports >> port) & 1))
    {
      for (auto& s : c.pads)
        SetNoController(&s, port);
      continue;
    }
    for (u16 f = 0; f < count; ++f)
    {
      for (u32 k = 0; k < PAD_SIZE; ++k)
      {
        const u8 b = raw[off++];
        c.pads[f][port * PAD_SIZE + k] =
            f ? static_cast<u8>(b ^ c.pads[f - 1][port * PAD_SIZE + k]) : b;
      }
    }
  }
  c.checksums.resize(count);
  for (u16 f = 0; f < count; ++f, off += 4)
    c.checksums[f] = GetU32(raw.data() + off);
  const u16 parts = GetU16(raw.data() + off);
  off += 2;
  if (parts > count || raw.size() != off + parts * 20u)
  {
    *error = "bad frame chunk size";
    return std::nullopt;
  }
  for (u16 i = 0; i < parts; ++i, off += 20)
  {
    Parts p;
    p.frame = GetU32(raw.data() + off);
    for (int r = 0; r < 3; ++r)
      p.rng[r] = GetU32(raw.data() + off + 4 + 4 * r);
    p.fighters_crc = GetU32(raw.data() + off + 16);
    c.parts.push_back(p);
  }
  return c;
}

// ---- FTRM ----
// u32 first_frame, u32 count, u8 ports, u8[3] 0, u32 block; per port in `ports` (ascending)
// count * block bytes, each frame XORed with the previous frame's.

std::vector<u8> EncodeFighters(const FighterChunk& c)
{
  std::vector<u8> out;
  PutU32(out, c.first_frame);
  PutU32(out, c.count);
  out.push_back(c.ports & 0xF);
  out.insert(out.end(), 3, 0);
  PutU32(out, c.block);
  for (const auto& d : c.data)
  {
    for (u32 f = 0; f < c.count; ++f)
    {
      for (u32 k = 0; k < c.block; ++k)
      {
        const size_t i = static_cast<size_t>(f) * c.block + k;
        out.push_back(f ? static_cast<u8>(d[i] ^ d[i - c.block]) : d[i]);
      }
    }
  }
  return out;
}

std::optional<FighterChunk> DecodeFighters(std::span<const u8> raw, std::string* error)
{
  if (raw.size() < 16)
  {
    *error = "fighter chunk too short";
    return std::nullopt;
  }
  FighterChunk c;
  c.first_frame = GetU32(raw.data());
  c.count = GetU32(raw.data() + 4);
  c.ports = raw[8];
  c.block = GetU32(raw.data() + 12);
  const int n = PortCount(c.ports);
  if (c.count == 0 || c.count > 10 * FRAMES_PER_CHUNK || (c.ports & 0xF0) || n == 0 ||
      raw[9] || raw[10] || raw[11] || c.block == 0 || c.block > MAX_FIGHTER_BLOCK ||
      c.first_frame > MAX_FRAMES ||
      raw.size() != 16 + static_cast<size_t>(n) * c.count * c.block)
  {
    *error = "bad fighter chunk";
    return std::nullopt;
  }
  size_t off = 16;
  for (int i = 0; i < n; ++i)
  {
    std::vector<u8> d(static_cast<size_t>(c.count) * c.block);
    for (size_t k = 0; k < d.size(); ++k)
      d[k] = k >= c.block ? static_cast<u8>(raw[off + k] ^ d[k - c.block]) : raw[off + k];
    off += d.size();
    c.data.push_back(std::move(d));
  }
  return c;
}

// ---- Writer ----

Writer::~Writer()
{
  Close();
}

bool Writer::WriteAll(const void* data, size_t size)
{
  if (!m_file.IsOpen())
    return false;
  if (size && !m_file.WriteBytes(data, size))
    return false;
  m_offset += size;
  return true;
}

bool Writer::Open(const std::string& path, const std::array<u8, 16>& uid, std::string* error)
{
  Close();
  if (::File::Exists(path))
  {
    *error = "the file exists";
    return false;
  }
  if (!m_file.Open(path, "wb"))
  {
    *error = "cannot create the file";
    return false;
  }
  m_offset = 0;
  std::vector<u8> header(MAGIC.begin(), MAGIC.end());
  PutU16(header, FORMAT_MAJOR);
  PutU16(header, FORMAT_MINOR);
  PutU32(header, HEADER_SIZE);
  header.insert(header.end(), uid.begin(), uid.end());
  if (!WriteAll(header.data(), header.size()) || !m_file.Flush())
  {
    *error = "cannot write the file";
    Close();
    return false;
  }
  return true;
}

std::optional<std::vector<u8>> EncodeChunk(u32 type, u32 flags, std::span<const u8> raw)
{
  std::vector<u8> deflated;
  std::span<const u8> stored = raw;
  if (flags & FLAG_DEFLATE)
  {
    auto d = Deflate(raw);
    if (!d)
      return std::nullopt;
    deflated = std::move(*d);
    stored = deflated;
  }
  std::vector<u8> out;
  out.reserve(CHUNK_HEADER_SIZE + stored.size());
  PutU32(out, type);
  PutU32(out, flags);
  PutU32(out, static_cast<u32>(stored.size()));
  PutU32(out, static_cast<u32>(raw.size()));
  PutU32(out, Crc(stored));
  out.insert(out.end(), stored.begin(), stored.end());
  return out;
}

bool Writer::Append(u32 type, u32 flags, std::span<const u8> raw)
{
  const auto chunk = EncodeChunk(type, flags, raw);
  return chunk && AppendChunk(*chunk);
}

bool Writer::AppendChunk(std::span<const u8> chunk)
{
  if (!m_file.IsOpen())
    return false;
  return WriteAll(chunk.data(), chunk.size()) && m_file.Flush();
}

bool Writer::Finish(std::span<const u8> endg, u32 last_frame)
{
  if (!m_file.IsOpen())
    return false;
  const u64 endg_offset = m_offset;
  if (!Append(CHUNK_ENDG, 0, endg))
    return false;
  std::vector<u8> trailer;
  PutU32(trailer, TRAILER_MAGIC);
  PutU32(trailer, static_cast<u32>(endg_offset));
  PutU32(trailer, last_frame);
  PutU32(trailer, Crc(trailer));
  const bool ok = WriteAll(trailer.data(), trailer.size()) && m_file.Flush();
  Close();
  return ok;
}

void Writer::Close()
{
  m_file.Close();
}

// ---- Reader ----

std::optional<std::string> SafeJson(std::span<const u8> bytes, size_t max_size)
{
  if (bytes.size() > max_size)
    return std::nullopt;
  std::string text(bytes.begin(), bytes.end());
  if (!PeerData::JsonSafeToParse(text, 12))
    return std::nullopt;
  picojson::value v;
  if (!picojson::parse(v, text).empty() || !v.is<picojson::object>())
    return std::nullopt;
  return text;
}

std::optional<File> Parse(std::span<const u8> bytes, const ReadOptions& options, std::string* error)
{
  File file;
  file.size = bytes.size();
  if (bytes.size() > MAX_FILE_SIZE)
  {
    *error = "the file is too large to be a replay";
    return std::nullopt;
  }
  if (bytes.size() < HEADER_SIZE || !std::equal(MAGIC.begin(), MAGIC.end(), bytes.begin()))
  {
    *error = "not a Brawl Online replay";
    return std::nullopt;
  }
  const u16 major = GetU16(bytes.data() + 8);
  file.minor = GetU16(bytes.data() + 10);
  const u32 header_size = GetU32(bytes.data() + 12);
  if (major != FORMAT_MAJOR)
  {
    *error = fmt::format("replay format {} (this version reads {})", major, FORMAT_MAJOR);
    return std::nullopt;
  }
  if (header_size < HEADER_SIZE || header_size > 4096 || header_size > bytes.size())
  {
    *error = "damaged replay header";
    return std::nullopt;
  }
  std::copy(bytes.begin() + 16, bytes.begin() + 32, file.uid.begin());

  // The trailer, if it is there and intact: the chunks end before it.
  size_t end = bytes.size();
  std::optional<u32> trailer_endg;
  if (bytes.size() >= header_size + TRAILER_SIZE)
  {
    const u8* t = bytes.data() + bytes.size() - TRAILER_SIZE;
    if (GetU32(t) == TRAILER_MAGIC && GetU32(t + 12) == Crc({t, 12}) && GetU32(t + 4) >= header_size &&
        GetU32(t + 4) < bytes.size() - TRAILER_SIZE)
    {
      end = bytes.size() - TRAILER_SIZE;
      trailer_endg = GetU32(t + 4);
    }
  }

  const auto truncate = [&](std::string why) {
    file.truncated = true;
    file.truncated_why = std::move(why);
  };
  size_t off = header_size;
  u32 chunks = 0;
  bool have_setup = false;
  std::optional<size_t> endg_at;
  while (off < end)
  {
    if (end - off < CHUNK_HEADER_SIZE)
    {
      truncate("a torn chunk header at the end");
      break;
    }
    if (++chunks > MAX_CHUNKS)
    {
      *error = "too many chunks";
      return std::nullopt;
    }
    const u8* h = bytes.data() + off;
    const u32 type = GetU32(h);
    const u32 flags = GetU32(h + 4);
    const u32 stored_len = GetU32(h + 8);
    const u32 raw_len = GetU32(h + 12);
    const u32 crc = GetU32(h + 16);
    const size_t at = off;
    off += CHUNK_HEADER_SIZE;
    const bool metadata = file.info.empty();
    if (stored_len > end - off)
    {
      if (metadata)
      {
        *error = "damaged replay (its first chunk is cut short)";
        return std::nullopt;
      }
      truncate(fmt::format("the {} chunk at {} is cut short", TypeName(type), at));
      break;
    }
    const std::span<const u8> stored{bytes.data() + off, stored_len};
    off += stored_len;
    if ((flags & ~(FLAG_DEFLATE | FLAG_CRITICAL)) || raw_len > MaxRaw(type) ||
        (!(flags & FLAG_DEFLATE) && raw_len != stored_len) || Crc(stored) != crc)
    {
      if (metadata)
      {
        *error = "damaged replay (its first chunk does not check out)";
        return std::nullopt;
      }
      truncate(fmt::format("the {} chunk at {} is damaged", TypeName(type), at));
      break;
    }
    const bool known = type == CHUNK_INFO || type == CHUNK_SETP || type == CHUNK_FTSP ||
                       type == CHUNK_FRMS || type == CHUNK_FTRM || type == CHUNK_EVNT ||
                       type == CHUNK_ENDG;
    if (!known)
    {
      if (flags & FLAG_CRITICAL)
      {
        *error = fmt::format("the replay needs a newer version (chunk {})", TypeName(type));
        return std::nullopt;
      }
      continue;
    }
    if (metadata && type != CHUNK_INFO)
    {
      *error = "damaged replay (no metadata)";
      return std::nullopt;
    }
    // Chunks the caller does not want are not decompressed.
    if ((type == CHUNK_FRMS && !options.frames) || (type == CHUNK_FTRM && !options.fighters))
      continue;
    std::vector<u8> inflated;
    std::span<const u8> raw = stored;
    if (flags & FLAG_DEFLATE)
    {
      auto d = Inflate(stored, raw_len);
      if (!d)
      {
        if (metadata)
        {
          *error = "damaged replay (its metadata does not decompress)";
          return std::nullopt;
        }
        truncate(fmt::format("the {} chunk at {} does not decompress", TypeName(type), at));
        break;
      }
      inflated = std::move(*d);
      raw = inflated;
    }
    switch (type)
    {
    case CHUNK_INFO:
    {
      if (!file.info.empty())
      {
        *error = "damaged replay (two INFO chunks)";
        return std::nullopt;
      }
      auto j = SafeJson(raw, MAX_INFO);
      if (!j)
      {
        *error = "damaged replay (bad metadata)";
        return std::nullopt;
      }
      file.info = std::move(*j);
      break;
    }
    case CHUNK_SETP:
    {
      if (have_setup)
      {
        *error = "damaged replay (two SETP chunks)";
        return std::nullopt;
      }
      auto j = SafeJson(raw, MAX_SETP);
      if (!j)
      {
        *error = "damaged replay (bad match setup)";
        return std::nullopt;
      }
      file.setup = std::move(*j);
      have_setup = true;
      break;
    }
    case CHUNK_FTSP:
    {
      auto j = SafeJson(raw, MAX_FTSP);
      if (j)
        file.fighter_spec = std::move(*j);
      break;
    }
    case CHUNK_FRMS:
    {
      if (!have_setup)
      {
        *error = "damaged replay (frames before the match setup)";
        return std::nullopt;
      }
      std::string why;
      auto c = DecodeFrames(raw, &why);
      if (!c)
      {
        truncate(fmt::format("frames at {}: {}", at, why));
        off = end;
        break;
      }
      if (file.pads.empty())
      {
        file.first_frame = c->first_frame;
        file.ports = c->ports;
      }
      if (c->first_frame != file.first_frame + file.pads.size() || c->ports != file.ports)
      {
        *error = "damaged replay (its frames do not follow each other)";
        return std::nullopt;
      }
      if (file.pads.size() + c->pads.size() > MAX_FRAMES)
      {
        *error = "the replay is longer than an hour";
        return std::nullopt;
      }
      file.pads.insert(file.pads.end(), c->pads.begin(), c->pads.end());
      file.checksums.insert(file.checksums.end(), c->checksums.begin(), c->checksums.end());
      file.parts.insert(file.parts.end(), c->parts.begin(), c->parts.end());
      break;
    }
    case CHUNK_FTRM:
    {
      std::string why;
      auto c = DecodeFighters(raw, &why);
      if (!c)
        break;  // stats only: a bad chunk does not end the replay
      if (file.fighters.empty())
      {
        file.fighter_block = c->block;
        file.fighter_ports = c->ports;
        file.fighters.resize(c->data.size());
        file.fighter_frames = 0;
      }
      if (c->block != file.fighter_block || c->ports != file.fighter_ports ||
          c->first_frame != file.first_frame + file.fighter_frames ||
          file.fighter_frames + c->count > MAX_FRAMES)
      {
        break;
      }
      for (size_t i = 0; i < c->data.size(); ++i)
        file.fighters[i].insert(file.fighters[i].end(), c->data[i].begin(), c->data[i].end());
      file.fighter_frames += c->count;
      break;
    }
    case CHUNK_EVNT:
    {
      // {"events": [{"frame": n, ...}, ...]}
      auto j = SafeJson(raw, MAX_EVNT);
      if (!j)
        break;
      picojson::value v;
      picojson::parse(v, *j);
      const auto& o = v.get<picojson::object>();
      const auto it = o.find("events");
      if (it == o.end() || !it->second.is<picojson::array>())
        break;
      for (const auto& e : it->second.get<picojson::array>())
      {
        if (!e.is<picojson::object>() || file.events.size() >= 1000)
          continue;
        const auto& eo = e.get<picojson::object>();
        const auto f = eo.find("frame");
        const auto n = PeerData::JsonUInt(f != eo.end() ? &f->second : nullptr, MAX_FRAMES);
        file.events.push_back({n ? static_cast<u32>(*n) : 0, e.serialize()});
      }
      break;
    }
    case CHUNK_ENDG:
    {
      auto j = SafeJson(raw, MAX_ENDG);
      if (j)
      {
        file.endg = std::move(*j);
        endg_at = at;
      }
      break;
    }
    }
  }
  if (file.info.empty())
  {
    *error = "damaged replay (no metadata)";
    return std::nullopt;
  }
  file.trailer = trailer_endg && endg_at && *endg_at == *trailer_endg;
  return file;
}

std::optional<File> ReadFile(const std::string& path, const ReadOptions& options, std::string* error)
{
  // Shared: a spectated game's file is read while the launcher still writes it, and on Windows
  // the default open refuses a file another program has open for writing.
  ::File::IOFile f(path, "rb", ::File::SharedAccess::ReadWrite);
  if (!f.IsOpen())
  {
    *error = "cannot open the file";
    return std::nullopt;
  }
  const u64 size = f.GetSize();
  if (size > MAX_FILE_SIZE)
  {
    *error = "the file is too large to be a replay";
    return std::nullopt;
  }
  std::vector<u8> bytes(size);
  if (size && !f.ReadBytes(bytes.data(), size))
  {
    *error = "cannot read the file";
    return std::nullopt;
  }
  return Parse(bytes, options, error);
}

std::string FileNamePart(const std::string& text, size_t max_chars)
{
  std::string out;
  size_t chars = 0;
  for (size_t i = 0; i < text.size() && chars < max_chars;)
  {
    const u8 c = static_cast<u8>(text[i]);
    u32 cp = 0;
    size_t n = 0;
    if (c < 0x80)
      cp = c, n = 1;
    else if ((c & 0xE0) == 0xC0)
      cp = c & 0x1F, n = 2;
    else if ((c & 0xF0) == 0xE0)
      cp = c & 0x0F, n = 3;
    else if ((c & 0xF8) == 0xF0)
      cp = c & 0x07, n = 4;
    bool ok = n != 0 && i + n <= text.size();
    for (size_t k = 1; ok && k < n; ++k)
    {
      const u8 cc = static_cast<u8>(text[i + k]);
      ok = (cc & 0xC0) == 0x80;
      cp = cp << 6 | (cc & 0x3F);
    }
    if (!ok)
    {
      ++i;  // not UTF-8: dropped
      continue;
    }
    const bool bad = cp < 0x20 || (cp >= 0x7F && cp < 0xA0) || cp == '<' || cp == '>' ||
                     cp == ':' || cp == '"' || cp == '/' || cp == '\\' || cp == '|' || cp == '?' ||
                     cp == '*' || (cp >= 0x200B && cp <= 0x200F) ||
                     (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) ||
                     cp == 0xFEFF || (cp >= 0xD800 && cp <= 0xDFFF);
    if (!bad)
    {
      out.append(text, i, n);
      ++chars;
    }
    i += n;
  }
  const auto trim = [](char ch) { return ch == ' ' || ch == '.'; };
  while (!out.empty() && trim(out.back()))
    out.pop_back();
  size_t lead = 0;
  while (lead < out.size() && trim(out[lead]))
    ++lead;
  return out.substr(lead);
}
}  // namespace Gprb::Replay
