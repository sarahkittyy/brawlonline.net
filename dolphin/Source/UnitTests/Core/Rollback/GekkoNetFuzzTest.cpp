// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// GekkoNet against malformed and hostile packets. Two game sessions (A and B, two players, the
// gameplay session's input size) talk through in-memory queues; packets are then injected into
// A as if B (the other player's machine, which may be malicious) had sent them.

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "compression.h"
#include "gekkonet.h"
#include "net.h"
#include "zpp/zpp_bits.h"

namespace
{
constexpr unsigned INPUT_SIZE = 0x40;
constexpr char ADDR[] = "peer";

struct Side
{
  std::deque<std::vector<unsigned char>> inbox;
  std::vector<GekkoNetResult*> results;
  std::vector<std::vector<unsigned char>> sent;  // everything this side sent
  std::deque<std::pair<int, std::vector<unsigned char>>> in_flight;  // (arrival step, packet)
};
Side g_a, g_b;
// Every this many packets A sends is lost on the way (0: none).
int g_drop_a_every = 0;
int g_a_sent_count = 0;
// Packets arrive this many steps (frames) after they were sent (0: at once), and the step.
int g_delay_steps = 0;
int g_step = 0;

void Deliver(Side& to, Side& from, const char* data, int length)
{
  std::vector<unsigned char> v(data, data + length);
  from.sent.push_back(v);
  if (&from == &g_a && g_drop_a_every && ++g_a_sent_count % g_drop_a_every == 0)
    return;
  if (g_delay_steps)
    to.in_flight.emplace_back(g_step + g_delay_steps, std::move(v));
  else
    to.inbox.push_back(std::move(v));
}
void SendFromA(GekkoNetAddress*, const char* d, int n)
{
  Deliver(g_b, g_a, d, n);
}
void SendFromB(GekkoNetAddress*, const char* d, int n)
{
  Deliver(g_a, g_b, d, n);
}

GekkoNetResult** Receive(Side& side, int* length)
{
  side.results.clear();
  for (const auto& pkt : side.inbox)
  {
    auto* r = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    r->data_len = static_cast<unsigned>(pkt.size());
    r->data = std::malloc(std::max<size_t>(pkt.size(), 1));
    if (!pkt.empty())
      std::memcpy(r->data, pkt.data(), pkt.size());
    r->addr.data = std::malloc(sizeof(ADDR) - 1);
    std::memcpy(r->addr.data, ADDR, sizeof(ADDR) - 1);
    r->addr.size = sizeof(ADDR) - 1;
    side.results.push_back(r);
  }
  side.inbox.clear();
  *length = static_cast<int>(side.results.size());
  return side.results.data();
}
GekkoNetResult** ReceiveA(int* n)
{
  return Receive(g_a, n);
}
GekkoNetResult** ReceiveB(int* n)
{
  return Receive(g_b, n);
}
void FreeData(void* p)
{
  std::free(p);
}

std::optional<Gekko::NetPacket> Decode(const std::vector<unsigned char>& bytes)
{
  Gekko::NetPacket pkt;
  zpp::bits::in in(bytes);
  if (zpp::bits::failure(in(pkt.header, pkt.body)))
    return std::nullopt;
  return pkt;
}

std::vector<unsigned char> Encode(const Gekko::NetPacket& pkt)
{
  std::vector<unsigned char> out;
  zpp::bits::out o(out);
  (void)o(pkt.header, pkt.body);
  return out;
}

struct Pair
{
  GekkoSession* a = nullptr;
  GekkoSession* b = nullptr;
  GekkoNetAdapter adapter_a{SendFromA, ReceiveA, FreeData};
  GekkoNetAdapter adapter_b{SendFromB, ReceiveB, FreeData};
  int advances_a = 0;
  int advances_b = 0;
  // the newest inputs A's session handed its game for player 0 (A's own player)
  std::vector<unsigned char> a_local_inputs_seen;
  // the first byte of player 0's input in each frame's newest advance, on each side
  std::map<int, unsigned char> a_frames, b_frames;

  Pair()
  {
    g_a = {};
    g_b = {};
    g_drop_a_every = 0;
    g_a_sent_count = 0;
    g_delay_steps = 0;
    g_step = 0;
    GekkoConfig cfg{};
    cfg.num_players = 2;
    cfg.input_prediction_window = 7;
    cfg.input_size = INPUT_SIZE;
    cfg.state_size = sizeof(unsigned);
    cfg.desync_detection = true;
    char addr_bytes[] = "peer";
    GekkoNetAddress addr{addr_bytes, sizeof(addr_bytes) - 1};

    gekko_create(&a, GekkoGameSession);
    gekko_start(a, &cfg);
    gekko_net_adapter_set(a, &adapter_a);
    gekko_add_actor(a, GekkoLocalPlayer, nullptr);
    gekko_add_actor(a, GekkoRemotePlayer, &addr);
    gekko_set_local_delay(a, 0, 2);

    gekko_create(&b, GekkoGameSession);
    gekko_start(b, &cfg);
    gekko_net_adapter_set(b, &adapter_b);
    gekko_add_actor(b, GekkoRemotePlayer, &addr);
    gekko_add_actor(b, GekkoLocalPlayer, nullptr);
    gekko_set_local_delay(b, 1, 2);
  }
  ~Pair()
  {
    gekko_destroy(&a);
    gekko_destroy(&b);
  }

  static int Update(GekkoSession* s, int player, unsigned char fill,
                    std::vector<unsigned char>* seen_player0, std::map<int, unsigned char>* frames)
  {
    std::array<unsigned char, INPUT_SIZE> input;
    input.fill(fill);
    gekko_add_local_input(s, player, input.data());
    int count = 0;
    GekkoGameEvent** events = gekko_update_session(s, &count);
    int advances = 0;
    for (int i = 0; i < count; ++i)
    {
      GekkoGameEvent* e = events[i];
      if (e->type == GekkoSaveEvent)
      {
        if (e->data.save.state_len)
          *e->data.save.state_len = sizeof(unsigned);
        if (e->data.save.checksum)
          *e->data.save.checksum = 0;
      }
      else if (e->type == GekkoAdvanceEvent)
      {
        ++advances;
        if (seen_player0 && e->data.adv.inputs)
          seen_player0->assign(e->data.adv.inputs, e->data.adv.inputs + INPUT_SIZE);
        if (e->data.adv.inputs)
          (*frames)[e->data.adv.frame] = e->data.adv.inputs[0];
      }
    }
    int scount = 0;
    (void)gekko_session_events(s, &scount);
    return advances;
  }

  void Step(unsigned char a_fill = 0x11, unsigned char b_fill = 0x22)
  {
    ++g_step;
    for (Side* side : {&g_a, &g_b})
    {
      while (!side->in_flight.empty() && side->in_flight.front().first <= g_step)
      {
        side->inbox.push_back(std::move(side->in_flight.front().second));
        side->in_flight.pop_front();
      }
    }
    gekko_network_poll(a);
    gekko_network_poll(b);
    advances_a += Update(a, 0, a_fill, &a_local_inputs_seen, &a_frames);
    advances_b += Update(b, 1, b_fill, nullptr, &b_frames);
  }

  // Runs until both sides have simulated `frames` frames (the handshake first).
  bool RunUntil(int frames, int max_steps = 5000)
  {
    for (int i = 0; i < max_steps; ++i)
    {
      Step();
      if (advances_a >= frames && advances_b >= frames)
        return true;
    }
    return false;
  }

  // A packet B really sent A, of `type`, newest first.
  std::optional<Gekko::NetPacket> CapturedFromB(Gekko::PacketType type) const
  {
    for (auto it = g_b.sent.rbegin(); it != g_b.sent.rend(); ++it)
    {
      auto p = Decode(*it);
      if (p && p->header.type == type)
        return p;
    }
    return std::nullopt;
  }
};

// The local player's inputs A sent B, decoded from A's Inputs packets after `from` (index into
// g_a.sent).
std::vector<std::vector<unsigned char>> InputsASent(size_t from)
{
  std::vector<std::vector<unsigned char>> out;
  for (size_t i = from; i < g_a.sent.size(); ++i)
  {
    auto p = Decode(g_a.sent[i]);
    if (!p || p->header.type != Gekko::Inputs)
      continue;
    auto* msg = std::get_if<Gekko::InputMsg>(&p->body);
    if (!msg)
      continue;
    std::vector<unsigned char> bytes = msg->inputs;
    if (msg->compressed)
    {
      auto rle = Gekko::Compression::RLEDecode(bytes.data(), static_cast<unsigned>(bytes.size()));
      bytes = Gekko::Compression::DeltaDecode(rle.data(), static_cast<unsigned>(rle.size()),
                                              INPUT_SIZE);
    }
    out.push_back(std::move(bytes));
  }
  return out;
}

// Replaces the first occurrence of the little-endian u32 `from` followed by `marker` with `to`.
bool PatchLength(std::vector<unsigned char>& bytes, unsigned from, unsigned to,
                 const std::vector<unsigned char>& marker)
{
  for (size_t i = 0; i + 4 + marker.size() <= bytes.size(); ++i)
  {
    unsigned v;
    std::memcpy(&v, &bytes[i], 4);
    if (v == from && std::equal(marker.begin(), marker.end(), bytes.begin() + i + 4))
    {
      std::memcpy(&bytes[i], &to, 4);
      return true;
    }
  }
  return false;
}
}  // namespace

TEST(GekkoNetFuzz, SessionsConnectAndRun)
{
  Pair pair;
  ASSERT_TRUE(pair.RunUntil(120));
  ASSERT_EQ(pair.a_local_inputs_seen.size(), INPUT_SIZE);
  EXPECT_EQ(pair.a_local_inputs_seen[0], 0x11);
}

// A vector whose size field says 4 GiB: zpp resized to it before looking at the bytes behind it
// (std::bad_alloc, i.e. a crash, or seconds of zero-filling per packet).
TEST(GekkoNetFuzz, HugeVectorSizesAreDropped)
{
  Pair pair;
  ASSERT_TRUE(pair.RunUntil(60));
  auto inputs = pair.CapturedFromB(Gekko::Inputs);
  ASSERT_TRUE(inputs.has_value());

  Gekko::NetPacket pkt = *inputs;
  Gekko::InputMsg msg;
  msg.start_frame = 1;
  msg.input_count = 1;
  msg.compressed = false;
  msg.inputs = {0xDE, 0xAD, 0xBE, 0xEF};
  msg.total_size = 4;
  pkt.body = msg;
  auto bytes = Encode(pkt);
  ASSERT_TRUE(PatchLength(bytes, 4, 0xFFFFFFFFu, {0xDE, 0xAD, 0xBE, 0xEF}));

  Gekko::NetPacket claim_pkt = *inputs;
  claim_pkt.header.type = Gekko::DisconnectClaim;
  Gekko::DisconnectClaimMsg claim;
  claim.player = 0;
  claim.start_frame = 0;
  claim.last_frame = 0;
  claim.inputs = {0xCA, 0xFE, 0xBA, 0xBE};
  claim_pkt.body = claim;
  auto claim_bytes = Encode(claim_pkt);
  ASSERT_TRUE(PatchLength(claim_bytes, 4, 0xFFFFFFF0u, {0xCA, 0xFE, 0xBA, 0xBE}));

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 64; ++i)
  {
    g_a.inbox.push_back(bytes);
    g_a.inbox.push_back(claim_bytes);
    pair.Step();
  }
  const auto took = std::chrono::steady_clock::now() - start;
  EXPECT_LT(took, std::chrono::seconds(5));
  // A still plays.
  const int before = pair.advances_a;
  for (int i = 0; i < 30; ++i)
    pair.Step();
  EXPECT_GT(pair.advances_a, before);
}

// SpectatorInputs carry every player's input. A player's session queued them for its own local
// player too, and then sent the peer's bytes back as its own input.
TEST(GekkoNetFuzz, SpectatorInputsDoNotReachLocalPlayers)
{
  Pair pair;
  ASSERT_TRUE(pair.RunUntil(60));
  auto inputs = pair.CapturedFromB(Gekko::Inputs);
  ASSERT_TRUE(inputs.has_value());

  const size_t sent_before = g_a.sent.size();
  for (int k = 0; k < 40; ++k)
  {
    Gekko::NetPacket pkt = *inputs;
    pkt.header.type = Gekko::SpectatorInputs;
    Gekko::InputMsg msg;
    // Frames from well before to well after A's newest input: whatever A's next one is.
    msg.start_frame = pair.advances_a - 20 + k;
    msg.input_count = 40;
    msg.compressed = false;
    msg.inputs.assign(static_cast<size_t>(msg.input_count) * 2 * INPUT_SIZE, 0xAB);
    msg.total_size = static_cast<unsigned short>(msg.inputs.size());
    pkt.body = msg;
    g_a.inbox.push_back(Encode(pkt));
    pair.Step();
  }
  for (int i = 0; i < 30; ++i)
    pair.Step();

  const auto sent = InputsASent(sent_before);
  ASSERT_FALSE(sent.empty());
  for (const auto& bytes : sent)
  {
    EXPECT_EQ(std::count(bytes.begin(), bytes.end(), 0xAB), 0)
        << "A sent the peer's bytes as its own input";
  }
  EXPECT_EQ(pair.a_local_inputs_seen[0], 0x11);
}

// Random bytes, real packets with bytes flipped, cut or extended, and real packets with fields set
// to extremes. Nothing may crash, hang or allocate without bound.
TEST(GekkoNetFuzz, MalformedPacketsDoNotCrash)
{
  Pair pair;
  ASSERT_TRUE(pair.RunUntil(60));

  std::vector<std::vector<unsigned char>> corpus(g_b.sent.end() - std::min<size_t>(g_b.sent.size(), 200),
                                                 g_b.sent.end());
  ASSERT_FALSE(corpus.empty());

  std::mt19937 rng(20261007);
  auto pick = [&](auto lo, auto hi) {
    return std::uniform_int_distribution<long long>(lo, hi)(rng);
  };
  const std::array<int, 10> extremes{INT_MIN, INT_MIN + 1, -2, -1, 0, 1, 127, 128, INT_MAX - 1, INT_MAX};
  auto frame = [&]() -> int {
    return pick(0, 1) ? extremes[pick(0, 9)] : static_cast<int>(pick(-1000, 100000));
  };

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20000; ++i)
  {
    std::vector<unsigned char> bytes;
    switch (pick(0, 3))
    {
    case 0:  // random bytes
      bytes.resize(static_cast<size_t>(pick(0, 300)));
      for (auto& b : bytes)
        b = static_cast<unsigned char>(pick(0, 255));
      if (!bytes.empty() && pick(0, 1))
        bytes[0] = static_cast<unsigned char>(pick(0, 12));  // a plausible packet type
      break;
    case 1:  // a real packet, mutated
    {
      bytes = corpus[static_cast<size_t>(pick(0, static_cast<long long>(corpus.size()) - 1))];
      const int flips = static_cast<int>(pick(1, 8));
      for (int f = 0; f < flips && !bytes.empty(); ++f)
        bytes[static_cast<size_t>(pick(0, static_cast<long long>(bytes.size()) - 1))] ^=
            static_cast<unsigned char>(pick(1, 255));
      if (pick(0, 3) == 0)
        bytes.resize(static_cast<size_t>(pick(0, static_cast<long long>(bytes.size()))));
      else if (pick(0, 3) == 0)
        bytes.resize(bytes.size() + static_cast<size_t>(pick(1, 64)), 0xFF);
      break;
    }
    default:  // a real packet's header with a crafted body
    {
      auto base = Decode(corpus[static_cast<size_t>(pick(0, static_cast<long long>(corpus.size()) - 1))]);
      if (!base)
        continue;
      Gekko::NetPacket pkt = *base;
      switch (pick(0, 5))
      {
      case 0:
      case 1:
      {
        pkt.header.type = pick(0, 1) ? Gekko::Inputs : Gekko::SpectatorInputs;
        Gekko::InputMsg m;
        m.start_frame = frame();
        m.input_count = static_cast<unsigned short>(pick(0, 65535));
        m.compressed = pick(0, 1) != 0;
        m.inputs.resize(static_cast<size_t>(pick(0, 2048)));
        for (auto& b : m.inputs)
          b = static_cast<unsigned char>(pick(0, 3) ? 0xFF : pick(0, 255));  // RLE: long runs
        m.total_size = static_cast<unsigned short>(m.inputs.size());
        pkt.body = m;
        break;
      }
      case 2:
      {
        pkt.header.type = Gekko::InputAck;
        Gekko::InputAckMsg m;
        m.ack_frame = frame();
        m.frame_advantage = static_cast<signed char>(pick(-128, 127));
        pkt.body = m;
        break;
      }
      case 3:
      {
        pkt.header.type = Gekko::SessionHealth;
        Gekko::SessionHealthMsg m;
        m.frame = frame();
        m.checksum = static_cast<unsigned>(pick(0, UINT_MAX));
        pkt.body = m;
        break;
      }
      case 4:
      {
        pkt.header.type = Gekko::DisconnectClaim;
        Gekko::DisconnectClaimMsg m;
        m.player = static_cast<int>(pick(-2, 3));
        m.start_frame = frame();
        m.last_frame = pick(0, 1) ? frame() : m.start_frame + static_cast<int>(pick(0, 3));
        m.inputs.resize(static_cast<size_t>(pick(0, 4)) * INPUT_SIZE);
        pkt.body = m;
        break;
      }
      default:
      {
        pkt.header.type = Gekko::NetworkHealth;
        Gekko::NetworkHealthMsg m;
        m.send_time = static_cast<unsigned long long>(pick(0, LLONG_MAX));
        m.received = pick(0, 1) != 0;
        pkt.body = m;
        break;
      }
      }
      bytes = Encode(pkt);
      break;
    }
    }
    g_a.inbox.push_back(std::move(bytes));
    if (i % 16 == 0)
      pair.Step();
    ASSERT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(60))
        << "stuck at packet " << i;
  }
  for (int i = 0; i < 20; ++i)
    pair.Step();
  SUCCEED();
}

// What A sends B from `from` on (index into g_a.sent), by packet type.
std::array<int, 16> CountASent(size_t from)
{
  std::array<int, 16> n{};
  for (size_t i = from; i < g_a.sent.size(); ++i)
  {
    if (auto p = Decode(g_a.sent[i]); p && p->header.type < n.size())
      ++n[p->header.type];
  }
  return n;
}

// A frame's inputs go once: a new ack used to make the next poll send the remaining unacked
// inputs again (about two Inputs packets a frame), and every frame received got its own ack.
// Three frames each way, so acks come in while inputs are still unacked.
TEST(GekkoNetTraffic, OneInputsPacketAndOneAckPerFrame)
{
  Pair pair;
  g_delay_steps = 3;
  ASSERT_TRUE(pair.RunUntil(120));
  const size_t from = g_a.sent.size();
  const int frames_before = pair.advances_a;
  for (int i = 0; i < 600; ++i)
    pair.Step();
  const int frames = pair.advances_a - frames_before;
  ASSERT_GE(frames, 590);
  const auto n = CountASent(from);
  EXPECT_LE(n[Gekko::Inputs], frames + frames / 20) << "Inputs packets for " << frames << " frames";
  EXPECT_LE(n[Gekko::InputAck], frames + frames / 20) << "acks for " << frames << " frames";
  EXPECT_GE(n[Gekko::Inputs], frames - frames / 20);
}

// With a tenth of A's packets lost, every input still reaches B (the next frame's packet carries
// the unacked ones again) and neither side stalls for long.
TEST(GekkoNetTraffic, LostPacketsAreRepairedByTheNextFrame)
{
  Pair pair;
  g_delay_steps = 3;
  ASSERT_TRUE(pair.RunUntil(120));
  g_drop_a_every = 10;
  const int a0 = pair.advances_a, b0 = pair.advances_b;
  for (int i = 0; i < 600; ++i)
    pair.Step();
  EXPECT_GE(pair.advances_a - a0, 590);
  EXPECT_GE(pair.advances_b - b0, 590);
}

// The local input delay changed mid-session (the launcher's setting while a game runs): both
// sides still see the same input for A's player on every confirmed frame. Raising it from 2 to 4
// repeats A's last input for the 2 frames it grew by and drops nothing; lowering it from 4 to 1
// drops A's next 3 inputs (their frames already went out) and repeats nothing. Before the fix,
// the raise repeated the input for the whole new delay (4 frames) and then dropped 2 inputs.
TEST(GekkoNetDelay, ChangeMidSessionKeepsBothSidesTogether)
{
  // A's inputs after the start: FIRST + 1 .. FIRST + 200, apart from the start's 0x11 and the
  // end's 0xF0.
  constexpr int FIRST = 0x20;
  Pair pair;
  ASSERT_TRUE(pair.RunUntil(120));
  for (int i = 1; i <= 200; ++i)
  {
    if (i == 50)
      gekko_set_local_delay(pair.a, 0, 4);
    if (i == 120)
      gekko_set_local_delay(pair.a, 0, 1);
    pair.Step(static_cast<unsigned char>(FIRST + i));
  }
  for (int i = 0; i < 60; ++i)
    pair.Step(0xF0);
  const int confirmed = std::min(gekko_confirmed_frame(pair.a), gekko_confirmed_frame(pair.b));
  ASSERT_GT(confirmed, 300);
  int compared = 0;
  for (const auto& [frame, value] : pair.b_frames)
  {
    if (frame > confirmed)
      break;
    const auto it = pair.a_frames.find(frame);
    ASSERT_NE(it, pair.a_frames.end()) << "frame " << frame;
    EXPECT_EQ(it->second, value) << "frame " << frame;
    ++compared;
  }
  EXPECT_GT(compared, 300);
  // A's inputs in frame order, as B played them.
  std::vector<int> seq;
  for (const auto& [frame, value] : pair.b_frames)
  {
    if (frame <= confirmed && value > FIRST && value <= FIRST + 200)
      seq.push_back(value - FIRST);
  }
  ASSERT_FALSE(seq.empty());
  int repeats = 0, skipped = 0;
  for (size_t i = 1; i < seq.size(); ++i)
  {
    ASSERT_GE(seq[i], seq[i - 1]) << "at " << i;
    if (seq[i] == seq[i - 1])
      ++repeats;
    else
      skipped += seq[i] - seq[i - 1] - 1;
  }
  EXPECT_EQ(seq.front(), 1);
  EXPECT_EQ(seq.back(), 200);
  EXPECT_EQ(repeats, 2);
  EXPECT_EQ(skipped, 3);
}
