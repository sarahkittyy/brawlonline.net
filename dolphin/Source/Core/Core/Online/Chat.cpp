// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/Chat.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Core/Online/ChatCrypto.h"
#include "Core/Online/ChatText.h"
#include "Core/Online/OnlineClient.h"
#include "Core/Online/User.h"
#include "Core/Rollback/PeerData.h"

namespace Online::Chat
{
namespace
{
namespace CC = ChatCrypto;
using Clock = std::chrono::steady_clock;

constexpr size_t MAX_PACKET = 8 * 1024;  // mm's limit (server/crates/mm/src/engine.rs)
constexpr size_t MAX_GROUP_ID = 96;
constexpr size_t MAX_NAME_BYTES = 64;
constexpr size_t MAX_CODE_BYTES = 32;
constexpr size_t MAX_ERROR_BYTES = 200;
constexpr size_t MAX_REASON_BYTES = 200;
constexpr size_t MAX_HIDDEN_FILE = 64 * 1024;
constexpr size_t MAX_PENDING = 8;
constexpr size_t MAX_OUTBOX = 32;
constexpr size_t MAX_LOG = 40;
// The local send limit, as mm's (docs/chat-protocol.md §3).
constexpr size_t SEND_BURST = 5;
constexpr auto SEND_BURST_WINDOW = std::chrono::seconds(5);
constexpr size_t SEND_PER_MINUTE = 30;
constexpr auto KEY_RESEND = std::chrono::seconds(3);
constexpr auto MATCH_STATE_EVERY = std::chrono::milliseconds(250);
constexpr size_t MAX_PAST_KEYS = 8;
// What a report's messages may take, leaving room in MAX_PACKET for the rest of it.
constexpr size_t MAX_REPORT_MESSAGES_BYTES = MAX_PACKET - 1024;

enum class GroupKind : u8
{
  None,
  Room,
  Match,
};

struct MemberState
{
  std::string uid;
  std::string name;
  std::string code;
  CC::Key32 id_key{};
  std::optional<CC::Key32> kx;    // verified against id_key
  std::optional<CC::Key32> pair;  // the pair key with this member (others only)
  u64 last_seq = 0;
  // Every group key this member has had in this group with the last sequence number seen under
  // it, so a key put back (by a misbehaving server) doesn't start its numbers over.
  std::vector<std::pair<CC::Key32, u64>> past_keys;
  double tokens = RECEIVE_BURST;
  Clock::time_point refilled{};
};

struct StoredLine
{
  Line line;
  // Message lines: what a report sends (the raw text, which the signature covers).
  u64 seq = 0;
  CC::Sig64 sig{};
  std::string raw;
};

struct State
{
  // The identity.
  std::unique_ptr<CC::Identity> identity;
  std::string identity_hex;
  bool identity_tried = false;
  std::string identity_error;

  // The group.
  GroupKind kind = GroupKind::None;
  std::string group;
  std::string you;
  std::vector<MemberState> members;
  std::optional<CC::GroupKey> key;
  std::optional<Clock::time_point> key_sent_at;
  u64 seq = 0;

  std::deque<std::string> pending;  // cleaned texts waiting for the Rooms thread
  std::deque<std::string> outbox;   // JSON to send
  std::deque<Clock::time_point> sent_times;

  std::deque<StoredLine> lines;
  u64 next_line_id = 1;
  u64 serial = 1;
  u64 last_message_ms = 0;

  // A match group is shown once its session has connected (Client::GetMatchState, polled), and
  // from then on until the group ends: after a ranked set the connection closes, but the players
  // can still say gg. A failed connect requeues and the next ticket ends the group unseen.
  bool match_connected = false;
  Clock::time_point match_checked{};

  bool hidden_loaded = false;
  std::set<std::string> hidden;

  u64 received = 0, accepted = 0, sent = 0, groups = 0, reports = 0;
  u64 drop_json = 0, drop_member = 0, drop_box = 0, drop_plaintext = 0, drop_replay = 0,
      drop_signature = 0, drop_text = 0, drop_rate = 0, bad_keys = 0, bad_groups = 0;
  std::deque<std::string> log;
};

std::mutex s_mutex;
State s;
std::optional<std::string> s_uid_for_testing;

std::string LocalUid()
{
  if (s_uid_for_testing)
    return *s_uid_for_testing;
  return Client::GetUser().GetUserInfo().uid;
}

void Log(std::string line)
{
  // under s_mutex. Never with another player's text: only ids, counts and our own wording.
  NOTICE_LOG_FMT(NETPLAY, "Chat: {}", line);
  s.log.push_back(std::move(line));
  while (s.log.size() > MAX_LOG)
    s.log.pop_front();
}

bool IsUid(std::string_view v)
{
  if (v.size() != 36)
    return false;
  for (size_t i = 0; i < v.size(); ++i)
  {
    const char c = v[i];
    if (i == 8 || i == 13 || i == 18 || i == 23)
    {
      if (c != '-')
        return false;
    }
    else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
    {
      return false;
    }
  }
  return true;
}

bool IsGroupId(std::string_view v)
{
  if (v.empty() || v.size() > MAX_GROUP_ID)
    return false;
  return std::ranges::all_of(v, [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == ':' || c == '+' || c == '-';
  });
}

const picojson::value* Get(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}

std::optional<std::string> Str(const picojson::object& o, const char* key, size_t max_bytes)
{
  const picojson::value* v = Get(o, key);
  if (!v || !v->is<std::string>() || v->get<std::string>().size() > max_bytes)
    return std::nullopt;
  return v->get<std::string>();
}

std::string CleanOr(const std::string& text, size_t max_code_points, const char* fallback)
{
  return ChatText::Clean(text, max_code_points).value_or(fallback);
}

std::string OnlineDir()
{
  return File::GetUserPath(D_ONLINE_IDX);
}

bool WriteFileAtomically(const std::string& path, std::string_view contents)
{
  const std::string tmp = path + ".tmp";
  if (!File::CreateFullPath(path) || !File::WriteStringToFile(tmp, contents))
    return false;
#ifndef _WIN32
  chmod(tmp.c_str(), 0600);
#endif
  return File::Rename(tmp, path);
}

void Touch()
{
  ++s.serial;
}

void AddLine(StoredLine line)
{
  // under s_mutex
  line.line.id = s.next_line_id++;
  if (line.line.kind != Line::Kind::System)
    s.last_message_ms = Common::Timer::NowMs();
  s.lines.push_back(std::move(line));
  while (s.lines.size() > MAX_LINES)
    s.lines.pop_front();
  Touch();
}

void AddSystemLine(std::string text)
{
  StoredLine l;
  l.line.kind = Line::Kind::System;
  l.line.text = std::move(text);
  AddLine(std::move(l));
}

MemberState* FindMember(std::string_view uid)
{
  const auto it = std::ranges::find(s.members, uid, &MemberState::uid);
  return it == s.members.end() ? nullptr : &*it;
}

int ColorOf(std::string_view uid)
{
  const auto it = std::ranges::find(s.members, uid, &MemberState::uid);
  return it == s.members.end() ? 0 : static_cast<int>((it - s.members.begin()) % MAX_MEMBERS);
}

bool AnyReader()
{
  return std::ranges::any_of(s.members, [](const MemberState& m) { return m.pair.has_value(); });
}

void WipeKey()
{
  if (s.key)
    CC::Wipe(s.key->secret.data(), s.key->secret.size());
  s.key.reset();
  for (MemberState& m : s.members)
  {
    if (m.pair)
      CC::Wipe(m.pair->data(), m.pair->size());
  }
}

void LeaveLocked(const char* why)
{
  if (s.kind == GroupKind::None)
    return;
  Log(fmt::format("left {} ({})", s.group, why));
  WipeKey();
  s.kind = GroupKind::None;
  s.group.clear();
  s.you.clear();
  s.members.clear();
  s.seq = 0;
  s.key_sent_at.reset();
  s.pending.clear();
  s.lines.clear();
  s.match_connected = false;
  Touch();
}

void LoadHiddenLocked()
{
  if (s.hidden_loaded)
    return;
  s.hidden_loaded = true;
  const std::string path = OnlineDir() + "chat-hidden.json";
  std::string text;
  if (!File::Exists(path) || !File::ReadFileToString(path, text) || text.size() > MAX_HIDDEN_FILE ||
      !Gprb::PeerData::JsonSafeToParse(text))
  {
    return;
  }
  picojson::value root;
  if (!picojson::parse(root, text).empty() || !root.is<picojson::object>())
    return;
  const picojson::value* list = Get(root.get<picojson::object>(), "hidden");
  if (!list || !list->is<picojson::array>())
    return;
  for (const picojson::value& v : list->get<picojson::array>())
  {
    if (s.hidden.size() >= MAX_HIDDEN)
      break;
    if (v.is<std::string>() && IsUid(v.get<std::string>()))
      s.hidden.insert(v.get<std::string>());
  }
}

void SaveHiddenLocked()
{
  picojson::array list;
  for (const std::string& uid : s.hidden)
    list.emplace_back(uid);
  picojson::object o;
  o["hidden"] = picojson::value(list);
  if (!WriteFileAtomically(OnlineDir() + "chat-hidden.json", picojson::value(o).serialize(true)))
    Log("cannot write chat-hidden.json");
}

void LoadIdentityLocked()
{
  if (s.identity_tried)
    return;
  s.identity_tried = true;
  const std::string path = OnlineDir() + "chat-identity.key";
  std::optional<CC::Key32> seed;
  if (File::Exists(path))
  {
    std::string text;
    if (File::ReadFileToString(path, text) && text.size() <= 256)
    {
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
      seed = CC::FromHexFixed<32>(text);
    }
    CC::Wipe(text.data(), text.size());
    if (!seed)
    {
      // Not ours to delete: keep it aside and make a new key (the server keeps the old public
      // key, so reports about what was said with it still verify).
      File::Rename(path, path + ".bad");
      Log("chat-identity.key is unreadable: kept as .bad, made a new key");
    }
  }
  if (!seed)
  {
    seed = CC::RandomKey32();
    std::string hex = CC::ToHex(*seed) + "\n";
    const bool ok = WriteFileAtomically(path, hex);
    CC::Wipe(hex.data(), hex.size());
    if (!ok)
    {
      s.identity_error = "cannot write chat-identity.key";
      Log(s.identity_error);
      CC::Wipe(seed->data(), seed->size());
      return;
    }
  }
  s.identity = std::make_unique<CC::Identity>(*seed);
  CC::Wipe(seed->data(), seed->size());
  s.identity_hex = CC::ToHex(s.identity->PublicKey());
}

void QueueOutbox(const picojson::object& o)
{
  if (s.outbox.size() >= MAX_OUTBOX)
  {
    Log("outbox full: dropped a message");
    return;
  }
  s.outbox.push_back(picojson::value(o).serialize());
}

void QueueKeyLocked(Clock::time_point now)
{
  if (!s.key || !s.identity)
    return;
  if (s.key_sent_at && now - *s.key_sent_at < KEY_RESEND)
    return;
  const CC::Sig64 sig = s.identity->Sign(CC::KxSigMessage(s.group, s.key->public_key));
  picojson::object o;
  o["type"] = picojson::value("chat-key");
  o["group"] = picojson::value(s.group);
  o["kx"] = picojson::value(CC::ToHex(s.key->public_key));
  o["sig"] = picojson::value(CC::ToHex(sig));
  QueueOutbox(o);
  s.key_sent_at = now;
}

// ---- chat-group ----

struct ParsedMember
{
  std::string uid, name, code;
  CC::Key32 id_key{};
  std::optional<CC::Key32> kx;
  std::optional<CC::Sig64> kx_sig;
};

std::optional<std::vector<ParsedMember>> ParseMembers(const picojson::value* v, std::string* why)
{
  if (!v || !v->is<picojson::array>())
  {
    *why = "members is not an array";
    return std::nullopt;
  }
  const auto& arr = v->get<picojson::array>();
  if (arr.empty() || arr.size() > MAX_MEMBERS)
  {
    *why = "member count";
    return std::nullopt;
  }
  std::vector<ParsedMember> out;
  for (const picojson::value& mv : arr)
  {
    if (!mv.is<picojson::object>())
    {
      *why = "member is not an object";
      return std::nullopt;
    }
    const auto& mo = mv.get<picojson::object>();
    ParsedMember m;
    const auto uid = Str(mo, "uid", 36);
    const auto name = Str(mo, "displayName", MAX_NAME_BYTES);
    const auto code = Str(mo, "connectCode", MAX_CODE_BYTES);
    const auto id_key = Str(mo, "idKey", 64);
    if (!uid || !IsUid(*uid) || !name || !code || !id_key)
    {
      *why = "member fields";
      return std::nullopt;
    }
    if (std::ranges::any_of(out, [&](const ParsedMember& o) { return o.uid == *uid; }))
    {
      *why = "duplicate member";
      return std::nullopt;
    }
    const auto id = CC::FromHexFixed<32>(*id_key);
    if (!id)
    {
      *why = "idKey";
      return std::nullopt;
    }
    m.uid = *uid;
    m.name = CleanOr(*name, ChatText::MAX_NAME_CODE_POINTS, "?");
    m.code = CleanOr(*code, ChatText::MAX_NAME_CODE_POINTS, "");
    m.id_key = *id;
    const picojson::value* kx = Get(mo, "kx");
    const picojson::value* kx_sig = Get(mo, "kxSig");
    if (kx && kx->is<std::string>() && kx_sig && kx_sig->is<std::string>())
    {
      m.kx = CC::FromHexFixed<32>(kx->get<std::string>());
      m.kx_sig = CC::FromHexFixed<64>(kx_sig->get<std::string>());
      if (!m.kx || !m.kx_sig)
      {
        *why = "kx";
        return std::nullopt;
      }
    }
    out.push_back(std::move(m));
  }
  return out;
}

void OnGroup(const picojson::object& o, Clock::time_point now)
{
  const picojson::value* g = Get(o, "group");
  if (!g || g->is<picojson::null>())
  {
    LeaveLocked("server");
    return;
  }
  const auto group = Str(o, "group", MAX_GROUP_ID);
  const auto kind_name = Str(o, "kind", 8);
  const auto you = Str(o, "you", 36);
  std::string why;
  const auto parsed = ParseMembers(Get(o, "members"), &why);
  const std::string my_uid = LocalUid();
  const GroupKind kind = kind_name == "room"  ? GroupKind::Room :
                         kind_name == "match" ? GroupKind::Match :
                                                GroupKind::None;
  if (!group || !IsGroupId(*group) || kind == GroupKind::None || !you || *you != my_uid ||
      !parsed)
  {
    ++s.bad_groups;
    Log(fmt::format("chat-group refused: {}", why.empty() ? "fields" : why));
    return;
  }
  if (std::ranges::count(*parsed, my_uid, &ParsedMember::uid) != 1)
  {
    ++s.bad_groups;
    Log("chat-group refused: not a member");
    return;
  }
  if (!s.identity)
  {
    Log("chat-group ignored: no identity key");
    return;
  }

  const bool fresh = s.kind == GroupKind::None || *group != s.group;
  if (fresh)
  {
    LeaveLocked("new group");
    s.kind = kind;
    s.group = *group;
    s.you = my_uid;
    s.key = CC::NewGroupKey();
    ++s.groups;
    Log(fmt::format("in {} ({} members)", s.group, parsed->size()));
  }

  std::vector<MemberState> next;
  for (const ParsedMember& p : *parsed)
  {
    MemberState m;
    if (const MemberState* old = FindMember(p.uid))
      m = *old;
    const bool is_me = p.uid == my_uid;
    if (is_me && p.id_key != s.identity->PublicKey())
      Log("the server shows another identity key for this player");
    if (!is_me && m.uid.empty())
    {
      m.refilled = now;
      if (!fresh)
        AddSystemLine(fmt::format("{} joined", p.name));
    }
    m.uid = p.uid;
    m.name = p.name;
    m.code = p.code;
    if (m.id_key != p.id_key)
    {
      m.kx.reset();
      m.pair.reset();
      m.last_seq = 0;
      m.past_keys.clear();
      m.id_key = p.id_key;
    }
    std::optional<CC::Key32> kx;
    if (p.kx && CC::Verify(p.id_key, *p.kx_sig, CC::KxSigMessage(s.group, *p.kx)))
      kx = p.kx;
    else if (p.kx)
      ++s.bad_keys;
    if (kx != m.kx)
    {
      // A new group key (the member reconnected): its sequence numbers start over, unless the
      // key was seen before.
      if (m.kx)
      {
        std::erase_if(m.past_keys, [&](const auto& e) { return e.first == *m.kx; });
        m.past_keys.emplace_back(*m.kx, m.last_seq);
        if (m.past_keys.size() > MAX_PAST_KEYS)
          m.past_keys.erase(m.past_keys.begin());
      }
      m.kx = kx;
      if (m.pair)
        CC::Wipe(m.pair->data(), m.pair->size());
      m.pair.reset();
      m.last_seq = 0;
      if (kx)
      {
        const auto seen = std::ranges::find(m.past_keys, *kx, &std::pair<CC::Key32, u64>::first);
        if (seen != m.past_keys.end())
          m.last_seq = seen->second;
      }
      if (kx && !is_me)
      {
        m.pair = CC::PairKey(*s.key, *kx, s.group);
        if (!m.pair)
          ++s.bad_keys;
      }
    }
    next.push_back(std::move(m));
  }
  if (!fresh)
  {
    for (const MemberState& old : s.members)
    {
      if (old.uid != my_uid && std::ranges::find(next, old.uid, &MemberState::uid) == next.end())
        AddSystemLine(fmt::format("{} left", old.name));
    }
  }
  for (MemberState& old : s.members)
  {
    if (old.pair && std::ranges::find(next, old.uid, &MemberState::uid) == next.end())
      CC::Wipe(old.pair->data(), old.pair->size());
  }
  s.members = std::move(next);

  const MemberState* me = FindMember(my_uid);
  if (!me->kx || *me->kx != s.key->public_key)
    QueueKeyLocked(now);
  Touch();
}

// ---- chat-msg ----

void OnMessage(const picojson::object& o, Clock::time_point now)
{
  const auto group = Str(o, "group", MAX_GROUP_ID);
  const auto from = Str(o, "from", 36);
  const auto box_hex = Str(o, "box", 2 * CC::MAX_BOX_BYTES);
  if (!group || !from || !box_hex)
  {
    ++s.drop_json;
    return;
  }
  MemberState* m = s.kind == GroupKind::None || *group != s.group ? nullptr : FindMember(*from);
  if (!m || m->uid == s.you || !m->pair)
  {
    ++s.drop_member;
    return;
  }
  const auto box = CC::FromHex(*box_hex, CC::MAX_BOX_BYTES);
  if (!box)
  {
    ++s.drop_box;
    return;
  }
  const auto plain = CC::Open(*m->pair, CC::AssociatedData(s.group, m->uid, s.you), *box);
  if (!plain)
  {
    ++s.drop_box;
    return;
  }
  const auto p = CC::DecodePlaintext(*plain);
  if (!p)
  {
    ++s.drop_plaintext;
    return;
  }
  if (p->seq <= m->last_seq)
  {
    ++s.drop_replay;
    return;
  }
  if (!CC::Verify(m->id_key, p->signature, CC::MessageSigMessage(s.group, m->uid, p->seq, p->text)))
  {
    ++s.drop_signature;
    return;
  }
  m->last_seq = p->seq;

  // Per-sender rate (mm limits too; this holds if it doesn't).
  const double elapsed = std::chrono::duration<double>(now - m->refilled).count();
  m->tokens = std::min<double>(RECEIVE_BURST, m->tokens + elapsed * RECEIVE_PER_SECOND);
  m->refilled = now;
  if (m->tokens < 1.0)
  {
    ++s.drop_rate;
    return;
  }
  m->tokens -= 1.0;

  auto text = ChatText::Clean(p->text);
  if (!text)
  {
    ++s.drop_text;
    return;
  }
  ++s.accepted;
  StoredLine l;
  l.line.kind = Line::Kind::Message;
  l.line.uid = m->uid;
  l.line.name = m->name;
  l.line.text = std::move(*text);
  l.line.color = ColorOf(m->uid);
  l.seq = p->seq;
  l.sig = p->signature;
  l.raw = p->text;
  AddLine(std::move(l));
}

std::string NameOf(std::string_view uid)
{
  const MemberState* m = FindMember(uid);
  return m ? m->name : std::string("that player");
}

std::string Title()
{
  if (s.kind == GroupKind::Room)
  {
    // room-<CODE>-<n>
    const std::string_view g = s.group;
    const size_t a = g.find('-');
    const size_t b = a == std::string_view::npos ? a : g.find('-', a + 1);
    if (b != std::string_view::npos)
      return fmt::format("Room {}", g.substr(a + 1, b - a - 1));
    return "Room";
  }
  for (const MemberState& m : s.members)
  {
    if (m.uid != s.you)
      return m.code.empty() ? m.name : fmt::format("{} ({})", m.name, m.code);
  }
  return "Chat";
}

void ProcessPendingLocked()
{
  while (!s.pending.empty())
  {
    std::string text = std::move(s.pending.front());
    s.pending.pop_front();
    if (s.kind == GroupKind::None || !s.identity)
      break;
    const u64 seq = ++s.seq;
    const CC::Sig64 sig = s.identity->Sign(CC::MessageSigMessage(s.group, s.you, seq, text));
    const std::vector<u8> plain = CC::EncodePlaintext(seq, sig, text);
    picojson::object boxes;
    for (const MemberState& m : s.members)
    {
      if (m.uid == s.you || !m.pair)
        continue;
      const std::vector<u8> box =
          CC::Seal(*m.pair, CC::RandomNonce(), CC::AssociatedData(s.group, s.you, m.uid), plain);
      boxes[m.uid] = picojson::value(CC::ToHex(box));
    }
    if (boxes.empty())
    {
      AddSystemLine("Nobody can read chat yet.");
      continue;
    }
    picojson::object o;
    o["type"] = picojson::value("chat-send");
    o["group"] = picojson::value(s.group);
    o["boxes"] = picojson::value(boxes);
    QueueOutbox(o);
    ++s.sent;
  }
}
}  // namespace

std::string IdentityPublicKeyHex()
{
  std::lock_guard lk(s_mutex);
  LoadIdentityLocked();
  return s.identity_hex;
}

bool OnServerMessage(const std::string& packet)
{
  // A cheap look first: everything else on the connection is the Rooms code's.
  if (packet.find("\"chat-") == std::string::npos || packet.size() > MAX_PACKET)
    return false;
  if (!Gprb::PeerData::JsonSafeToParse(packet))
    return false;
  picojson::value root;
  if (!picojson::parse(root, packet).empty() || !root.is<picojson::object>())
    return false;
  const auto& o = root.get<picojson::object>();
  const auto type = Str(o, "type", 32);
  if (!type || !type->starts_with("chat-"))
    return false;

  const auto now = Clock::now();
  std::lock_guard lk(s_mutex);
  ++s.received;
  if (*type == "chat-group")
  {
    OnGroup(o, now);
  }
  else if (*type == "chat-msg")
  {
    OnMessage(o, now);
  }
  else if (*type == "chat-error")
  {
    const auto error = Str(o, "error", MAX_ERROR_BYTES);
    if (s.kind != GroupKind::None && error)
      AddSystemLine(CleanOr(*error, ChatText::MAX_MESSAGE_CODE_POINTS, "Chat error."));
  }
  else if (*type == "chat-reported")
  {
    const auto from = Str(o, "from", 36);
    if (s.kind != GroupKind::None && from && IsUid(*from))
      AddSystemLine(fmt::format("Reported {}.", NameOf(*from)));
  }
  else
  {
    ++s.drop_json;
  }
  return true;
}

std::vector<std::string> TakeOutbox()
{
  // A match's group is shown once its session has connected. Checked here, on the Rooms
  // thread's tick, outside the chat lock (the client takes its own).
  bool check_match;
  {
    std::lock_guard lk(s_mutex);
    check_match = s.kind == GroupKind::Match && !s.match_connected &&
                  Clock::now() - s.match_checked > MATCH_STATE_EVERY;
  }
  const bool connected =
      check_match && Client::GetMatchState().state == Matchmaking::State::ConnectionSuccess;

  std::lock_guard lk(s_mutex);
  if (check_match && s.kind == GroupKind::Match)
  {
    s.match_checked = Clock::now();
    if (connected && !s.match_connected)
    {
      s.match_connected = true;
      Touch();
    }
  }
  ProcessPendingLocked();
  std::vector<std::string> out(std::make_move_iterator(s.outbox.begin()),
                               std::make_move_iterator(s.outbox.end()));
  s.outbox.clear();
  return out;
}

void OnConnectionLost()
{
  std::lock_guard lk(s_mutex);
  s.outbox.clear();
  LeaveLocked("connection lost");
}

void LeaveMatchGroup()
{
  std::lock_guard lk(s_mutex);
  if (s.kind != GroupKind::Match)
    return;
  picojson::object o;
  o["type"] = picojson::value("chat-leave");
  o["group"] = picojson::value(s.group);
  QueueOutbox(o);
  LeaveLocked("session ended");
}

std::optional<View> GetView(u64 known_serial)
{
  std::lock_guard lk(s_mutex);
  LoadHiddenLocked();
  if (known_serial == s.serial)
    return std::nullopt;
  View v;
  v.serial = s.serial;
  v.active = s.kind == GroupKind::Room || (s.kind == GroupKind::Match && s.match_connected);
  v.room = s.kind == GroupKind::Room;
  v.title = Title();
  v.last_message_ms = s.last_message_ms;
  for (size_t i = 0; i < s.members.size(); ++i)
  {
    const MemberState& m = s.members[i];
    Member out;
    out.uid = m.uid;
    out.name = m.name;
    out.code = m.code;
    out.is_me = m.uid == s.you;
    out.has_key = m.kx.has_value();
    out.hidden = s.hidden.contains(m.uid);
    out.color = static_cast<int>(i % MAX_MEMBERS);
    v.members.push_back(std::move(out));
  }
  v.lines.reserve(s.lines.size());
  for (const StoredLine& l : s.lines)
    v.lines.push_back(l.line);
  return v;
}

SendResult Send(const std::string& text)
{
  auto cleaned = ChatText::Clean(text);
  if (!cleaned)
    return SendResult::Empty;
  const auto now = Clock::now();
  std::lock_guard lk(s_mutex);
  if (s.kind == GroupKind::None || !s.identity)
    return SendResult::NoGroup;
  if (!AnyReader())
    return SendResult::NoReaders;
  while (!s.sent_times.empty() && now - s.sent_times.front() > std::chrono::minutes(1))
    s.sent_times.pop_front();
  const size_t recent = static_cast<size_t>(std::ranges::count_if(
      s.sent_times, [&](Clock::time_point t) { return now - t < SEND_BURST_WINDOW; }));
  if (recent >= SEND_BURST || s.sent_times.size() >= SEND_PER_MINUTE ||
      s.pending.size() >= MAX_PENDING)
  {
    return SendResult::TooFast;
  }
  s.sent_times.push_back(now);
  s.pending.push_back(*cleaned);

  StoredLine l;
  l.line.kind = Line::Kind::Own;
  l.line.uid = s.you;
  const MemberState* me = FindMember(s.you);
  l.line.name = me ? me->name : std::string("You");
  l.line.text = std::move(*cleaned);
  l.line.color = ColorOf(s.you);
  AddLine(std::move(l));
  return SendResult::Sent;
}

void SetHidden(const std::string& uid, bool hidden)
{
  if (!IsUid(uid))
    return;
  std::lock_guard lk(s_mutex);
  LoadHiddenLocked();
  if (hidden == s.hidden.contains(uid))
    return;
  if (hidden)
  {
    if (s.hidden.size() >= MAX_HIDDEN)
      s.hidden.erase(s.hidden.begin());
    s.hidden.insert(uid);
  }
  else
  {
    s.hidden.erase(uid);
  }
  SaveHiddenLocked();
  Touch();
}

bool IsHidden(const std::string& uid)
{
  std::lock_guard lk(s_mutex);
  LoadHiddenLocked();
  return s.hidden.contains(uid);
}

bool Report(const std::string& uid, const std::string& reason)
{
  std::lock_guard lk(s_mutex);
  if (s.kind == GroupKind::None || uid == s.you || !FindMember(uid))
    return false;
  // The newest messages that fit mm's packet (20 of 300 bytes don't, with JSON's escapes).
  picojson::array messages;
  size_t bytes = 0;
  for (auto it = s.lines.rbegin(); it != s.lines.rend() && messages.size() < MAX_REPORTED_MESSAGES;
       ++it)
  {
    if (it->line.kind != Line::Kind::Message || it->line.uid != uid)
      continue;
    picojson::object m;
    m["seq"] = picojson::value(static_cast<double>(it->seq));
    m["text"] = picojson::value(it->raw);
    m["sig"] = picojson::value(CC::ToHex(it->sig));
    const size_t size = picojson::value(m).serialize().size() + 1;
    if (bytes + size > MAX_REPORT_MESSAGES_BYTES)
      break;
    bytes += size;
    messages.emplace_back(std::move(m));
  }
  if (messages.empty())
  {
    AddSystemLine(fmt::format("{} has sent nothing to report.", NameOf(uid)));
    return false;
  }
  std::reverse(messages.begin(), messages.end());
  picojson::object o;
  o["type"] = picojson::value("chat-report");
  o["group"] = picojson::value(s.group);
  o["from"] = picojson::value(uid);
  o["reason"] = picojson::value(std::string(
      ChatText::Truncate(ChatText::Clean(reason).value_or(""), MAX_REASON_BYTES, MAX_REASON_BYTES)));
  o["messages"] = picojson::value(messages);
  QueueOutbox(o);
  ++s.reports;
  Log(fmt::format("reported {} ({} messages)", uid, messages.size()));
  return true;
}

void ResetForTesting(const std::string& local_uid)
{
  std::lock_guard lk(s_mutex);
  WipeKey();
  s = State{};
  s_uid_for_testing = local_uid;
}

picojson::object Status()
{
  std::lock_guard lk(s_mutex);
  picojson::object o;
  o["identity_key"] = picojson::value(s.identity_hex);
  o["identity_error"] = picojson::value(s.identity_error);
  o["kind"] = picojson::value(s.kind == GroupKind::Room  ? "room" :
                              s.kind == GroupKind::Match ? "match" :
                                                           "none");
  o["group"] = picojson::value(s.group);
  o["you"] = picojson::value(s.you);
  o["title"] = picojson::value(s.kind == GroupKind::None ? std::string() : Title());
  o["match_connected"] = picojson::value(s.match_connected);
  o["seq"] = picojson::value(static_cast<double>(s.seq));
  picojson::array members;
  for (const MemberState& m : s.members)
  {
    picojson::object mo;
    mo["uid"] = picojson::value(m.uid);
    mo["name"] = picojson::value(m.name);
    mo["code"] = picojson::value(m.code);
    mo["is_me"] = picojson::value(m.uid == s.you);
    mo["has_key"] = picojson::value(m.kx.has_value());
    mo["can_read"] = picojson::value(m.pair.has_value());
    mo["last_seq"] = picojson::value(static_cast<double>(m.last_seq));
    mo["hidden"] = picojson::value(s.hidden.contains(m.uid));
    members.emplace_back(std::move(mo));
  }
  o["members"] = picojson::value(members);
  picojson::array lines;
  for (const StoredLine& l : s.lines)
  {
    picojson::object lo;
    lo["id"] = picojson::value(static_cast<double>(l.line.id));
    lo["kind"] = picojson::value(l.line.kind == Line::Kind::Message ? "message" :
                                 l.line.kind == Line::Kind::Own     ? "own" :
                                                                      "system");
    lo["uid"] = picojson::value(l.line.uid);
    lo["name"] = picojson::value(l.line.name);
    lo["text"] = picojson::value(l.line.text);
    lo["seq"] = picojson::value(static_cast<double>(l.seq));
    lines.emplace_back(std::move(lo));
  }
  o["lines"] = picojson::value(lines);
  picojson::object c;
  const auto put = [&](const char* k, u64 v) { c[k] = picojson::value(static_cast<double>(v)); };
  put("received", s.received);
  put("accepted", s.accepted);
  put("sent", s.sent);
  put("groups", s.groups);
  put("reports", s.reports);
  put("drop_json", s.drop_json);
  put("drop_member", s.drop_member);
  put("drop_box", s.drop_box);
  put("drop_plaintext", s.drop_plaintext);
  put("drop_replay", s.drop_replay);
  put("drop_signature", s.drop_signature);
  put("drop_text", s.drop_text);
  put("drop_rate", s.drop_rate);
  put("bad_keys", s.bad_keys);
  put("bad_groups", s.bad_groups);
  o["counters"] = picojson::value(c);
  o["outbox"] = picojson::value(static_cast<double>(s.outbox.size() + s.pending.size()));
  picojson::array log;
  for (const std::string& l : s.log)
    log.emplace_back(l);
  o["log"] = picojson::value(log);
  return o;
}

std::optional<std::string> HarnessRequest(const std::string& command, const picojson::object& args)
{
  if (command == "chat_send")
  {
    const auto text = Str(args, "text", 4096);
    if (!text)
      return "text: a string";
    switch (Send(*text))
    {
    case SendResult::Sent:
      return std::nullopt;
    case SendResult::Empty:
      return "empty after cleaning";
    case SendResult::NoGroup:
      return "not in a chat group";
    case SendResult::NoReaders:
      return "nobody can read chat yet";
    case SendResult::TooFast:
      return "too fast";
    }
    return "unknown";
  }
  if (command == "chat_hide")
  {
    const auto uid = Str(args, "uid", 36);
    const picojson::value* hidden = Get(args, "hidden");
    if (!uid || !IsUid(*uid))
      return "uid: a uid";
    SetHidden(*uid, !hidden || !hidden->is<bool>() || hidden->get<bool>());
    return std::nullopt;
  }
  if (command == "chat_report")
  {
    const auto uid = Str(args, "uid", 36);
    if (!uid || !IsUid(*uid))
      return "uid: a uid";
    if (!Report(*uid, Str(args, "reason", 4096).value_or("")))
      return "nothing to report";
    return std::nullopt;
  }
  if (command == "chat_leave")
  {
    LeaveMatchGroup();
    return std::nullopt;
  }
  return "unknown chat command";
}
}  // namespace Online::Chat
