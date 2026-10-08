// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/User.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Core/Rollback/PeerData.h"

namespace Online
{
namespace
{
// Slippi's defaults (slippi-rust-extensions user/src/chat.rs). The server sends the same list.
constexpr std::array<const char*, 16> DEFAULT_CHAT_MESSAGES = {
    "ggs",   "one more", "brb",          "good luck", "well played",           "that was fun",
    "thanks", "too good", "sorry",       "my b",      "lol",                   "wow",
    "gotta go", "one sec", "let's play again later",  "bad connection",
};

constexpr auto WATCH_INTERVAL = std::chrono::milliseconds(500);

std::string GetString(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<std::string>())
    return {};
  return it->second.get<std::string>();
}

// Slippi's UserInfo::sanitize: a chat list that is not exactly 16 entries becomes the default.
std::vector<std::string> ChatMessagesFrom(const picojson::object& o)
{
  const auto it = o.find("chatMessages");
  if (it != o.end() && it->second.is<picojson::array>())
  {
    const auto& arr = it->second.get<picojson::array>();
    if (arr.size() == 16 &&
        std::ranges::all_of(arr, [](const picojson::value& v) { return v.is<std::string>(); }))
    {
      std::vector<std::string> out;
      for (const auto& v : arr)
        out.push_back(v.get<std::string>());
      return out;
    }
  }
  return User::GetDefaultChatMessages();
}

struct Version
{
  std::array<long, 3> numbers{};
  bool prerelease = false;
};

Version ParseVersion(const std::string& text)
{
  Version v;
  std::string core = text;
  if (const auto dash = core.find_first_of("-+"); dash != std::string::npos)
  {
    v.prerelease = core[dash] == '-';
    core.resize(dash);
  }
  const auto parts = SplitString(core, '.');
  for (size_t i = 0; i < parts.size() && i < v.numbers.size(); ++i)
    v.numbers[i] = std::strtol(parts[i].c_str(), nullptr, 10);
  return v;
}
}  // namespace

int CompareVersions(const std::string& a, const std::string& b)
{
  const Version va = ParseVersion(a);
  const Version vb = ParseVersion(b);
  if (va.numbers != vb.numbers)
    return va.numbers < vb.numbers ? -1 : 1;
  if (va.prerelease != vb.prerelease)
    return va.prerelease ? -1 : 1;
  return 0;
}

User::User(std::string user_json_path, std::string accounts_url)
    : m_user_json_path(std::move(user_json_path)), m_accounts_url(std::move(accounts_url))
{
}

User::~User()
{
  StopWatcher();
}

std::vector<std::string> User::GetDefaultChatMessages()
{
  return {DEFAULT_CHAT_MESSAGES.begin(), DEFAULT_CHAT_MESSAGES.end()};
}

bool User::AttemptLogin()
{
  std::string contents;
  if (!File::ReadFileToString(m_user_json_path, contents))
    return false;  // Not logged in yet (the file does not exist).

  picojson::value root;
  const std::string error = picojson::parse(root, contents);
  if (!error.empty() || !root.is<picojson::object>())
  {
    ERROR_LOG_FMT(NETPLAY, "Online: unable to parse {}: {}", m_user_json_path,
                  error.empty() ? "not a JSON object" : error);
    return false;
  }
  const auto& o = root.get<picojson::object>();

  UserInfo info;
  info.uid = GetString(o, "uid");
  info.play_key = GetString(o, "playKey");
  info.display_name = GetString(o, "displayName");
  info.connect_code = GetString(o, "connectCode");
  info.latest_version = GetString(o, "latestVersion");
  info.chat_messages = ChatMessagesFrom(o);
  info.file_contents = contents;
  const std::string uid = info.uid;
  {
    std::lock_guard lk(m_mutex);
    m_info = std::move(info);
  }
  INFO_LOG_FMT(NETPLAY, "Online: logged in as {} ({})", GetUserInfo().display_name,
               GetUserInfo().connect_code);

  FetchFromServer(uid);
  return true;
}

void User::FetchFromServer(const std::string& uid)
{
  if (m_accounts_url.empty() || uid.empty())
    return;

  m_fetch_status = UserFetchStatus::Fetching;
  Common::HttpRequest request(std::chrono::seconds(5));
  const std::string url = fmt::format("{}/user/{}?additionalFields=chatMessages,rank",
                                      m_accounts_url, request.EscapeComponent(uid));
  const auto response = request.Get(url);
  picojson::value root;
  const std::string body = response ? std::string(response->begin(), response->end()) : "";
  if (!response || !Gprb::PeerData::JsonSafeToParse(body, 16) || !picojson::parse(root, body).empty() ||
      !root.is<picojson::object>())
  {
    WARN_LOG_FMT(NETPLAY, "Online: failed to fetch the user info from {} (HTTP {})", url,
                 request.GetLastResponseCode());
    m_fetch_status = UserFetchStatus::Error;
    return;
  }

  const auto& o = root.get<picojson::object>();
  {
    std::lock_guard lk(m_mutex);
    // Slippi's overwrite_from_server: the server's values replace the file's.
    if (m_info.uid != uid)
      return;  // Logged out (or switched) meanwhile.
    m_info.display_name = GetString(o, "displayName");
    m_info.connect_code = GetString(o, "connectCode");
    m_info.latest_version = GetString(o, "latestVersion");
    m_info.chat_messages = ChatMessagesFrom(o);
    const auto rank = o.find("rank");
    if (rank != o.end() && rank->second.is<picojson::object>())
    {
      const auto& r = rank->second.get<picojson::object>();
      auto num = [&r](const char* key) {
        const auto it = r.find(key);
        // finite and within int range before the casts below (they are undefined otherwise)
        const double v = it != r.end() && it->second.is<double>() ? it->second.get<double>() : 0.0;
        return std::isfinite(v) ? std::clamp(v, -1e9, 1e9) : 0.0;
      };
      m_info.ranked_rating = static_cast<float>(num("ratingOrdinal"));
      m_info.ranked_update_count = static_cast<int>(num("ratingUpdateCount"));
      m_info.ranked_global_placement = static_cast<int>(num("dailyGlobalPlacement"));
      m_info.ranked_regional_placement = static_cast<int>(num("dailyRegionalPlacement"));
    }
  }
  m_fetch_status = UserFetchStatus::Fetched;
}

void User::ListenForLogIn()
{
  // Slippi's UserInfoWatcher::watch_for_login: no-op while a watcher runs.
  if (m_watching.load())
    return;
  if (m_watcher.joinable())
    m_watcher.join();

  m_watching = true;
  m_watcher = std::thread([this] {
    Common::SetCurrentThreadName("Online user.json watcher");
    while (m_watching.load())
    {
      if (AttemptLogin())
        break;
      for (auto waited = std::chrono::milliseconds(0); waited < WATCH_INTERVAL && m_watching.load();
           waited += std::chrono::milliseconds(50))
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
    m_watching = false;
  });
}

void User::StopWatcher()
{
  m_watching = false;
  if (m_watcher.joinable())
    m_watcher.join();
}

void User::LogOut()
{
  {
    std::lock_guard lk(m_mutex);
    m_info = UserInfo{};
  }
  m_fetch_status = UserFetchStatus::Error;
  if (File::Exists(m_user_json_path) && !File::Delete(m_user_json_path))
    ERROR_LOG_FMT(NETPLAY, "Online: failed to remove {} on logout", m_user_json_path);
  StopWatcher();
}

void User::OverwriteLatestVersion(const std::string& version)
{
  std::lock_guard lk(m_mutex);
  m_info.latest_version = version;
}

UserInfo User::GetUserInfo() const
{
  std::lock_guard lk(m_mutex);
  return m_info;
}

bool User::IsLoggedIn() const
{
  std::lock_guard lk(m_mutex);
  return !m_info.uid.empty();
}

AppState User::GetAppState() const
{
  // Slippi's prepareOnlineStatus: 0 logged out, 2 when latestVersion is newer than this build,
  // else 1.
  const UserInfo info = GetUserInfo();
  if (info.uid.empty())
    return AppState::LoggedOut;
  if (!info.latest_version.empty() && CompareVersions(info.latest_version, APP_VERSION) > 0)
    return AppState::UpdateRequired;
  return AppState::LoggedIn;
}
}  // namespace Online
