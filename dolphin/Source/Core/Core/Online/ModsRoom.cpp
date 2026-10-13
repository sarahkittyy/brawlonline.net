// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/ModsRoom.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/Online/OnlineClient.h"
#include "Core/Online/User.h"
#include "Core/Rollback/MachineSession.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Online::ModsRoom
{
namespace
{
constexpr size_t MAX_MESSAGE = 16 * 1024;
constexpr size_t BUILD_HASH_LEN = 64;
constexpr char NOT_FOUND[] = "Room not found.";

struct Member
{
  std::string name;
  std::string code;
  std::string controller;
  std::string host;
  u16 port = 0;
  std::string lan_host;
  u16 lan_port = 0;
};

struct View
{
  std::string code;
  bool is_public = false;
  std::string build_hash, build_name;
  int host = 0;  // slot 1-4
  int you = 0;   // slot 1-4
  std::string token;
  std::array<bool, 4> open{};
  std::array<std::optional<Member>, 4> slots;
};

enum class Kind
{
  None,
  Host,
  Join,
};

struct Room
{
  Kind kind = Kind::None;
  std::string build_hash, build_name;
  std::string join_code;
  bool is_public = true;
  std::string controller = "gc";
  u64 sent_on = 0;  // the connection the create or join went on
  // The room this player is in (for a rejoin on a new connection: the same code and slot).
  std::string room_code;
  int room_slot = 0;
  bool rejoining = false;
  bool leaving = false;
  bool done = false;  // out of the room for good (left, removed, could not join)
  std::optional<View> view;
  std::string error;
  u64 states = 0;
  std::deque<std::string> outbox;
  std::vector<std::string> log;
  // The launcher's hand-off (Files).
  picojson::value last_request;
  std::string status_written;
  std::chrono::steady_clock::time_point status_at{};
};

struct State : Room
{
  std::mutex mutex;
};

State s;
std::string s_command_line;  // --mods-room (before boot, then read once)

void Log(std::string line)
{
  INFO_LOG_FMT(NETPLAY, "ModsRoom: {}", line);
  if (s.log.size() >= 64)
    s.log.erase(s.log.begin());
  s.log.push_back(std::move(line));
}

bool ValidHash(const std::string& h)
{
  if (h.size() != BUILD_HASH_LEN)
    return false;
  for (char c : h)
  {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

std::string Str(const picojson::object& o, const char* key, size_t max)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<std::string>())
    return {};
  return it->second.get<std::string>().substr(0, max);
}

double Num(const picojson::object& o, const char* key, double def)
{
  const auto it = o.find(key);
  return it != o.end() && it->second.is<double>() ? it->second.get<double>() : def;
}

bool Flag(const picojson::object& o, const char* key, bool def)
{
  const auto it = o.find(key);
  return it != o.end() && it->second.is<bool>() ? it->second.get<bool>() : def;
}

const picojson::object* Obj(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  return it != o.end() && it->second.is<picojson::object>() ? &it->second.get<picojson::object>() :
                                                              nullptr;
}

// An endpoint {host, port}; false when it is missing or not valid.
bool EndpointOf(const picojson::object& o, const char* key, std::string* host, u16* port)
{
  const picojson::object* e = Obj(o, key);
  if (!e)
    return false;
  const std::string h = Str(*e, "host", 64);
  const double p = Num(*e, "port", 0);
  if (h.empty() || p < 1 || p > 65535)
    return false;
  *host = h;
  *port = static_cast<u16>(p);
  return true;
}

std::optional<View> ParseState(const picojson::object& o)
{
  View v;
  v.code = Str(o, "code", 16);
  v.is_public = Flag(o, "public", false);
  if (const picojson::object* b = Obj(o, "build"))
  {
    v.build_hash = Str(*b, "hash", BUILD_HASH_LEN);
    v.build_name = Str(*b, "name", 64);
  }
  v.host = static_cast<int>(Num(o, "host", 0));
  v.you = static_cast<int>(Num(o, "you", 0));
  v.token = Str(o, "token", 128);
  if (v.code.empty() || v.you < 1 || v.you > 4 || v.host < 1 || v.host > 4)
    return std::nullopt;
  const auto sl = o.find("slots");
  if (sl == o.end() || !sl->second.is<picojson::array>())
    return std::nullopt;
  for (const auto& e : sl->second.get<picojson::array>())
  {
    if (!e.is<picojson::object>())
      continue;
    const auto& so = e.get<picojson::object>();
    const int slot = static_cast<int>(Num(so, "slot", 0));
    if (slot < 1 || slot > 4)
      continue;
    v.open[slot - 1] = Flag(so, "open", false);
    const picojson::object* po = Obj(so, "player");
    if (!po)
      continue;
    Member m;
    m.name = Str(*po, "displayName", 64);
    m.code = Str(*po, "connectCode", 16);
    m.controller = Str(*po, "controller", 16);
    EndpointOf(*po, "endpoint", &m.host, &m.port);
    EndpointOf(*po, "lan", &m.lan_host, &m.lan_port);
    v.slots[slot - 1] = std::move(m);
  }
  if (!v.slots[v.you - 1])
    return std::nullopt;
  return v;
}

std::string IsoUtcNow()
{
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

picojson::object RoomJson(const View& v)
{
  picojson::object r;
  r["code"] = picojson::value(v.code);
  r["public"] = picojson::value(v.is_public);
  picojson::object build;
  build["hash"] = picojson::value(v.build_hash);
  build["name"] = picojson::value(v.build_name);
  r["build"] = picojson::value(build);
  r["host"] = picojson::value(static_cast<double>(v.host));
  r["you"] = picojson::value(static_cast<double>(v.you));
  picojson::array slots;
  for (int i = 0; i < 4; ++i)
  {
    picojson::object so;
    so["slot"] = picojson::value(static_cast<double>(i + 1));
    so["open"] = picojson::value(v.open[i]);
    if (const auto& m = v.slots[i])
    {
      picojson::object p;
      p["displayName"] = picojson::value(m->name);
      p["connectCode"] = picojson::value(m->code);
      p["controller"] = picojson::value(m->controller);
      p["connected"] = picojson::value(!m->host.empty());
      so["player"] = picojson::value(p);
    }
    slots.emplace_back(so);
  }
  r["slots"] = picojson::value(slots);
  return r;
}

std::string CreateJson()
{
  picojson::object build;
  build["hash"] = picojson::value(s.build_hash);
  build["name"] = picojson::value(s.build_name);
  picojson::object m;
  m["type"] = picojson::value("mods-create");
  m["build"] = picojson::value(build);
  m["public"] = picojson::value(s.is_public);
  m["controller"] = picojson::value(s.controller);
  return picojson::value(m).serialize();
}

std::string JoinJson(const std::string& code, int slot)
{
  picojson::object build;
  build["hash"] = picojson::value(s.build_hash);
  picojson::object m;
  m["type"] = picojson::value("mods-join");
  m["code"] = picojson::value(code);
  m["build"] = picojson::value(build);
  m["controller"] = picojson::value(s.controller);
  if (slot > 0)
    m["slot"] = picojson::value(static_cast<double>(slot));
  return picojson::value(m).serialize();
}
}  // namespace

void SetCommandLine(std::string json)
{
  s_command_line = std::move(json);
}

std::optional<Machine::Options> FromEnvironment(std::string* error)
{
  error->clear();
  const char* env = !s_command_line.empty() ? s_command_line.c_str() : std::getenv("PPR_MODS_ROOM");
  if (!env || !*env)
    return std::nullopt;
  picojson::value v;
  if (const std::string err = picojson::parse(v, env); !err.empty() || !v.is<picojson::object>())
  {
    *error = "PPR_MODS_ROOM is not a JSON object";
    return std::nullopt;
  }
  const auto& o = v.get<picojson::object>();
  const picojson::object* build = Obj(o, "build");
  const std::string hash = build ? Str(*build, "hash", BUILD_HASH_LEN + 1) : "";
  if (!ValidHash(hash))
  {
    *error = "PPR_MODS_ROOM: build.hash must be 64 lowercase hex digits";
    return std::nullopt;
  }
  const bool host = Flag(o, "host", false);
  const std::string join = Str(o, "join", 16);
  if (host == !join.empty())
  {
    *error = "PPR_MODS_ROOM: either \"host\": true or \"join\": \"<code>\"";
    return std::nullopt;
  }
  const std::string controller = o.count("controller") ? Str(o, "controller", 16) : "gc";
  const double delay = Num(o, "delay", 2), port = Num(o, "port", 0), pad = Num(o, "pad", 0);
  if ((controller != "gc" && controller != "wiimote") || delay < 0 || delay > 9 || port < 0 ||
      port > 65535 || pad < 0 || pad > 3)
  {
    *error = "PPR_MODS_ROOM: controller, delay, port or pad out of range";
    return std::nullopt;
  }
  Machine::Options opts;
  opts.room = true;
  opts.join = !host;
  opts.local_slot = 0;  // a joiner's comes with the room
  opts.local_port = static_cast<u16>(port);
  opts.delay = static_cast<int>(delay);
  opts.local_pad = static_cast<int>(pad);
  opts.controller =
      controller == "wiimote" ? Machine::Controller::WiiRemote : Machine::Controller::GameCube;
  opts.name = Client::GetUser().GetUserInfo().display_name.substr(0, 64);
  {
    std::lock_guard lk(s.mutex);
    static_cast<Room&>(s) = Room{};
    s.kind = host ? Kind::Host : Kind::Join;
    s.build_hash = hash;
    s.build_name = Str(*build, "name", 64);
    s.join_code = join;
    s.is_public = Flag(o, "public", true);
    s.controller = controller;
    Log(host ? fmt::format("will open a room for build {}", hash.substr(0, 8)) :
               fmt::format("will join room {} (build {})", join, hash.substr(0, 8)));
  }
  return opts;
}

bool OnServerMessage(const std::string& text)
{
  // Cheap test first: every other message on the connection goes elsewhere.
  if (text.size() > MAX_MESSAGE || text.find("\"mods-") == std::string::npos)
    return false;
  picojson::value v;
  if (!picojson::parse(v, text).empty() || !v.is<picojson::object>())
    return false;
  const auto& o = v.get<picojson::object>();
  const std::string type = Str(o, "type", 32);
  if (type.rfind("mods-", 0) != 0)
    return false;

  std::optional<View> view;
  bool stop = false;
  {
    std::lock_guard lk(s.mutex);
    if (type == "mods-state")
    {
      view = ParseState(o);
      if (!view)
      {
        Log("a mods-state that is not valid");
        return true;
      }
      if (!s.view || s.view->code != view->code || s.view->you != view->you)
        Log(fmt::format("in room {} (slot {}, host slot {})", view->code, view->you, view->host));
      s.view = view;
      s.room_code = view->code;
      s.room_slot = view->you;
      s.rejoining = false;
      s.error.clear();
      ++s.states;
    }
    else if (type == "mods-left")
    {
      Log(s.leaving ? "left the room" : "removed from the room");
      if (!s.leaving)
        s.error = "You were removed from the room.";
      s.view.reset();
      s.room_code.clear();
      s.done = true;
      stop = true;
    }
    else if (type == "mods-error")
    {
      const std::string op = Str(o, "op", 32);
      const std::string why = Str(o, "error", 200);
      Log(fmt::format("{}: {}", op, why));
      s.error = why;
      if (op == "mods-join" && s.rejoining && why == NOT_FOUND && Machine::IsRunning())
      {
        // Back after the connection dropped, and the room is gone: this player's session still
        // runs, so it opens a new room for it.
        s.rejoining = false;
        s.room_code.clear();
        s.kind = Kind::Host;
        s.outbox.push_back(CreateJson());
        Log("the room is gone: opening a new one");
      }
      else if ((op == "mods-create" || op == "mods-join") && !s.view)
      {
        s.done = true;
      }
    }
  }
  if (stop)
  {
    Machine::Stop();
    return true;
  }
  if (!view)
    return true;

  std::vector<Machine::RoomMember> members;
  bool probed = false;
  for (int i = 0; i < 4; ++i)
  {
    const auto& m = view->slots[i];
    if (!m)
      continue;
    Machine::RoomMember rm;
    rm.slot = i;
    rm.code = m->code;
    rm.name = m->name;
    rm.host = m->host;
    rm.port = m->port;
    rm.lan_host = m->lan_host;
    rm.lan_port = m->lan_port;
    members.push_back(std::move(rm));
    if (i == view->you - 1)
      probed = !m->host.empty();
  }
  Machine::SetRoom(view->you - 1, members);
  if (!probed)
  {
    Machine::ProbeEndpoint(Config::GetMatchmakingHost(),
                           static_cast<u16>(Config::GetMatchmakingPort()), view->token);
  }
  return true;
}

std::vector<std::string> TakeOutbox(u64 connection)
{
  std::lock_guard lk(s.mutex);
  std::vector<std::string> out;
  if (s.kind == Kind::None || s.done)
  {
    s.outbox.clear();
    return out;
  }
  if (s.sent_on != connection)
  {
    // A new connection: mm forgot this player's membership with the old one.
    s.sent_on = connection;
    s.outbox.clear();
    s.view.reset();
    if (!s.room_code.empty())
    {
      s.rejoining = true;
      out.push_back(JoinJson(s.room_code, s.room_slot));
      Log(fmt::format("joining room {} again (slot {})", s.room_code, s.room_slot));
    }
    else if (s.kind == Kind::Host)
    {
      out.push_back(CreateJson());
      Log("opening the room");
    }
    else
    {
      out.push_back(JoinJson(s.join_code, 0));
      Log(fmt::format("joining room {}", s.join_code));
    }
  }
  while (!s.outbox.empty())
  {
    out.push_back(std::move(s.outbox.front()));
    s.outbox.pop_front();
  }
  return out;
}

void Files(const std::string& dir)
{
  {
    std::lock_guard lk(s.mutex);
    if (s.kind == Kind::None && s.log.empty())
      return;
  }
  // The launcher's request, if any: read, deleted, answered in the status.
  const std::string req_path = dir + "mods-request.json";
  if (File::Exists(req_path))
  {
    std::string text;
    const bool read = File::GetSize(req_path) <= 4096 && File::ReadFileToString(req_path, text);
    File::Delete(req_path, File::IfAbsentBehavior::NoConsoleWarning);
    picojson::value v;
    picojson::object answer;
    if (read && picojson::parse(v, text).empty() && v.is<picojson::object>())
    {
      const auto& o = v.get<picojson::object>();
      const std::string id = Str(o, "id", 100);
      answer["id"] = picojson::value(id);
      if (Num(o, "version", 0) != 1 || id.empty())
      {
        answer["result"] = picojson::value("failed");
        answer["error"] = picojson::value("not version 1 with an id");
      }
      else if (const auto error = Request(o))
      {
        answer["result"] = picojson::value("failed");
        answer["error"] = picojson::value(*error);
      }
      else
      {
        answer["result"] = picojson::value("ok");
      }
    }
    else
    {
      answer["result"] = picojson::value("failed");
      answer["error"] = picojson::value("not a JSON object");
    }
    std::lock_guard lk(s.mutex);
    s.last_request = picojson::value(answer);
  }

  // The session as the launcher shows it.
  picojson::object session;
  const picojson::value ms = Machine::Status();
  if (ms.is<picojson::object>())
  {
    const auto& m = ms.get<picojson::object>();
    for (const char* key : {"phase", "error", "players", "epoch", "frame", "slot"})
    {
      if (const auto it = m.find(key); it != m.end())
        session[key] = it->second;
    }
    picojson::array peers;
    if (const auto it = m.find("peers"); it != m.end() && it->second.is<picojson::array>())
    {
      for (const auto& pv : it->second.get<picojson::array>())
      {
        if (!pv.is<picojson::object>())
          continue;
        const auto& po = pv.get<picojson::object>();
        picojson::object p;
        p["slot"] = picojson::value(Num(po, "slot", -1) + 1);
        p["connected"] = picojson::value(Flag(po, "connected", false));
        p["rttMs"] = picojson::value(Num(po, "rtt_ms", -1));
        peers.emplace_back(p);
      }
    }
    session["peers"] = picojson::value(peers);
  }

  picojson::object o;
  std::string key;
  {
    std::lock_guard lk(s.mutex);
    o["version"] = picojson::value(1.0);
    o["kind"] = picojson::value(s.kind == Kind::Host ? "host" : s.kind == Kind::Join ? "join" : "none");
    o["state"] = picojson::value(s.done ? "left" : s.view ? "in" : "joining");
    o["error"] = picojson::value(s.error);
    o["room"] = s.view ? picojson::value(RoomJson(*s.view)) : picojson::value();
    o["session"] = picojson::value(session);
    o["lastRequest"] = s.last_request;
#ifdef _WIN32
    o["pid"] = picojson::value(static_cast<double>(GetCurrentProcessId()));
#else
    o["pid"] = picojson::value(static_cast<double>(getpid()));
#endif
    // The frame counts up all the time: not a change by itself.
    picojson::object keyed = o;
    picojson::object ks = session;
    ks.erase("frame");
    keyed["session"] = picojson::value(ks);
    key = picojson::value(keyed).serialize();
    const auto now = std::chrono::steady_clock::now();
    if (key == s.status_written && now - s.status_at < std::chrono::seconds(5))
      return;
    s.status_written = key;
    s.status_at = now;
  }
  o["updatedAt"] = picojson::value(IsoUtcNow());
  File::CreateFullPath(dir);
  const std::string tmp = dir + "mods-room.json.tmp";
  const std::string path = dir + "mods-room.json";
  if (!File::WriteStringToFile(tmp, picojson::value(o).serialize(true)) || !File::Rename(tmp, path))
    WARN_LOG_FMT(NETPLAY, "ModsRoom: cannot write {}", path);
}

picojson::object Status()
{
  std::lock_guard lk(s.mutex);
  picojson::object o;
  o["kind"] = picojson::value(s.kind == Kind::Host ? "host" : s.kind == Kind::Join ? "join" : "none");
  o["error"] = picojson::value(s.error);
  o["done"] = picojson::value(s.done);
  o["states"] = picojson::value(static_cast<double>(s.states));
  o["probe"] = picojson::value(Machine::ProbeState());
  if (s.view)
  {
    const View& v = *s.view;
    picojson::object r;
    r["code"] = picojson::value(v.code);
    r["public"] = picojson::value(v.is_public);
    r["build"] = picojson::value(v.build_hash);
    r["build_name"] = picojson::value(v.build_name);
    r["host"] = picojson::value(static_cast<double>(v.host));
    r["you"] = picojson::value(static_cast<double>(v.you));
    picojson::array slots;
    for (int i = 0; i < 4; ++i)
    {
      picojson::object so;
      so["slot"] = picojson::value(static_cast<double>(i + 1));
      so["open"] = picojson::value(v.open[i]);
      if (const auto& m = v.slots[i])
      {
        so["name"] = picojson::value(m->name);
        so["code"] = picojson::value(m->code);
        so["controller"] = picojson::value(m->controller);
        so["endpoint"] = picojson::value(m->host.empty() ? "" : fmt::format("{}:{}", m->host, m->port));
        so["lan"] = picojson::value(m->lan_host.empty() ? "" :
                                                          fmt::format("{}:{}", m->lan_host, m->lan_port));
      }
      slots.emplace_back(so);
    }
    r["slots"] = picojson::value(slots);
    o["room"] = picojson::value(r);
  }
  picojson::array log;
  for (const auto& l : s.log)
    log.emplace_back(l);
  o["log"] = picojson::value(log);
  return o;
}

std::optional<std::string> Request(const picojson::object& args)
{
  const std::string op = Str(args, "op", 16);
  picojson::object m;
  if (op == "leave")
  {
    m["type"] = picojson::value("mods-leave");
  }
  else if (op == "kick" || op == "slot")
  {
    const double slot = Num(args, "slot", 0);
    if (slot < 1 || slot > 4)
      return "slot must be 1 to 4";
    m["type"] = picojson::value(op == "kick" ? "mods-kick" : "mods-slot");
    m["slot"] = picojson::value(slot);
    if (op == "slot")
      m["open"] = picojson::value(Flag(args, "open", true));
  }
  else if (op == "public")
  {
    m["type"] = picojson::value("mods-public");
    m["public"] = picojson::value(Flag(args, "public", true));
  }
  else
  {
    return "op must be leave, kick, slot or public";
  }
  std::lock_guard lk(s.mutex);
  if (!s.view)
    return "not in a room";
  if (op == "leave")
    s.leaving = true;
  s.outbox.push_back(picojson::value(m).serialize());
  return std::nullopt;
}
}  // namespace Online::ModsRoom
