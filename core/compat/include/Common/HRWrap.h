// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Common/HRWrap.h (core/standalone only): the same HRWrap without
// the winrt::hresult formatter (C++/WinRT is not in the engine's toolchain). Dolphin's DInput code
// logs its HRESULTs with it.

#pragma once

#include <string>

#include <windows.h>

#include <fmt/format.h>

#include "Common/CommonTypes.h"

namespace Common
{
inline std::string GetHResultMessage(HRESULT hr)
{
  char* buffer = nullptr;
  const DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string text = length != 0 && buffer != nullptr ? std::string(buffer, length) : "unknown error";
  if (buffer != nullptr)
    LocalFree(buffer);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  return text;
}

struct HRWrap
{
  constexpr explicit HRWrap(HRESULT hr) : m_hr(hr) {}
  const HRESULT m_hr;
};
}  // namespace Common

template <>
struct fmt::formatter<Common::HRWrap>
{
  constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const Common::HRWrap& hr, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "{} ({:#010x})", Common::GetHResultMessage(hr.m_hr),
                          static_cast<u32>(hr.m_hr));
  }
};
