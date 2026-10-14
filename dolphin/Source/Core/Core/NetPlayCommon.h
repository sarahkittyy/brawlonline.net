// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <SFML/Network/Packet.hpp>

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace NetPlay
{
using namespace std::chrono_literals;
// An arbitrary amount of time of no acknowledgement of sent packets before netplay decides a
// connection is disconnected
constexpr std::chrono::milliseconds PEER_TIMEOUT = 30s;

std::string GetExternalIPAddress();

bool CompressFileIntoPacket(const std::string& file_path, sf::Packet& packet);
bool CompressFolderIntoPacket(const std::string& folder_path, sf::Packet& packet);
bool CompressBufferIntoPacket(std::span<const u8> in_buffer, sf::Packet& packet);
bool DecompressPacketIntoFile(sf::Packet& packet, const std::string& file_path);
bool DecompressPacketIntoFolder(sf::Packet& packet, const std::string& folder_path);
std::optional<std::vector<u8>> DecompressPacketIntoBuffer(sf::Packet& packet);

// The names of the host's active cheat codes, sent with their lines so a guest knows what the
// host's codes are meant to do (the host names them: a label, nothing more). At most
// MAX_CODE_NAMES names of at most MAX_CODE_NAME_BYTES bytes, control characters removed.
constexpr size_t MAX_CODE_NAMES = 255;
constexpr size_t MAX_CODE_NAME_BYTES = 64;
std::string SanitizeCodeName(std::string_view name);
void WriteCodeNames(std::span<const std::string> names, sf::Packet& packet);
// An older host's packet has no names: an empty list.
std::vector<std::string> ReadCodeNames(sf::Packet& packet);
// "A, B, C and 4 more" for the chat and the screen.
std::string DescribeCodeNames(std::span<const std::string> names);
}  // namespace NetPlay
