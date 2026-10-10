// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The chat text cleaner (docs/chat-protocol.md §5). Everything another player types reaches the
// screen only through Clean: what is typed is cleaned before it is sent, what arrives is cleaned
// again before it is shown. The server's common::chat::clean_text applies the same rules.
// Pure functions, unit-tested and fuzzed (UnitTests/Core/Online/ChatTextTest.cpp).

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace Online::ChatText
{
constexpr size_t MAX_MESSAGE_CODE_POINTS = 100;
constexpr size_t MAX_MESSAGE_BYTES = 300;
constexpr size_t MAX_NAME_CODE_POINTS = 32;
// At most this many combining marks in a row (the rest are dropped).
constexpr size_t MAX_COMBINING_RUN = 2;

// `text` cleaned: nullopt if it is not valid UTF-8 or nothing is left. The result is valid UTF-8,
// at most `max_code_points` code points and `max_bytes` bytes, with no control, bidi, invisible,
// private-use or noncharacter code points, single spaces only and no leading or trailing space.
std::optional<std::string> Clean(std::string_view text,
                                 size_t max_code_points = MAX_MESSAGE_CODE_POINTS,
                                 size_t max_bytes = MAX_MESSAGE_BYTES);

// Decodes one code point from valid UTF-8 at `text[*pos]` and advances `*pos`; nullopt (and
// `*pos` unchanged) on an invalid or truncated sequence, an overlong form, a surrogate or a value
// above U+10FFFF.
std::optional<char32_t> DecodeUtf8(std::string_view text, size_t* pos);

// The number of code points in valid UTF-8 (each byte that does not continue a sequence).
size_t CountCodePoints(std::string_view utf8);

// The longest prefix of valid UTF-8 `utf8` that has at most `max_code_points` code points and
// `max_bytes` bytes (cut on a code point boundary).
std::string_view Truncate(std::string_view utf8, size_t max_code_points, size_t max_bytes);
}  // namespace Online::ChatText
