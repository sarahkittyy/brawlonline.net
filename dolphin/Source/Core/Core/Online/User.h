// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The logged-in online user, read from <User>/Online/user.json.
//
// A port of Slippi's user handling (slippi-rust-extensions user/src/lib.rs and watcher.rs, wrapped
// by SlippiUser.cpp in Slippi's Dolphin):
// - the launcher writes user.json {uid, playKey, connectCode, displayName, latestVersion} (it is
//   Slippi-identical, see launcher/PPLUS_PORTING.md);
// - Dolphin polls for the file every 500 ms until it parses (ListenForLogIn), then stops watching;
// - after reading the file it asks the accounts service for the user's public data
//   (GET <accounts>/user/{uid}?additionalFields=chatMessages,rank, Slippi's users-rest) and takes
//   the display name, code, latest version and chat messages from there; a failed lookup keeps
//   the file's values;
// - logging out (in-game) clears the user and deletes user.json; the launcher never deletes it.

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"

namespace Online
{
// The version this build reports as appVersion in matchmaking tickets and compares with
// user.json's latestVersion. The launcher's PlusOnline 0.1.0 writes latestVersion 0.1.0.
constexpr char APP_VERSION[] = "0.1.0";

// Compares two "major.minor.patch[-pre]" versions like Slippi's semver check: -1, 0 or 1.
// Missing or non-numeric parts count as 0; a pre-release sorts before the release.
int CompareVersions(const std::string& a, const std::string& b);

struct UserInfo
{
  std::string uid;
  std::string play_key;
  std::string display_name;
  std::string connect_code;
  std::string latest_version;
  std::string file_contents;

  // Filled for players in a get-ticket-resp (Slippi's UserInfo doubles as player info).
  int port = 0;
  bool is_bot = false;
  std::vector<std::string> chat_messages;

  float ranked_rating = 0;
  int ranked_update_count = 0;
  int ranked_global_placement = 0;
  int ranked_regional_placement = 0;
};

// The online status the game shows (Slippi's GET_ONLINE_STATUS app state).
enum class AppState : u8
{
  LoggedOut = 0,
  LoggedIn = 1,
  UpdateRequired = 2,
};

enum class UserFetchStatus : u8
{
  NotFetched,
  Fetching,
  Fetched,
  Error,
};

class User
{
public:
  // user_json_path: usually <User>/Online/user.json. accounts_url: the accounts service base URL
  // (empty to skip the users-rest lookup).
  User(std::string user_json_path, std::string accounts_url);
  ~User();

  User(const User&) = delete;
  User& operator=(const User&) = delete;

  // Reads user.json now (and does the users-rest lookup). True if the file parsed.
  bool AttemptLogin();
  // Starts the background watcher: AttemptLogin every 500 ms until it succeeds. No-op while one
  // runs.
  void ListenForLogIn();
  // Clears the user, deletes user.json and stops the watcher.
  void LogOut();
  // The mm server sends latestVersion with "out of date" errors.
  void OverwriteLatestVersion(const std::string& version);
  // A ranked set's result carries the new rating (Online/Ranked.h).
  void SetRankedRating(float rating, int sets_played);

  UserInfo GetUserInfo() const;
  bool IsLoggedIn() const;
  AppState GetAppState() const;
  UserFetchStatus GetFetchStatus() const { return m_fetch_status.load(); }
  bool IsWatching() const { return m_watching.load(); }
  const std::string& GetUserJsonPath() const { return m_user_json_path; }

  static std::vector<std::string> GetDefaultChatMessages();

private:
  void FetchFromServer(const std::string& uid);
  void StopWatcher();

  const std::string m_user_json_path;
  const std::string m_accounts_url;

  mutable std::mutex m_mutex;
  UserInfo m_info;

  std::atomic<UserFetchStatus> m_fetch_status{UserFetchStatus::NotFetched};
  std::atomic<bool> m_watching{false};
  std::thread m_watcher;
};
}  // namespace Online
