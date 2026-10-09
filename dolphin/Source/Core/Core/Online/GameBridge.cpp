// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/GameBridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/NetPlayClient.h"
#include "Core/Online/OnlineClient.h"
#include "Core/Online/Ranked.h"
#include "Core/Online/RecentCodes.h"
#include "Core/Online/User.h"
#include "Core/PowerPC/MMU.h"
#include "Core/Rollback/GameplaySession.h"
#include "Core/Rollback/RollbackManager.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Online::GameBridge
{
namespace
{
// ---- The contract: game-code/PPOnline/include/ppom.h (docs/game-code.md, "PPOM block"). ----
constexpr u32 MAGIC = 0x50504F4D;  // "PPOM"
constexpr u16 VERSION = 3;
constexpr u32 HEADER_SIZE = 0x24;

// Header fields (u16 unless noted)
constexpr u32 H_VERSION = 0x04, H_SIZE = 0x06, H_MAILBOX_OFF = 0x08, H_MAILBOX_SIZE = 0x0A;
constexpr u32 H_SESSION_OFF = 0x0C, H_SESSION_SIZE = 0x0E, H_LOCAL_OFF = 0x10,
              H_LOCAL_SIZE = 0x12;

// Mailbox
constexpr u32 MB_REQ_WRITE = 0x00, MB_REQ_READ = 0x04, MB_RESP_COUNT = 0x08, MB_RESP_SEEN = 0x0C;
constexpr u32 MB_REQ = 0x10;
constexpr u32 REQ_SLOTS = 4, REQ_SIZE = 0x80, REQ_PAYLOAD_OFF = 8, REQ_PAYLOAD = 0x78;
constexpr u32 MB_RESP = MB_REQ + REQ_SLOTS * REQ_SIZE;  // 0x210
constexpr u32 RESP_PAYLOAD_OFF = 8, RESP_PAYLOAD = 0x1F8;
constexpr u32 MAILBOX_SIZE = MB_RESP + RESP_PAYLOAD_OFF + RESP_PAYLOAD;  // 0x410

// LOCAL (per machine, excluded from rollback; follows the mailbox): Dolphin's view of this
// player's session, and the game's lock-in.
constexpr u32 LOCAL_SIZE = 0x80;
constexpr u32 L_SEQ = 0x00, L_STATE = 0x04, L_LOCAL_PORT = 0x05, L_REMOTE_READY = 0x06,
              L_DISCONNECTED = 0x07, L_PEER_NAME = 0x08, L_LOCK = 0x28;
// The player's port values (name tag and controls, ppom.h PortValues), game-written with the
// lock-in.
constexpr u32 L_OWN = 0x40;
// Game: 1 once it draws DISCONNECTED in the match HUD.
constexpr u32 L_HUD_DISCONNECTED = 0x7C;
// How long the game gets to show DISCONNECTED (it does so on the frame it sees `disconnected`).
constexpr int HUD_WAIT_FRAMES = 30;
// LockIn (game-written): seq u32, ready, cssChar, charKind, costume, stagePick u16, asl, game
constexpr u32 LK_SEQ = 0x00, LK_READY = 0x04, LK_CSS = 0x05, LK_KIND = 0x06, LK_COSTUME = 0x07,
              LK_STAGE = 0x08, LK_ASL = 0x0A, LK_GAME = 0x0B;
// SESSION (the same on every machine of a session; written only outside a match).
constexpr u32 SESSION_SIZE = 0x210;
constexpr u32 S_SEQ = 0x00, S_STATE = 0x04, S_MODE = 0x05, S_GAME = 0x06, S_LAST_WINNER = 0x07,
              S_STAGE = 0x08, S_ASL = 0x0A, S_NUM_PLAYERS = 0x0B, S_PLAYERS = 0x0C;
constexpr u32 SP_SIZE = 0x80, SP_PRESENT = 0, SP_KIND = 1, SP_COSTUME = 2, SP_NAME = 4,
              SP_CODE = 0x24, SP_PORT_VALUES = 0x40;
constexpr int SESSION_PLAYERS = 4;

constexpr u8 CMD_GET_MATCH_STATE = 0xB3;
constexpr u8 CMD_FIND_OPPONENT = 0xB4;
constexpr u8 CMD_GET_ONLINE_STATUS = 0xB9;
constexpr u8 CMD_CLEANUP_CONNECTION = 0xBA;
constexpr u8 CMD_FETCH_CODE_SUGGESTION = 0xBE;
constexpr u8 CMD_GET_RANK = 0xE3;

constexpr u8 STATUS_OK = 0;
constexpr u8 STATUS_UNSUPPORTED = 0xFF;

constexpr int CODE_LEN = 9, NAME_LEN = 16, ERROR_LEN = 120;

// FindOpponent request payload: mode, lockedChar, costume, team, code u16[9]
constexpr u32 FO_CODE = 4;
// MatchState response payload
constexpr u32 MS_PEER_NAME = 4, MS_PEER_CODE = 0x24, MS_ERROR = 0x38;
// OnlineStatus response payload
constexpr u32 OS_NAME = 2, OS_CODE = 0x22;
// CodeSuggestionRequest payload: mode, scroll, inputLen, pad, index u32, input u16[9]
constexpr u32 CSQ_INDEX = 4, CSQ_INPUT = 8;
// CodeSuggestion response payload: found, len, pad[2], index u32, code u16[9]
constexpr u32 CS_INDEX = 4, CS_CODE = 8;
// RankInfo response payload: state, hasChange, pad[2], rating f32, setsPlayed u32, change f32
constexpr u32 RK_RATING = 4, RK_SETS = 8, RK_CHANGE = 12;
constexpr u8 MODE_TEAMS = 3;

// The game's OSModuleInfo list (first, last).
constexpr u32 OS_MODULE_LIST_HEAD = 0x800030C8;
constexpr int LOCATE_RETRY_FRAMES = 30;

// ---- State. The CPU thread owns everything except what Status() reads under s_mutex. ----
std::atomic<bool> s_enabled{true};
std::atomic<bool> s_hand_off{true};

struct Located
{
  u32 block = 0;
  u32 mailbox = 0;
  u32 local = 0;
  u32 session = 0;
  u32 module = 0;
};

std::mutex s_mutex;
std::optional<Located> s_located;
u64 s_frames = 0;
u64 s_locate_attempts = 0;
int s_locate_wait = 0;
u64 s_requests = 0;
u64 s_responses = 0;
u64 s_lost = 0;
u64 s_deferred_frames = 0;  // frames a response waited for the game to take the previous one
std::map<u8, u64> s_by_cmd;
picojson::object s_last_request;
picojson::object s_last_response;
bool s_netplay_paused = false;
// Session blocks (CPU thread).
u8 s_search_mode = 0;
std::vector<u8> s_session_written;
std::vector<u8> s_local_written;
u32 s_session_seq = 0;
u32 s_local_seq = 0;
u32 s_lock_seq = 0;
picojson::object s_lock_seen;
bool s_was_disconnected = false;
bool s_was_in_match = false;
// Frames left for the game to show DISCONNECTED itself before the OSD stands in (-1: none).
int s_hud_wait = -1;
std::atomic<u64> s_osd_disconnects{0};

bool IsRam(const Core::CPUThreadGuard& guard, u32 addr, u32 size = 4)
{
  return PowerPC::MMU::HostIsRAMAddress(guard, addr) &&
         PowerPC::MMU::HostIsRAMAddress(guard, addr + size - 1);
}

u32 R32(const Core::CPUThreadGuard& guard, u32 addr)
{
  return PowerPC::MMU::HostRead<u32>(guard, addr);
}
u16 R16(const Core::CPUThreadGuard& guard, u32 addr)
{
  return PowerPC::MMU::HostRead<u16>(guard, addr);
}
u8 R8(const Core::CPUThreadGuard& guard, u32 addr)
{
  return PowerPC::MMU::HostRead<u8>(guard, addr);
}
void W32(const Core::CPUThreadGuard& guard, u32 v, u32 addr)
{
  PowerPC::MMU::HostWrite<u32>(guard, v, addr);
}

// Text: the game uses UTF-16BE. Names come as UTF-8 (BMP only; anything else becomes '?').
std::vector<u16> ToUtf16(const std::string& s, size_t max_units)
{
  std::vector<u16> out;
  for (size_t i = 0; i < s.size() && out.size() + 1 < max_units;)
  {
    const u8 c = static_cast<u8>(s[i]);
    u32 cp = '?';
    size_t n = 1;
    if (c < 0x80)
      cp = c;
    else if ((c & 0xE0) == 0xC0 && i + 1 < s.size())
      cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F), n = 2;
    else if ((c & 0xF0) == 0xE0 && i + 2 < s.size())
      cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F), n = 3;
    else if ((c & 0xF8) == 0xF0)
      n = 4;
    out.push_back(cp > 0xFFFF ? u16('?') : static_cast<u16>(cp));
    i += n;
  }
  return out;
}

void WriteU16Text(std::vector<u8>& buf, u32 off, const std::string& s, size_t max_units)
{
  const auto units = ToUtf16(s, max_units);
  for (size_t k = 0; k < units.size(); ++k)
  {
    buf[off + 2 * k] = static_cast<u8>(units[k] >> 8);
    buf[off + 2 * k + 1] = static_cast<u8>(units[k]);
  }
}

// A typed connect code: ASCII, full-width forms (what Brawl's keypad produces) or Slippi's
// full-width number sign, all to upper-case ASCII.
std::string ReadCode(const Core::CPUThreadGuard& guard, u32 addr)
{
  std::string code;
  for (int i = 0; i < CODE_LEN; ++i)
  {
    u16 c = R16(guard, addr + 2 * i);
    if (c == 0)
      break;
    if (c >= 0xFF01 && c <= 0xFF5E)
      c = static_cast<u16>(c - 0xFF01 + 0x21);
    if (c >= 'a' && c <= 'z')
      c = static_cast<u16>(c - 'a' + 'A');
    code.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return code;
}

std::optional<Located> Locate(const Core::CPUThreadGuard& guard)
{
  if (!IsRam(guard, OS_MODULE_LIST_HEAD))
    return std::nullopt;
  u32 module = R32(guard, OS_MODULE_LIST_HEAD);
  for (int n = 0; module != 0 && n < 64; ++n)
  {
    if (!IsRam(guard, module, 0x40))
      return std::nullopt;
    const u32 next = R32(guard, module + 0x04);
    if (R32(guard, module) == PPOM_MODULE_ID)
    {
      const u32 num_sections = R32(guard, module + 0x0C);
      const u32 sections = R32(guard, module + 0x10);
      if (num_sections == 0 || num_sections > 32 || !IsRam(guard, sections, num_sections * 8))
        return std::nullopt;
      for (u32 s = 0; s < num_sections; ++s)
      {
        const u32 offset = R32(guard, sections + 8 * s);
        const u32 size = R32(guard, sections + 8 * s + 4);
        // Linked modules hold absolute section addresses; bit 0 marks executable sections.
        if (offset == 0 || (offset & 1) || size < HEADER_SIZE + MAILBOX_SIZE ||
            !IsRam(guard, offset, size))
        {
          continue;
        }
        for (u32 a = offset; a + HEADER_SIZE <= offset + size; a += 4)
        {
          if (R32(guard, a) != MAGIC || R16(guard, a + H_VERSION) != VERSION)
            continue;
          const u32 block_size = R16(guard, a + H_SIZE);
          const u32 mb_off = R16(guard, a + H_MAILBOX_OFF);
          const u32 mb_size = R16(guard, a + H_MAILBOX_SIZE);
          const u32 se_off = R16(guard, a + H_SESSION_OFF);
          const u32 se_size = R16(guard, a + H_SESSION_SIZE);
          const u32 lo_off = R16(guard, a + H_LOCAL_OFF);
          const u32 lo_size = R16(guard, a + H_LOCAL_SIZE);
          // The mailbox and LOCAL are contiguous: one range excluded from rollback.
          if (mb_off < HEADER_SIZE || mb_size != MAILBOX_SIZE || lo_off != mb_off + mb_size ||
              lo_size != LOCAL_SIZE || se_size != SESSION_SIZE || se_off < HEADER_SIZE ||
              se_off + se_size > block_size || lo_off + lo_size > block_size ||
              a + block_size > offset + size)
          {
            continue;
          }
          return Located{a, a + mb_off, a + lo_off, a + se_off, module};
        }
      }
      return std::nullopt;
    }
    module = next;
  }
  return std::nullopt;
}

struct Response
{
  u8 cmd = 0;
  u8 status = STATUS_OK;
  std::vector<u8> payload = std::vector<u8>(RESP_PAYLOAD, 0);
  std::string summary;
};

void WriteResponse(const Core::CPUThreadGuard& guard, u32 mailbox, u32 seq, const Response& r)
{
  const u32 resp = mailbox + MB_RESP;
  for (u32 i = 0; i < RESP_PAYLOAD; i += 4)
  {
    const u32 v = (u32(r.payload[i]) << 24) | (u32(r.payload[i + 1]) << 16) |
                  (u32(r.payload[i + 2]) << 8) | u32(r.payload[i + 3]);
    W32(guard, v, resp + RESP_PAYLOAD_OFF + i);
  }
  W32(guard, seq, resp);
  W32(guard, (u32(r.cmd) << 24) | (u32(r.status) << 16), resp + 4);
  // Published last: the game acts on a response once respCount changes.
  W32(guard, R32(guard, mailbox + MB_RESP_COUNT) + 1, mailbox + MB_RESP_COUNT);
}

u8 SessionPhase(const Client::MatchState& ms)
{
  // 0 none, 1 connecting, 2 transferring, 3 plugged (ppom.h MatchState.sessionPhase)
  if (ms.handoff != "started")
    return 0;
  const std::string& p = ms.session.phase;
  if (p == "running" || p == "held")
    return 3;
  if (p == "error" || p == "ended" || p.empty())
    return 0;
  return 1;
}

Response MatchStateResponse(const Client::MatchState& ms, bool just_started)
{
  Response r;
  r.cmd = CMD_GET_MATCH_STATE;
  Matchmaking::State state = ms.state;
  // The game counts a fresh search as running from the moment it asked.
  if (just_started && state == Matchmaking::State::Idle)
    state = Matchmaking::State::Initializing;
  r.payload[0] = static_cast<u8>(state);
  std::string peer_name, peer_code;
  if (ms.match)
  {
    r.payload[1] = ms.match->is_host ? 1 : 2;
    for (const auto& p : ms.match->players)
    {
      if (!p.is_local)
      {
        peer_name = p.display_name;
        peer_code = p.connect_code;
        break;
      }
    }
  }
  r.payload[2] = SessionPhase(ms);
  r.payload[3] = 0;
  WriteU16Text(r.payload, MS_PEER_NAME, peer_name, NAME_LEN);
  WriteU16Text(r.payload, MS_PEER_CODE, peer_code, CODE_LEN);
  if (state == Matchmaking::State::ErrorEncountered)
    WriteU16Text(r.payload, MS_ERROR, ms.error, ERROR_LEN);
  r.summary = fmt::format("GET_MATCH_STATE mmState={} ({}) peer='{}' '{}'{}", u8(state),
                          Matchmaking::StateName(state), peer_name, peer_code,
                          state == Matchmaking::State::ErrorEncountered ?
                              fmt::format(" error='{}'", ms.error) :
                              std::string());
  return r;
}

Response OnlineStatusResponse()
{
  Response r;
  r.cmd = CMD_GET_ONLINE_STATUS;
  User& user = Client::GetUser();
  const UserInfo info = user.GetUserInfo();
  const AppState state = user.GetAppState();
  r.payload[0] = static_cast<u8>(state);
  if (state != AppState::LoggedOut)
  {
    WriteU16Text(r.payload, OS_NAME, info.display_name, NAME_LEN);
    WriteU16Text(r.payload, OS_CODE, info.connect_code, CODE_LEN);
  }
  r.summary = fmt::format("GET_ONLINE_STATUS state={} name='{}' code='{}'", u8(state),
                          info.display_name, info.connect_code);
  return r;
}

Response FindOpponent(const Core::CPUThreadGuard& guard, u32 payload, std::string* request)
{
  const u8 mode = R8(guard, payload);
  s_search_mode = mode;
  const u8 locked_char = R8(guard, payload + 1);
  const u8 costume = R8(guard, payload + 2);
  const u8 team = R8(guard, payload + 3);
  const std::string code = ReadCode(guard, payload + FO_CODE);
  *request = fmt::format("FIND_OPPONENT mode={} code='{}' char={:#x} costume={} team={}", mode,
                         code, locked_char, costume, team);

  Client::SearchOptions options;
  options.settings.mode = static_cast<Matchmaking::Mode>(mode);
  if (mode == u8(Matchmaking::Mode::Direct) || mode == u8(Matchmaking::Mode::Teams))
    options.settings.connect_code = code;
  options.hand_off = s_hand_off.load();
  options.selections["character"] = picojson::value(static_cast<double>(locked_char));
  options.selections["costume"] = picojson::value(static_cast<double>(costume));
  options.selections["team"] = picojson::value(static_cast<double>(team));

  // Slippi logs the code to direct-codes.json / teams-codes.json when the search starts
  // (EXI_DeviceSlippi.cpp startFindMatch), before anything can fail.
  if (!code.empty() &&
      (mode == u8(Matchmaking::Mode::Direct) || mode == u8(Matchmaking::Mode::Teams)))
  {
    RecentCodes::Add(mode == u8(Matchmaking::Mode::Teams) ? RecentCodes::Kind::Teams :
                                                             RecentCodes::Kind::Direct,
                     Client::GetUser().GetUserInfo().uid, code);
  }

  std::optional<std::string> error;
  if (mode > u8(Matchmaking::Mode::Teams))
  {
    error = "Unknown online mode";
  }
  else
  {
    error = Client::FindMatch(options);
    if (error)
    {
      // Slippi's FIND_OPPONENT always starts over: drop the old search or connection first.
      Client::Cleanup();
      error = Client::FindMatch(options);
    }
  }
  if (error)
  {
    Client::MatchState ms;
    ms.state = Matchmaking::State::ErrorEncountered;
    ms.error = *error;
    return MatchStateResponse(ms, false);
  }
  return MatchStateResponse(Client::GetMatchState(), true);
}

void PutU32(std::vector<u8>& buf, u32 off, u32 v)
{
  buf[off] = static_cast<u8>(v >> 24);
  buf[off + 1] = static_cast<u8>(v >> 16);
  buf[off + 2] = static_cast<u8>(v >> 8);
  buf[off + 3] = static_cast<u8>(v);
}

// Slippi's GET_RANK (the CSS rank box, RankInfo.c): this player's Elo rating and the change of
// the last ranked set. There are no rank tiers.
Response RankResponse()
{
  const Ranked::RankInfo info = Ranked::GetRankInfo();
  Response r;
  r.cmd = CMD_GET_RANK;
  r.payload[0] = static_cast<u8>(info.state);
  r.payload[1] = info.change ? 1 : 0;
  PutU32(r.payload, RK_RATING, std::bit_cast<u32>(info.rating));
  PutU32(r.payload, RK_SETS, info.sets_played);
  PutU32(r.payload, RK_CHANGE, std::bit_cast<u32>(info.change.value_or(0.0f)));
  r.summary = fmt::format("GET_RANK state={} rating={:.1f} sets={}{}", u8(info.state), info.rating,
                          info.sets_played,
                          info.change ? fmt::format(" change={:+.1f}", *info.change) : std::string());
  return r;
}

// Slippi's FETCH_CODE_SUGGESTION (handleNameEntryLoad): the recent code that starts with what
// was typed. Not found: the input comes back with the request's index (Slippi echoes it too).
Response CodeSuggestion(const Core::CPUThreadGuard& guard, u32 payload, std::string* request)
{
  const u8 mode = R8(guard, payload);
  const u8 scroll = R8(guard, payload + 1);
  const u8 input_len = std::min<u8>(R8(guard, payload + 2), CODE_LEN - 1);
  const u32 index = R32(guard, payload + CSQ_INDEX);
  std::string input = ReadCode(guard, payload + CSQ_INPUT);
  if (input.size() > input_len)
    input.resize(input_len);
  const auto kind = mode == MODE_TEAMS ? RecentCodes::Kind::Teams : RecentCodes::Kind::Direct;
  const RecentCodes::Scroll dir =
      scroll <= 3 ? static_cast<RecentCodes::Scroll>(scroll) : RecentCodes::Scroll::None;
  const auto s =
      RecentCodes::Suggest(kind, Client::GetUser().GetUserInfo().uid, input, index, dir);
  *request = fmt::format("FETCH_CODE_SUGGESTION mode={} scroll={} input='{}' index={}", mode,
                         scroll, input, index);

  Response r;
  r.cmd = CMD_FETCH_CODE_SUGGESTION;
  const std::string& text = s.found ? s.code : input;
  r.payload[0] = s.found ? 1 : 0;
  r.payload[1] = static_cast<u8>(std::min<size_t>(text.size(), CODE_LEN - 1));
  const u32 new_index = s.index;
  r.payload[CS_INDEX] = static_cast<u8>(new_index >> 24);
  r.payload[CS_INDEX + 1] = static_cast<u8>(new_index >> 16);
  r.payload[CS_INDEX + 2] = static_cast<u8>(new_index >> 8);
  r.payload[CS_INDEX + 3] = static_cast<u8>(new_index);
  WriteU16Text(r.payload, CS_CODE, text, CODE_LEN);
  r.summary = s.found ? fmt::format("suggestion '{}' index={}", s.code, s.index) :
                        fmt::format("no suggestion index={}", index);
  return r;
}

const char* CmdName(u8 cmd)
{
  switch (cmd)
  {
  case CMD_GET_MATCH_STATE:
    return "GET_MATCH_STATE";
  case CMD_FIND_OPPONENT:
    return "FIND_OPPONENT";
  case CMD_GET_ONLINE_STATUS:
    return "GET_ONLINE_STATUS";
  case CMD_CLEANUP_CONNECTION:
    return "CLEANUP_CONNECTION";
  case 0xB6:
    return "OPEN_LOGIN";
  case 0xB8:
    return "UPDATE";
  case CMD_FETCH_CODE_SUGGESTION:
    return "FETCH_CODE_SUGGESTION";
  case CMD_GET_RANK:
    return "GET_RANK";
  default:
    return "?";
  }
}

void Record(u32 seq, u8 cmd, const std::string& request, const Response* response)
{
  std::lock_guard lk(s_mutex);
  ++s_requests;
  ++s_by_cmd[cmd];
  s_last_request.clear();
  s_last_request["seq"] = picojson::value(static_cast<double>(seq));
  s_last_request["cmd"] = picojson::value(static_cast<double>(cmd));
  s_last_request["name"] = picojson::value(CmdName(cmd));
  s_last_request["text"] = picojson::value(request);
  s_last_request["frame"] = picojson::value(static_cast<double>(s_frames));
  if (response)
  {
    ++s_responses;
    s_last_response.clear();
    s_last_response["seq"] = picojson::value(static_cast<double>(seq));
    s_last_response["cmd"] = picojson::value(static_cast<double>(response->cmd));
    s_last_response["status"] = picojson::value(static_cast<double>(response->status));
    s_last_response["text"] = picojson::value(response->summary);
    s_last_response["frame"] = picojson::value(static_cast<double>(s_frames));
  }
}

void Service(const Core::CPUThreadGuard& guard, u32 mailbox)
{
  const u32 wr = R32(guard, mailbox + MB_REQ_WRITE);
  u32 rd = R32(guard, mailbox + MB_REQ_READ);
  if (wr == rd)
    return;
  if (wr < rd)
  {
    // The game's sequence started over (it never does within one boot): resync.
    W32(guard, wr, mailbox + MB_REQ_READ);
    return;
  }
  if (wr - rd > REQ_SLOTS)
  {
    // The ring wrapped while nobody serviced it: the oldest requests are gone.
    const u32 lost = wr - rd - REQ_SLOTS;
    WARN_LOG_FMT(NETPLAY, "GameBridge: {} request(s) overwritten before they were read", lost);
    std::lock_guard lk(s_mutex);
    s_lost += lost;
    rd = wr - REQ_SLOTS;
  }

  bool responded = false;
  while (rd < wr)
  {
    const u32 seq = rd + 1;
    const u32 slot = mailbox + MB_REQ + ((seq - 1) % REQ_SLOTS) * REQ_SIZE;
    if (R32(guard, slot) != seq)
    {
      // Overwritten by a newer request, or not fully written: skip it.
      rd = seq;
      continue;
    }
    const u8 cmd = R8(guard, slot + 4);
    const bool needs_response = cmd != CMD_CLEANUP_CONNECTION;
    if (needs_response)
    {
      // One response per frame, and never over one the game has not taken yet.
      if (responded ||
          R32(guard, mailbox + MB_RESP_COUNT) != R32(guard, mailbox + MB_RESP_SEEN))
      {
        std::lock_guard lk(s_mutex);
        ++s_deferred_frames;
        break;
      }
    }

    const u32 payload = slot + REQ_PAYLOAD_OFF;
    std::string request = CmdName(cmd);
    std::optional<Response> response;
    switch (cmd)
    {
    case CMD_GET_ONLINE_STATUS:
      response = OnlineStatusResponse();
      break;
    case CMD_FIND_OPPONENT:
      response = FindOpponent(guard, payload, &request);
      break;
    case CMD_GET_MATCH_STATE:
    {
      Client::MatchState ms = Client::GetMatchState();
      // A ranked set ends after the game that decides it, once the game is back on the character
      // select: the connection closes and the CSS reads as idle (Slippi's ranked set end). The
      // rating change is fetched meanwhile (GET_RANK).
      if (ms.handoff == "started" && Ranked::IsSetOver())
      {
        NOTICE_LOG_FMT(NETPLAY, "GameBridge: the ranked set is over; closing the connection");
        Client::Cleanup();
        ms = Client::GetMatchState();
      }
      // Slippi: the next GET_MATCH_STATE after the peer left or timed out cleans up and reads
      // as IDLE (EXI_DeviceSlippi.cpp handleConnectionCleanup); the CSS goes back to its idle
      // prompt without an error (docs/backend-design.md 5.6).
      const auto d = ms.session.detail.find("disconnected");
      if (ms.handoff == "started" && d != ms.session.detail.end() && d->second.is<bool>() &&
          d->second.get<bool>())
      {
        NOTICE_LOG_FMT(NETPLAY, "GameBridge: the opponent is gone ({}); cleaning up",
                       ms.session.error);
        Client::Cleanup();
        ms = Client::GetMatchState();
      }
      response = MatchStateResponse(ms, false);
      break;
    }
    case CMD_CLEANUP_CONNECTION:
      Client::Cleanup();
      break;
    case CMD_FETCH_CODE_SUGGESTION:
      response = CodeSuggestion(guard, payload, &request);
      break;
    case CMD_GET_RANK:
      response = RankResponse();
      break;
    default:
      response = Response{};
      response->cmd = cmd;
      response->status = STATUS_UNSUPPORTED;
      response->summary = fmt::format("{:#x} unsupported", cmd);
      break;
    }
    if (response)
    {
      WriteResponse(guard, mailbox, seq, *response);
      responded = true;
    }
    // Polls are once per frame while searching; keep them out of the log unless they change.
    if (cmd != CMD_GET_MATCH_STATE)
    {
      NOTICE_LOG_FMT(NETPLAY, "GameBridge: request {} {}{}", seq, request,
                     response ? " -> " + response->summary : std::string());
    }
    else
    {
      static std::string s_last_poll;
      if (response && response->summary != s_last_poll)
      {
        s_last_poll = response->summary;
        NOTICE_LOG_FMT(NETPLAY, "GameBridge: request {} {} -> {}", seq, request,
                       response->summary);
      }
    }
    Record(seq, cmd, request, response ? &*response : nullptr);
    rd = seq;
  }
  W32(guard, rd, mailbox + MB_REQ_READ);
}

void PutU16Text(std::vector<u8>& buf, u32 off, const std::string& s, size_t max_units)
{
  WriteU16Text(buf, off, s, max_units);
}

// Writes `bytes` at `addr` when they differ from what was written last, then bumps the u32
// sequence word at `addr` (the first four bytes) so the game sees a consistent update.
void WriteBlock(const Core::CPUThreadGuard& guard, u32 addr, std::vector<u8> bytes,
                std::vector<u8>* last, u32* seq)
{
  // Compare without the sequence word.
  std::fill(bytes.begin(), bytes.begin() + 4, u8(0));
  if (*last == bytes)
    return;
  *last = bytes;
  for (size_t i = 4; i < bytes.size(); i += 4)
  {
    W32(guard, (u32(bytes[i]) << 24) | (u32(bytes[i + 1]) << 16) | (u32(bytes[i + 2]) << 8) |
                   u32(bytes[i + 3]),
        addr + static_cast<u32>(i));
  }
  W32(guard, ++*seq, addr);
}

// The SESSION and LOCAL blocks (design 5.2): the game's lock-in to the gameplay session, the
// session's lobby and match setup back to the game.
void SyncSession(const Core::CPUThreadGuard& guard, const Located& loc)
{
  // The lock-in the game wrote (only the character select writes it).
  const u32 lock = loc.local + L_LOCK;
  const u32 lock_seq = R32(guard, lock + LK_SEQ);
  if (lock_seq != 0)
  {
    Gprb::Session::LockIn l;
    l.ready = R8(guard, lock + LK_READY) != 0;
    l.css = R8(guard, lock + LK_CSS);
    l.char_kind = R8(guard, lock + LK_KIND);
    l.costume = R8(guard, lock + LK_COSTUME);
    l.stage_pick = R16(guard, lock + LK_STAGE);
    l.asl = R8(guard, lock + LK_ASL);
    l.game = R8(guard, lock + LK_GAME);
    for (u32 i = 0; i < l.port_values.size(); ++i)
      l.port_values[i] = R8(guard, loc.local + L_OWN + i);
    Gprb::Session::SetLocalLock(l);
    if (lock_seq != s_lock_seq)
    {
      s_lock_seq = lock_seq;
      std::lock_guard lk(s_mutex);
      s_lock_seen.clear();
      s_lock_seen["seq"] = picojson::value(static_cast<double>(lock_seq));
      s_lock_seen["ready"] = picojson::value(l.ready);
      s_lock_seen["char_kind"] = picojson::value(static_cast<double>(l.char_kind));
      s_lock_seen["costume"] = picojson::value(static_cast<double>(l.costume));
      s_lock_seen["stage_pick"] = picojson::value(static_cast<double>(l.stage_pick));
      s_lock_seen["game"] = picojson::value(static_cast<double>(l.game));
      s_lock_seen["tag"] = picojson::value((l.port_values[0] & 1) != 0);
    }
  }

  const Gprb::Session::Lobby lobby = Gprb::Session::GetLobby();
  std::vector<std::string> names(SESSION_PLAYERS), codes(SESSION_PLAYERS);
  std::string peer_name;
  if (lobby.active)
  {
    const Client::MatchState ms = Client::GetMatchState();
    if (ms.match && ms.handoff == "started")
    {
      // In-game ports: the decider (host) is P1, as in the gameplay session.
      std::vector<const PlayerInfo*> order;
      for (const auto& p : ms.match->players)
      {
        if (p.is_local == ms.match->is_host)
          order.insert(order.begin(), &p);
        else
          order.push_back(&p);
      }
      for (size_t i = 0; i < order.size() && i < names.size(); ++i)
      {
        names[i] = order[i]->display_name;
        codes[i] = order[i]->connect_code;
        if (!order[i]->is_local && peer_name.empty())
          peer_name = order[i]->display_name;
      }
    }
  }

  // The opponent is gone (design 5.6). In a match the game plays the error sound, draws
  // DISCONNECTED in red at the top of its HUD (Slippi's in-game text) and ends the game; on the
  // character select it goes back to its idle prompt without any text, as Slippi does. Dolphin's
  // red OSD message only stands in when the game was in a match and did not show the text itself
  // (an older plugin, or a scene where it cannot draw).
  if (lobby.disconnected && !s_was_disconnected)
  {
    NOTICE_LOG_FMT(NETPLAY, "GameBridge: the opponent disconnected{}",
                   s_was_in_match ? " in a match" : "");
    Ranked::OnPeerGone();
    s_hud_wait = s_was_in_match ? HUD_WAIT_FRAMES : -1;
  }
  if (s_hud_wait >= 0)
  {
    if (R8(guard, loc.local + L_HUD_DISCONNECTED) != 0)
    {
      s_hud_wait = -1;
    }
    else if (s_hud_wait-- == 0)
    {
      OSD::AddMessage("DISCONNECTED", 5000, OSD::Color::RED);
      ++s_osd_disconnects;
      NOTICE_LOG_FMT(NETPLAY, "GameBridge: the game did not show DISCONNECTED; OSD instead");
    }
  }
  s_was_in_match = lobby.in_match;
  s_was_disconnected = lobby.disconnected;

  // LOCAL: always (it is not part of the rolled-back state).
  std::vector<u8> local(LOCAL_SIZE, 0);
  local[L_STATE] = !lobby.active ? 0 : lobby.in_match ? 3 : lobby.connected ? 2 : 1;
  local[L_LOCAL_PORT] = lobby.active ? static_cast<u8>(lobby.local_port) : 0xFF;
  local[L_REMOTE_READY] = lobby.remote_ready ? 1 : 0;
  local[L_DISCONNECTED] = lobby.disconnected ? 1 : 0;
  PutU16Text(local, L_PEER_NAME, peer_name, NAME_LEN);
  // Keep the game's lock-in as it is.
  for (u32 i = L_LOCK; i < LOCAL_SIZE; ++i)
    local[i] = R8(guard, loc.local + i);
  WriteBlock(guard, loc.local, local, &s_local_written, &s_local_seq);

  // SESSION: never while a match runs, so it is the same on both machines for the whole match.
  if (lobby.in_match)
    return;
  std::vector<u8> session(SESSION_SIZE, 0);
  if (lobby.active)
  {
    session[S_STATE] = lobby.setup_ready ? 2 : 1;
    session[S_MODE] = s_search_mode;
    session[S_GAME] = static_cast<u8>(lobby.game);
    session[S_LAST_WINNER] = lobby.last_winner;
    session[S_STAGE] = static_cast<u8>(lobby.stage >> 8);
    session[S_STAGE + 1] = static_cast<u8>(lobby.stage);
    session[S_ASL] = lobby.asl;
    session[S_NUM_PLAYERS] = static_cast<u8>(lobby.num_players);
    for (int i = 0; i < SESSION_PLAYERS; ++i)
    {
      const u32 p = S_PLAYERS + i * SP_SIZE;
      const auto& pl = lobby.players[i];
      session[p + SP_PRESENT] = pl.present ? 1 : 0;
      session[p + SP_KIND] = pl.present ? pl.char_kind : 0xFF;
      session[p + SP_COSTUME] = pl.costume;
      PutU16Text(session, p + SP_NAME, names[i], NAME_LEN);
      PutU16Text(session, p + SP_CODE, codes[i], CODE_LEN);
      if (pl.present)
        std::copy(pl.port_values.begin(), pl.port_values.end(), session.begin() + p + SP_PORT_VALUES);
    }
  }
  else
  {
    session[S_STAGE] = session[S_STAGE + 1] = 0xFF;
    session[S_LAST_WINNER] = 0xFF;
  }
  WriteBlock(guard, loc.session, session, &s_session_written, &s_session_seq);
}
}  // namespace

void OnFrameEnd(const Core::CPUThreadGuard& guard)
{
  std::optional<Located> located;
  {
    std::lock_guard lk(s_mutex);
    ++s_frames;
    located = s_located;
  }

  // Still valid? One word per frame.
  if (located && (!IsRam(guard, located->block) || R32(guard, located->block) != MAGIC))
  {
    WARN_LOG_FMT(NETPLAY, "GameBridge: PPOM block at {:08x} is gone", located->block);
    located.reset();
    std::lock_guard lk(s_mutex);
    s_located.reset();
    Rollback::RollbackManager::Get().SetMailboxRegion(0, 0);
  }
  if (!located)
  {
    {
      std::lock_guard lk(s_mutex);
      if (s_locate_wait > 0)
      {
        --s_locate_wait;
        return;
      }
      s_locate_wait = LOCATE_RETRY_FRAMES;
      ++s_locate_attempts;
    }
    located = Locate(guard);
    if (!located)
      return;
    NOTICE_LOG_FMT(NETPLAY,
                   "GameBridge: PPOM block at {:08x} (mailbox {:08x}, local {:08x}, session "
                   "{:08x}, module {:08x})",
                   located->block, located->mailbox, located->local, located->session,
                   located->module);
    // Out of rollback state (design 5.2): the mailbox (the game reads it only outside matches)
    // and LOCAL (what differs between the machines), which follows it.
    Rollback::RollbackManager::Get().SetMailboxRegion(located->mailbox,
                                                      MAILBOX_SIZE + LOCAL_SIZE);
    s_session_written.clear();
    s_local_written.clear();
    std::lock_guard lk(s_mutex);
    s_located = located;
  }

  // Never during a netplay session: what Dolphin would answer differs between the machines.
  const bool netplay = NetPlay::IsNetPlayRunning();
  {
    std::lock_guard lk(s_mutex);
    if (netplay != s_netplay_paused)
    {
      s_netplay_paused = netplay;
      NOTICE_LOG_FMT(NETPLAY, "GameBridge: servicing {} (netplay {})",
                     netplay ? "paused" : "resumed", netplay ? "running" : "stopped");
    }
  }
  if (netplay || !s_enabled.load())
    return;
  SyncSession(guard, *located);
  Service(guard, located->mailbox);
}

void Reset()
{
  // Start watching for user.json at boot, as Slippi does when its EXI device is created: the
  // main menu's first GET_ONLINE_STATUS must already see the login (it comes ~10 s later).
  Client::GetUser();
  {
    std::lock_guard lk(s_mutex);
    s_located.reset();
    s_locate_wait = 0;
  }
  Rollback::RollbackManager::Get().SetMailboxRegion(0, 0);
}

void SetEnabled(bool enabled)
{
  s_enabled = enabled;
}

void SetHandOff(bool hand_off)
{
  s_hand_off = hand_off;
}

picojson::object Status()
{
  std::lock_guard lk(s_mutex);
  picojson::object o;
  o["enabled"] = picojson::value(s_enabled.load());
  o["hand_off"] = picojson::value(s_hand_off.load());
  o["found"] = picojson::value(s_located.has_value());
  if (s_located)
  {
    o["block"] = picojson::value(static_cast<double>(s_located->block));
    o["mailbox"] = picojson::value(static_cast<double>(s_located->mailbox));
    o["mailbox_size"] = picojson::value(static_cast<double>(MAILBOX_SIZE));
    o["local"] = picojson::value(static_cast<double>(s_located->local));
    o["session"] = picojson::value(static_cast<double>(s_located->session));
    o["session_seq"] = picojson::value(static_cast<double>(s_session_seq));
    o["local_seq"] = picojson::value(static_cast<double>(s_local_seq));
    o["lock_in"] = picojson::value(s_lock_seen);
    o["module"] = picojson::value(static_cast<double>(s_located->module));
  }
  o["module_id"] = picojson::value(static_cast<double>(PPOM_MODULE_ID));
  o["frames"] = picojson::value(static_cast<double>(s_frames));
  o["locate_attempts"] = picojson::value(static_cast<double>(s_locate_attempts));
  o["netplay_paused"] = picojson::value(s_netplay_paused);
  o["requests"] = picojson::value(static_cast<double>(s_requests));
  o["responses"] = picojson::value(static_cast<double>(s_responses));
  o["lost"] = picojson::value(static_cast<double>(s_lost));
  o["deferred_frames"] = picojson::value(static_cast<double>(s_deferred_frames));
  o["osd_disconnects"] = picojson::value(static_cast<double>(s_osd_disconnects.load()));
  picojson::object by_cmd;
  for (const auto& [cmd, n] : s_by_cmd)
    by_cmd[CmdName(cmd)] = picojson::value(static_cast<double>(n));
  o["by_cmd"] = picojson::value(by_cmd);
  o["last_request"] = picojson::value(s_last_request);
  o["last_response"] = picojson::value(s_last_response);
  return o;
}
}  // namespace Online::GameBridge
