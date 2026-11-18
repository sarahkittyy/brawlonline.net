// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/GameBuild.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

#include <fmt/format.h>
#include <mbedtls/sha256.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/Online/SpecialAssets.h"
#include "Core/Rollback/MachineSDCard.h"

namespace Online::GameBuild
{
namespace
{
std::mutex s_mutex;
std::string s_boot_file;
std::string s_key;
bool s_resolved = false;
std::chrono::steady_clock::time_point s_booted_at;
// What the last accepted hello-resp said of this build (Rooms.cpp). Not forgotten at a boot: the
// connection outlives one, and a boot of another build says `hello` again.
Layout s_layout;       // (under s_mutex)
u32 s_layout_serial = 1;

// A codeset is about 100 KB; a boot file (a launcher .dol) about 350 KB.
constexpr u64 MAX_CODESET = 4u << 20;
constexpr u64 MAX_BOOT_FILE = 64u << 20;
}  // namespace

std::string Sha256Hex(const unsigned char* data, size_t size)
{
  std::array<unsigned char, 32> sum{};
  mbedtls_sha256_ret(data, size, sum.data(), 0);
  std::string out;
  for (const unsigned char b : sum)
    out += fmt::format("{:02x}", b);
  return out;
}

std::string MakeKey(std::string_view folder, std::string_view boot_sha256,
                    const std::array<std::string, 4>& codeset_sha256)
{
  if (folder.empty())
    return {};
  std::string text = fmt::format("boot={}\n", boot_sha256.empty() ? "-" : boot_sha256);
  for (size_t i = 0; i < CODESETS.size(); ++i)
    text += fmt::format("{}={}\n", CODESETS[i], codeset_sha256[i].empty() ? "-" : codeset_sha256[i]);
  const std::string sum =
      Sha256Hex(reinterpret_cast<const unsigned char*>(text.data()), text.size());
  return fmt::format("{}:{}", folder, sum.substr(0, 32));
}

void OnBoot(std::string boot_file)
{
  std::lock_guard lk(s_mutex);
  s_boot_file = std::move(boot_file);
  s_key.clear();
  s_resolved = false;
  s_booted_at = std::chrono::steady_clock::now();
}

void Resolve(std::string_view game_path, const std::string& card_image)
{
  std::string boot_file;
  {
    std::lock_guard lk(s_mutex);
    if (s_resolved)
      return;
    boot_file = s_boot_file;
  }
  const std::string folder = SpecialAssets::CardFolder(game_path);
  std::string boot_sha;
  std::array<std::string, 4> codesets;
  if (!folder.empty())
  {
    if (File::IOFile boot(boot_file, "rb"); boot.IsOpen() && boot.GetSize() <= MAX_BOOT_FILE)
    {
      std::vector<u8> bytes(boot.GetSize());
      if (boot.ReadBytes(bytes.data(), bytes.size()))
        boot_sha = Sha256Hex(bytes.data(), bytes.size());
    }
    // Shared: the emulated SD slot has the image open (as SpecialAssets).
    File::IOFile image(card_image, "rb", File::SharedAccess::ReadWrite);
    for (size_t i = 0; image.IsOpen() && i < CODESETS.size(); ++i)
    {
      const auto bytes = Machine::SDCard::ReadCardFile(
          image, image.GetSize(), fmt::format("{}/{}", folder, CODESETS[i]), MAX_CODESET);
      if (bytes)
        codesets[i] = Sha256Hex(bytes->data(), bytes->size());
    }
  }
  const std::string key = MakeKey(folder, boot_sha, codesets);
  std::lock_guard lk(s_mutex);
  s_key = key;
  s_resolved = true;
  NOTICE_LOG_FMT(NETPLAY, "GameBuild: the game is {} (folder \"{}\", boot {}, codesets {} {} {} {})",
                 key.empty() ? "of no build" : key, folder, boot_sha.substr(0, 12),
                 codesets[0].substr(0, 12), codesets[1].substr(0, 12), codesets[2].substr(0, 12),
                 codesets[3].substr(0, 12));
}

std::string Key()
{
  std::lock_guard lk(s_mutex);
  return s_key;
}

void SetLayout(const Layout& layout)
{
  std::lock_guard lk(s_mutex);
  if (layout == s_layout)
    return;
  s_layout = layout;
  if (++s_layout_serial == 0)
    s_layout_serial = 1;
}

Layout GetLayout(u32* serial)
{
  std::lock_guard lk(s_mutex);
  *serial = s_layout_serial;
  return s_layout;
}

bool ReadyForHello(bool no_plugin, int wait_seconds)
{
  std::lock_guard lk(s_mutex);
  if (s_resolved || no_plugin)
    return true;
  return std::chrono::steady_clock::now() - s_booted_at >= std::chrono::seconds(wait_seconds);
}
}  // namespace Online::GameBuild
