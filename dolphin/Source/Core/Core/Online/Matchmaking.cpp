// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Port of Slippi's SlippiMatchmaking.cpp (and the connect phase of SlippiNetplay.cpp). Comments
// marked "Slippi:" quote what the original does where this file has to differ or explain.

#include "Core/Online/Matchmaking.h"

#include <chrono>
#include <cstdlib>
#include <utility>

#include <enet/enet.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/Online/Timeouts.h"
#include "Core/Online/User.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace Online
{
namespace
{
constexpr char CREATE_TICKET[] = "create-ticket";
constexpr char CREATE_TICKET_RESP[] = "create-ticket-resp";
constexpr char GET_TICKET_RESP[] = "get-ticket-resp";

constexpr int MM_CHANNELS = 3;
// Not in Slippi (its netplay client keeps servicing the same ENet host after the connect window):
// a backend that binds its own socket releases the link right away, so give the peer's side of
// the handshake time to finish before our disconnect can overtake it.
constexpr u64 P2P_SETTLE_MS = 500;

std::string IpToString(enet_uint32 host)
{
  // ENetAddress::host is in network byte order: the bytes in memory are a.b.c.d.
  const auto* b = reinterpret_cast<const u8*>(&host);
  return fmt::format("{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
}

std::string GetStr(const picojson::object& o, const char* key, const std::string& def = "")
{
  const auto it = o.find(key);
  return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : def;
}

double GetNum(const picojson::object& o, const char* key, double def = 0)
{
  const auto it = o.find(key);
  return it != o.end() && it->second.is<double>() ? it->second.get<double>() : def;
}

bool GetBool(const picojson::object& o, const char* key, bool def = false)
{
  const auto it = o.find(key);
  return it != o.end() && it->second.is<bool>() ? it->second.get<bool>() : def;
}

// "a.b.c.d:port" -> Endpoint. False if it does not have that shape.
bool ParseEndpoint(const std::string& text, Endpoint* out)
{
  const auto parts = SplitString(text, ':');
  if (parts.size() != 2 || parts[0].empty() || parts[1].empty())
    return false;
  char* end = nullptr;
  const long port = std::strtol(parts[1].c_str(), &end, 10);
  if (*end != '\0' || port <= 0 || port > 0xFFFF)
    return false;
  out->ip = parts[0];
  out->port = static_cast<u16>(port);
  return true;
}

// Slippi: getLocalAddress. "Set up and connect a socket (UDP, so "connect" doesn't actually send
// any packets) so that the OS will determine what device/local IP address we will actually use."
enet_uint32 GetLocalAddress(const ENetAddress* mm_address)
{
  ENetSocket socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (socket == ENET_SOCKET_NULL)
  {
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Failed to get local address: socket create");
    return 0;
  }
  if (enet_socket_connect(socket, mm_address) == -1)
  {
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Failed to get local address: socket connect");
    enet_socket_destroy(socket);
    return 0;
  }
  ENetAddress address;
  if (enet_socket_get_address(socket, &address) == -1)
  {
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Failed to get local address: socket get address");
    enet_socket_destroy(socket);
    return 0;
  }
  enet_socket_destroy(socket);
  return address.host;
}

// Slippi: getLocalAddressFallback (the host name's address).
enet_uint32 GetLocalAddressFallback()
{
  char host[256]{};
  if (gethostname(host, sizeof(host) - 1) != 0)
  {
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Error finding LAN address");
    return 0;
  }
  ENetAddress address{};
  if (enet_address_set_host(&address, host) != 0)
  {
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Error finding LAN host");
    return 0;
  }
  return address.host;
}
}  // namespace

Matchmaking::Matchmaking(User* user, ConnectedCallback on_connected)
    : m_user(user), m_on_connected(std::move(on_connected)),
      m_generator(static_cast<unsigned>(Common::Timer::NowMs()))
{
}

Matchmaking::~Matchmaking()
{
  m_is_mm_terminated = true;
  m_state = State::ErrorEncountered;
  SetError(ErrorSource::Client, "Matchmaking shut down");

  if (m_matchmake_thread.joinable())
    m_matchmake_thread.join();

  TerminateMmConnection();
}

const char* Matchmaking::ModeName(Mode mode)
{
  switch (mode)
  {
  case Mode::Ranked:
    return "ranked";
  case Mode::Unranked:
    return "unranked";
  case Mode::Direct:
    return "direct";
  case Mode::Teams:
    return "teams";
  case Mode::Party:
    return "party";
  }
  return "?";
}

const char* Matchmaking::StateName(State state)
{
  switch (state)
  {
  case State::Idle:
    return "idle";
  case State::Initializing:
    return "initializing";
  case State::Matchmaking:
    return "matchmaking";
  case State::OpponentConnecting:
    return "opponent_connecting";
  case State::ConnectionSuccess:
    return "connection_success";
  case State::ErrorEncountered:
    return "error";
  }
  return "?";
}

const char* Matchmaking::ErrorSourceName(ErrorSource source)
{
  switch (source)
  {
  case ErrorSource::None:
    return "";
  case ErrorSource::Client:
    return "client";
  case ErrorSource::CreateTicket:
    return "create_ticket";
  case ErrorSource::GetTicket:
    return "get_ticket";
  case ErrorSource::Connection:
    return "connection";
  }
  return "?";
}

bool Matchmaking::IsFixedRulesMode(Mode mode)
{
  return mode == Mode::Unranked || mode == Mode::Ranked || mode == Mode::Party;
}

std::vector<u8> Matchmaking::EncodeConnectCode(const std::string& code)
{
  // JIS X 0208 row 3 (0x824F-0x829A) holds the full-width digits and Latin letters; '#' is
  // 0x8194. Anything else is sent as is.
  std::vector<u8> out;
  for (const char ch : code)
  {
    const u8 c = static_cast<u8>(ch);
    if (c >= '0' && c <= '9')
      out.insert(out.end(), {0x82, static_cast<u8>(0x4F + (c - '0'))});
    else if (c >= 'A' && c <= 'Z')
      out.insert(out.end(), {0x82, static_cast<u8>(0x60 + (c - 'A'))});
    else if (c >= 'a' && c <= 'z')
      out.insert(out.end(), {0x82, static_cast<u8>(0x81 + (c - 'a'))});
    else if (c == '#')
      out.insert(out.end(), {0x81, 0x94});
    else
      out.push_back(c);
  }
  return out;
}

void Matchmaking::FindMatch(const SearchSettings& settings)
{
  if (IsSearching())
    return;
  if (m_matchmake_thread.joinable())
    m_matchmake_thread.join();

  m_is_mm_connected = false;
  m_is_mm_terminated = false;

  INFO_LOG_FMT(NETPLAY, "[Matchmaking] Starting matchmaking...");

  m_search_settings = settings;
  {
    std::lock_guard lk(m_mutex);
    m_error_msg.clear();
    m_match.reset();
    m_last_ticket_response = picojson::value();
  }
  m_error_source = ErrorSource::None;
  m_ticket_count = 0;
  m_connect_attempts = 0;
  m_state = State::Initializing;
  m_matchmake_thread = std::thread(&Matchmaking::MatchmakeThread, this);
}

bool Matchmaking::IsSearching() const
{
  const State s = m_state.load();
  return s == State::Initializing || s == State::Matchmaking || s == State::OpponentConnecting;
}

std::string Matchmaking::GetErrorMessage() const
{
  std::lock_guard lk(m_mutex);
  return m_error_msg;
}

std::optional<Match> Matchmaking::GetMatch() const
{
  std::lock_guard lk(m_mutex);
  return m_match;
}

int Matchmaking::LocalPlayerIndex() const
{
  std::lock_guard lk(m_mutex);
  return m_match ? m_match->local_player_index : 0;
}

u8 Matchmaking::RemotePlayerCount() const
{
  std::lock_guard lk(m_mutex);
  if (!m_match || m_match->players.empty())
    return 0;
  return static_cast<u8>(m_match->players.size() - 1);
}

P2PLink Matchmaking::TakeLink()
{
  std::lock_guard lk(m_mutex);
  return std::move(m_link);
}

std::string Matchmaking::GetLanAddress() const
{
  std::lock_guard lk(m_mutex);
  return m_lan_address;
}

std::string Matchmaking::GetServerAddress() const
{
  std::lock_guard lk(m_mutex);
  return m_server_address;
}

picojson::value Matchmaking::GetLastTicketResponse() const
{
  std::lock_guard lk(m_mutex);
  return m_last_ticket_response;
}

void Matchmaking::SetError(ErrorSource source, std::string message)
{
  {
    std::lock_guard lk(m_mutex);
    m_error_msg = std::move(message);
  }
  m_error_source = source;
}

void Matchmaking::SendMmMessage(const picojson::value& msg)
{
  const std::string contents = msg.serialize();
  ENetPacket* packet =
      enet_packet_create(contents.c_str(), contents.length(), ENET_PACKET_FLAG_RELIABLE);
  enet_peer_send(m_server, 0, packet);
}

int Matchmaking::ReceiveMmMessage(picojson::value& msg, int timeout_ms)
{
  constexpr int host_service_timeout_ms = 250;

  // Make sure loop runs at least once
  if (timeout_ms < host_service_timeout_ms)
    timeout_ms = host_service_timeout_ms;

  // Slippi: "This is not a perfect way to timeout but hopefully it's close enough?"
  const int max_attempts = timeout_ms / host_service_timeout_ms;

  for (int i = 0; i < max_attempts; i++)
  {
    if (m_is_mm_terminated)
      return -1;

    ENetEvent net_event;
    const int net = enet_host_service(m_client, &net_event, host_service_timeout_ms);
    if (net <= 0)
      continue;

    switch (net_event.type)
    {
    case ENET_EVENT_TYPE_RECEIVE:
    {
      const std::string str(reinterpret_cast<const char*>(net_event.packet->data),
                            net_event.packet->dataLength);
      enet_packet_destroy(net_event.packet);
      msg = picojson::value();
      if (!picojson::parse(msg, str).empty())
        msg = picojson::value();
      return 0;
    }
    case ENET_EVENT_TYPE_DISCONNECT:
      // Return -2 code to indicate we have lost connection to the server
      return -2;
    default:
      break;
    }
  }

  return -1;
}

void Matchmaking::MatchmakeThread()
{
  Common::SetCurrentThreadName("Online matchmaking");
  while (IsSearching())
  {
    if (m_is_mm_terminated)
      break;

    switch (m_state.load())
    {
    case State::Initializing:
      StartMatchmaking();
      break;
    case State::Matchmaking:
      HandleMatchmaking();
      break;
    case State::OpponentConnecting:
      HandleConnecting();
      break;
    default:
      break;
    }
  }

  // Clean up ENET connections
  TerminateMmConnection();
}

void Matchmaking::DisconnectFromServer()
{
  m_is_mm_connected = false;

  if (!m_server)
    return;
  enet_peer_disconnect(m_server, 0);

  ENetEvent net_event;
  while (enet_host_service(m_client, &net_event, 3000) > 0)
  {
    switch (net_event.type)
    {
    case ENET_EVENT_TYPE_RECEIVE:
      enet_packet_destroy(net_event.packet);
      break;
    case ENET_EVENT_TYPE_DISCONNECT:
      m_server = nullptr;
      return;
    default:
      break;
    }
  }

  // didn't disconnect gracefully force disconnect
  enet_peer_reset(m_server);
  m_server = nullptr;
}

void Matchmaking::TerminateMmConnection()
{
  // Disconnect from server
  DisconnectFromServer();

  // Destroy client
  if (m_client)
  {
    enet_host_destroy(m_client);
    m_client = nullptr;
  }
}

void Matchmaking::StartMatchmaking()
{
  m_client = nullptr;
  int retry_count = 0;

  // Slippi: fixed-rules modes first require the ISO MD5 check ("Cannot queue for this mode with a
  // modded ISO known to desync"). Our build/codeset hash check is a later phase (backend-design 6);
  // the server refuses those modes for now anyway.

  const UserInfo user_info = m_user->GetUserInfo();
  while (m_client == nullptr && retry_count < 15)
  {
    if (Config::Get(Config::ONLINE_FORCE_NETPLAY_PORT))
      m_host_port = static_cast<u16>(Config::Get(Config::ONLINE_NETPLAY_PORT));
    else
      m_host_port = static_cast<u16>(41000 + (m_generator() % 10000));
    INFO_LOG_FMT(NETPLAY, "[Matchmaking] Port to use: {}...", m_host_port.load());

    // Slippi: "We are explicitly setting the client address because we are trying to utilize our
    // connection to the matchmaking service in order to hole punch. This port will end up being
    // the port we listen on when we start our server"
    ENetAddress client_addr;
    client_addr.host = ENET_HOST_ANY;
    client_addr.port = m_host_port;

    m_client = enet_host_create(&client_addr, 1, MM_CHANNELS, 0, 0);
    retry_count++;
  }

  if (m_client == nullptr)
  {
    m_state = State::ErrorEncountered;
    SetError(ErrorSource::Client, "Failed to create mm client");
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Failed to create client...");
    return;
  }

  const std::string mm_host = Config::GetMatchmakingHost();
  const int mm_port = Config::GetMatchmakingPort();
  {
    std::lock_guard lk(m_mutex);
    m_server_address = fmt::format("{}:{}", mm_host, mm_port);
  }

  ENetAddress addr{};
  if (enet_address_set_host(&addr, mm_host.c_str()) != 0)
  {
    // Slippi does not check this; enet_host_connect to 0.0.0.0 then fails the 10 s connect.
    m_state = State::ErrorEncountered;
    SetError(ErrorSource::Client, "Failed to connect to mm server");
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Cannot resolve mm server {}", mm_host);
    return;
  }
  addr.port = static_cast<enet_uint16>(mm_port);

  m_server = enet_host_connect(m_client, &addr, MM_CHANNELS, 0);
  if (m_server == nullptr)
  {
    m_state = State::ErrorEncountered;
    SetError(ErrorSource::Client, "Failed to start connection to mm server");
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Failed to start connection to mm server...");
    return;
  }

  // Before we can request a ticket, we must wait for connection to be successful
  int connect_attempt_count = 0;
  while (!m_is_mm_connected)
  {
    if (m_is_mm_terminated)
      return;

    ENetEvent net_event;
    const int net = enet_host_service(m_client, &net_event, 500);
    if (net <= 0 || net_event.type != ENET_EVENT_TYPE_CONNECT)
    {
      if (net > 0 && net_event.type == ENET_EVENT_TYPE_RECEIVE)
        enet_packet_destroy(net_event.packet);

      // Not yet connected, will retry
      connect_attempt_count++;
      if (connect_attempt_count >= 20)
      {
        ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Failed to connect to mm server...");
        m_state = State::ErrorEncountered;
        SetError(ErrorSource::Client, "Failed to connect to mm server");
        return;
      }
      continue;
    }

    m_is_mm_connected = true;
    INFO_LOG_FMT(NETPLAY, "[Matchmaking] Connected to mm server...");
  }

  INFO_LOG_FMT(NETPLAY, "[Matchmaking] Trying to find match...");

  // Slippi: "Determine local IP address. We can attempt to connect to our opponent via local IP
  // address if we have the same external IP address. The following scenarios can cause us to have
  // the same external IP address:
  // - we are connected to the same LAN
  // - we are connected to the same VPN node
  // - we are behind the same CGNAT"
  std::string lan_addr;
  if (Config::Get(Config::ONLINE_FORCE_LAN_IP))
  {
    WARN_LOG_FMT(NETPLAY, "[Matchmaking] Overwriting LAN IP sent with configured address");
    lan_addr = fmt::format("{}:{}", Config::Get(Config::ONLINE_LAN_IP), m_host_port.load());
  }
  else
  {
    enet_uint32 local_address = GetLocalAddress(&addr);
    if (local_address == 0)
      local_address = GetLocalAddressFallback();
    if (local_address != 0)
      lan_addr = fmt::format("{}:{}", IpToString(local_address), m_host_port.load());
  }
  {
    std::lock_guard lk(m_mutex);
    m_lan_address = lan_addr;
  }
  INFO_LOG_FMT(NETPLAY, "[Matchmaking] Sending LAN address: {}", lan_addr);

  picojson::array connect_code_buf;
  for (const u8 b : EncodeConnectCode(m_search_settings.connect_code))
    connect_code_buf.emplace_back(static_cast<double>(b));

  // Send message to server to create ticket. Same fields as Slippi's create-ticket.
  picojson::object user;
  user["uid"] = picojson::value(user_info.uid);
  user["playKey"] = picojson::value(user_info.play_key);
  user["connectCode"] = picojson::value(user_info.connect_code);
  user["displayName"] = picojson::value(user_info.display_name);
  picojson::object search;
  search["mode"] = picojson::value(static_cast<double>(m_search_settings.mode));
  search["connectCode"] = picojson::value(std::move(connect_code_buf));
  picojson::object request;
  request["type"] = picojson::value(CREATE_TICKET);
  request["user"] = picojson::value(std::move(user));
  request["search"] = picojson::value(std::move(search));
  request["appVersion"] = picojson::value(APP_VERSION);
  request["ipAddressLan"] = picojson::value(lan_addr);
  SendMmMessage(picojson::value(std::move(request)));
  m_ticket_count++;

  // Get response from server
  picojson::value response;
  const int rcv_res = ReceiveMmMessage(response, 5000);
  if (m_is_mm_terminated)
    return;
  if (rcv_res != 0)
  {
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Did not receive response from server for create ticket");
    m_state = State::ErrorEncountered;
    SetError(ErrorSource::Client, "Failed to join mm queue");
    return;
  }

  const picojson::object empty;
  const auto& resp = response.is<picojson::object>() ? response.get<picojson::object>() : empty;
  if (GetStr(resp, "type") != CREATE_TICKET_RESP)
  {
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Received incorrect response for create ticket");
    ERROR_LOG_FMT(NETPLAY, "{}", response.serialize());
    m_state = State::ErrorEncountered;
    SetError(ErrorSource::Client, "Invalid response when joining mm queue");
    return;
  }

  const std::string err = GetStr(resp, "error");
  if (!err.empty())
  {
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Received error from server for create ticket: {}", err);
    m_state = State::ErrorEncountered;
    SetError(ErrorSource::CreateTicket, err);
    return;
  }

  m_state = State::Matchmaking;
  INFO_LOG_FMT(NETPLAY, "[Matchmaking] Request ticket success");
}

void Matchmaking::HandleMatchmaking()
{
  // Deal with class shut down
  if (m_state != State::Matchmaking)
    return;

  // Get response from server
  picojson::value get_resp_value;
  const int rcv_res = ReceiveMmMessage(get_resp_value, 2000);
  if (m_is_mm_terminated || m_state != State::Matchmaking)
    return;
  if (rcv_res == -1)
  {
    DEBUG_LOG_FMT(NETPLAY, "[Matchmaking] Have not yet received assignment");
    return;
  }
  if (rcv_res != 0)
  {
    // Right now the only other code is -2 meaning the server died probably?
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Lost connection to the mm server");
    m_state = State::ErrorEncountered;
    SetError(ErrorSource::Connection, "Lost connection to the mm server");
    return;
  }

  const picojson::object empty;
  const auto& get_resp =
      get_resp_value.is<picojson::object>() ? get_resp_value.get<picojson::object>() : empty;
  {
    std::lock_guard lk(m_mutex);
    m_last_ticket_response = get_resp_value;
  }
  if (GetStr(get_resp, "type") != GET_TICKET_RESP)
  {
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Received incorrect response for get ticket");
    m_state = State::ErrorEncountered;
    SetError(ErrorSource::Client, "Invalid response when getting mm status");
    return;
  }

  const std::string err = GetStr(get_resp, "error");
  const std::string latest_version = GetStr(get_resp, "latestVersion");
  if (!err.empty())
  {
    if (!latest_version.empty())
    {
      // Slippi: "Update version number when the mm server tells us our version is outdated for
      // people whose file updates dont work"
      m_user->OverwriteLatestVersion(latest_version);
    }

    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Received error from server for get ticket: {}", err);
    m_state = State::ErrorEncountered;
    SetError(ErrorSource::GetTicket, err);
    return;
  }

  Match match;
  match.match_id = GetStr(get_resp, "matchId");
  match.local_port = m_host_port;
  INFO_LOG_FMT(NETPLAY, "[Matchmaking] Match ID: {}", match.match_id);

  std::vector<std::string> remote_ips;
  const auto players_it = get_resp.find("players");
  if (players_it != get_resp.end() && players_it->second.is<picojson::array>())
  {
    const auto& queue = players_it->second.get<picojson::array>();
    std::string local_external_ip;

    for (const auto& v : queue)
    {
      const auto& el = v.is<picojson::object>() ? v.get<picojson::object>() : empty;
      PlayerInfo player;
      player.is_local = GetBool(el, "isLocalPlayer");
      player.uid = GetStr(el, "uid");
      player.display_name = GetStr(el, "displayName");
      player.connect_code = GetStr(el, "connectCode");
      player.port = static_cast<int>(GetNum(el, "port"));
      player.is_bot = GetBool(el, "isBot");
      player.ip_address = GetStr(el, "ipAddress", "1.1.1.1:123");
      player.ip_address_lan = GetStr(el, "ipAddressLan", "1.1.1.1:123");
      player.chat_messages = User::GetDefaultChatMessages();
      const auto chat = el.find("chatMessages");
      if (chat != el.end() && chat->second.is<picojson::array>() &&
          chat->second.get<picojson::array>().size() == 16)
      {
        player.chat_messages.clear();
        for (const auto& m : chat->second.get<picojson::array>())
          player.chat_messages.push_back(m.is<std::string>() ? m.get<std::string>() : "");
      }
      match.players.push_back(player);

      if (player.is_local)
      {
        local_external_ip = SplitString(player.ip_address, ':')[0];
        match.local_player_index = player.port - 1;
      }
    }

    // Loop a second time to get the correct remote IPs
    for (const auto& player : match.players)
    {
      if (player.port - 1 == match.local_player_index)
        continue;

      const std::string& ext_ip = player.ip_address;
      const std::string& lan_ip = player.ip_address_lan;
      INFO_LOG_FMT(NETPLAY, "[Matchmaking] LAN IP: {}", lan_ip);

      if (SplitString(ext_ip, ':')[0] != local_external_ip || lan_ip.empty())
      {
        // If external IPs are different, just use that address
        remote_ips.push_back(ext_ip);
        continue;
      }

      // Slippi: "TODO: Instead of using one or the other, it might be better to try both"
      // If external IPs are the same, try using LAN IPs
      remote_ips.push_back(lan_ip);
    }
  }
  match.is_host = GetBool(get_resp, "isHost");

  // Allowed stages (srStageKind ids): the server's list for the mode (server/config/
  // rulesets.json). Slippi falls back to a default Melee list when `stages` is missing; ours is
  // in the session (Gprb::Session::DefaultStages(), P+'s legal list), which draws the random
  // stages from this list (Unranked's games, Direct's game 1). Stage picks are not restricted.
  const auto stages_it = get_resp.find("stages");
  if (stages_it != get_resp.end() && stages_it->second.is<picojson::array>())
  {
    for (const auto& s : stages_it->second.get<picojson::array>())
    {
      if (s.is<double>())
        match.stages.push_back(static_cast<u16>(s.get<double>()));
    }
  }
  match.items = static_cast<u32>(GetNum(get_resp, "items"));
  INFO_LOG_FMT(NETPLAY, "[Matchmaking] Stages from the server: {}", fmt::join(match.stages, ","));

  for (const auto& ip : remote_ips)
  {
    Endpoint ep;
    if (!ParseEndpoint(ip, &ep))
    {
      // Slippi would throw from std::stoi here; refuse the match instead.
      ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Unusable peer address '{}'", ip);
      m_state = State::ErrorEncountered;
      SetError(ErrorSource::Client, "Invalid response when getting mm status");
      return;
    }
    match.remotes.push_back(ep);
  }

  {
    std::lock_guard lk(m_mutex);
    m_match = match;
  }

  // Disconnect and destroy enet client to mm server
  TerminateMmConnection();

  // Slippi reports "connecting" to its backend for ranked here (reportOnlineMatchStatus). Ranked
  // reporting is a later phase.

  m_state = State::OpponentConnecting;
  INFO_LOG_FMT(NETPLAY, "[Matchmaking] Opponent found. is_decider: {}", match.is_host);
}

void Matchmaking::HandleConnecting()
{
  std::optional<Match> match_opt = GetMatch();
  if (!match_opt)
  {
    m_state = State::Initializing;
    return;
  }
  Match match = *match_opt;
  m_connect_attempts++;

  const size_t remote_player_count = match.remotes.size();
  std::string ip_log;
  for (const auto& r : match.remotes)
    ip_log += r.ToString() + ", ";
  INFO_LOG_FMT(NETPLAY, "[Matchmaking] My port: {} || Remote player IPs: {}", m_host_port.load(),
               ip_log);

  // --- SlippiNetplayClient constructor ---
  // Slippi: "It is important to be able to set the local port to listen on even in a client
  // connection because not doing so will break hole punching, the host is expecting traffic to
  // come from a specific ip/port and if the port does not match what it is expecting, it will not
  // get through the NAT on some routers"
  ENetAddress local_addr;
  local_addr.host = ENET_HOST_ANY;
  local_addr.port = m_host_port;
  P2PLink::HostPtr client{enet_host_create(&local_addr, 10, MM_CHANNELS, 0, 0)};
  if (!client)
  {
    // Slippi: PanicAlert("Couldn't Create Client").
    ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Couldn't create the P2P client on port {}",
                  m_host_port.load());
    m_state = State::ErrorEncountered;
    SetError(ErrorSource::Client, "Couldn't Create Client");
    return;
  }

  std::vector<ENetPeer*> servers;
  std::vector<ENetAddress> remote_addrs;
  for (const auto& r : match.remotes)
  {
    ENetAddress addr{};
    enet_address_set_host(&addr, r.ip.c_str());
    addr.port = r.port;
    ENetPeer* peer = enet_host_connect(client.get(), &addr, MM_CHANNELS, 0);
    if (!peer)
    {
      ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Couldn't create peer for {}", r.ToString());
      m_state = State::ErrorEncountered;
      SetError(ErrorSource::Client, "Couldn't create peer.");
      return;
    }
    servers.push_back(peer);
    remote_addrs.push_back(addr);
  }

  // --- SlippiNetplayClient::ThreadFunc, connect phase ---
  const u64 start_time = Common::Timer::NowMs();
  std::vector<bool> connections(remote_player_count, false);
  bool connected = false;
  while (!connected)
  {
    // Deal with class shut down
    if (m_state != State::OpponentConnecting || m_is_mm_terminated)
      return;

    // This will confirm that connection went through successfully
    ENetEvent net_event;
    const int net = enet_host_service(client.get(), &net_event, 500);
    if (net > 0)
    {
      switch (net_event.type)
      {
      case ENET_EVENT_TYPE_RECEIVE:
        enet_packet_destroy(net_event.packet);
        break;

      case ENET_EVENT_TYPE_DISCONNECT:
        INFO_LOG_FMT(NETPLAY, "[Netplay] got disconnect event with peer addr {}:{}",
                     IpToString(net_event.peer->address.host), net_event.peer->address.port);
        break;

      case ENET_EVENT_TYPE_CONNECT:
      {
        INFO_LOG_FMT(NETPLAY, "[Netplay] got connect event with peer addr {}:{}",
                     IpToString(net_event.peer->address.host), net_event.peer->address.port);
        bool is_already_connected = false;
        for (size_t i = 0; i < servers.size(); i++)
        {
          if (connections[i] && net_event.peer->address.host == servers[i]->address.host &&
              net_event.peer->address.port == servers[i]->address.port)
          {
            servers[i] = net_event.peer;
            is_already_connected = true;
            break;
          }
        }

        if (is_already_connected)
        {
          // Slippi: "Don't add this person again if they are already connected."
          INFO_LOG_FMT(NETPLAY, "[Netplay] Already connected!");
          break;
        }

        for (size_t i = 0; i < servers.size(); i++)
        {
          // Slippi matches the host only, not the port: "for some people, their internet will
          // switch the port they're sending from".
          if (remote_addrs[i].host == net_event.peer->address.host && !connections[i])
          {
            INFO_LOG_FMT(NETPLAY, "[Netplay] Overwriting ENetPeer for address: {}:{}",
                         IpToString(net_event.peer->address.host), net_event.peer->address.port);
            servers[i] = net_event.peer;
            connections[i] = true;
            break;
          }
        }
        break;
      }
      default:
        break;
      }
    }

    bool all_connected = true;
    for (size_t i = 0; i < remote_player_count; i++)
    {
      if (!connections[i])
        all_connected = false;
    }

    if (all_connected)
    {
      INFO_LOG_FMT(NETPLAY, "Online connection successful!");
      connected = true;
      break;
    }

    // Time out after enough time has passed
    if (Common::Timer::NowMs() - start_time >= P2P_CONNECT_TIMEOUT_MS)
    {
      std::vector<int> failed_connections;
      for (size_t i = 0; i < remote_player_count; i++)
      {
        if (!connections[i])
          failed_connections.push_back(static_cast<int>(i));
      }
      INFO_LOG_FMT(NETPLAY, "Online connection failed");

      if (m_search_settings.mode == Mode::Teams)
      {
        // Slippi: "If we failed setting up a connection in teams mode, show a detailed error about
        // who we had issues connecting to."
        ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Failed to connect to players");
        std::string err = "Timed out waiting for other players to connect";
        if (!failed_connections.empty())
        {
          err = "Could not connect to players: ";
          for (size_t i = 0; i < failed_connections.size(); i++)
          {
            int p = failed_connections[i];
            if (p >= match.local_player_index)
              p++;
            if (p < static_cast<int>(match.players.size()))
              err += match.players[p].display_name;
            if (i < failed_connections.size() - 1)
              err += ", ";
          }
        }
        m_state = State::ErrorEncountered;
        SetError(ErrorSource::Connection, err);
        return;
      }

      ERROR_LOG_FMT(NETPLAY, "[Matchmaking] Connection attempt failed, looking for someone else.");
      // Return to the start to get a new ticket to find someone else we can hopefully connect with
      m_state = State::Initializing;
      return;
    }
  }

  match.connect_ms = Common::Timer::NowMs() - start_time;

  // Let the peer's side of the handshake finish (see P2P_SETTLE_MS).
  const u64 settle_start = Common::Timer::NowMs();
  while (Common::Timer::NowMs() - settle_start < P2P_SETTLE_MS)
  {
    if (m_state != State::OpponentConnecting || m_is_mm_terminated)
      return;
    ENetEvent net_event;
    if (enet_host_service(client.get(), &net_event, 20) > 0 &&
        net_event.type == ENET_EVENT_TYPE_RECEIVE)
    {
      enet_packet_destroy(net_event.packet);
    }
  }

  for (const ENetPeer* peer : servers)
    match.connected.push_back({IpToString(peer->address.host), peer->address.port});
  {
    std::lock_guard lk(m_mutex);
    m_match = match;
  }

  INFO_LOG_FMT(NETPLAY, "[Matchmaking] Connection success! ({} ms)", match.connect_ms);
  P2PLink link(std::move(client), servers);

  // Connection success, our work is done
  m_state = State::ConnectionSuccess;
  if (m_on_connected)
    m_on_connected(match, link);
  if (link.IsOpen())
  {
    std::lock_guard lk(m_mutex);
    m_link = std::move(link);
  }
}
}  // namespace Online
