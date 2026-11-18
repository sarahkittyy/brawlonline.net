// Copyright 2026 brawlonline.net
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/InputSuggest.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

#include <SFML/Network/UdpSocket.hpp>

#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/Rollback/GameplayRollback.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/GCPadStatus.h"

namespace Gprb::Suggest
{
namespace
{
// Which controller changes go out (PPR_GPRB_SUGGEST_RULES):
// - all: any change; at most ANALOG_PER_FRAME a frame that change only the sticks or triggers
//   (GitHub issue #35's patch as posted).
// - cross: only a change that can start an action in P+ (Meaningful: Slippi's and Melee
//   Unlocked's rules with P+'s lines).
// - follow (default): cross, and after a crossing for a frame, any later change before that frame's
//   read, at most one every FOLLOW_GAP (Melee Unlocked's follow-ups): a prediction is right only if
//   every byte is, and a stick keeps moving after it crosses a line.
enum class Rules
{
  All,
  Cross,
  Follow,
};

Rules RulesMode()
{
  static const Rules v = [] {
    const char* e = std::getenv("PPR_GPRB_SUGGEST_RULES");
    const std::string s = e ? e : "";
    return s == "all" ? Rules::All : s == "cross" ? Rules::Cross : Rules::Follow;
  }();
  return v;
}

constexpr int ANALOG_PER_FRAME = 3;
constexpr auto FOLLOW_GAP = std::chrono::milliseconds(2);

// Tests (PPR_GPRB_SUGGEST_LIE=1): every suggestion says the opposite of A and a random main stick,
// as a peer that lies would (the receivers' judge must stop following it).
bool Lies()
{
  static const bool v = [] {
    const char* e = std::getenv("PPR_GPRB_SUGGEST_LIE");
    return e && *e == '1';
  }();
  return v;
}

// A controller mapped in Dolphin (keyboard, XInput, SDL, DirectInput...) has no report stream of
// its own: while the local player plays one, a thread updates the input devices and samples it at
// this rate (PPR_GPRB_SUGGEST_SAMPLE_HZ; 1000, an overclocked adapter's rate; 125, a stock one's).
// The harness's pads are such controllers.
double SampleHz()
{
  static const double v = [] {
    const char* e = std::getenv("PPR_GPRB_SUGGEST_SAMPLE_HZ");
    const double hz = e && *e ? std::atof(e) : 1000.0;
    return hz > 0 ? hz : 1000.0;
  }();
  return v;
}

// ControllerInterface updates every device under one mutex, which the CPU thread's SI poll takes
// too: the sampler waits at least this many times as long as its last sample took, so it holds the
// devices at most a quarter of the time (Dolphin.exe's hotkey thread already updates them twice
// every 5 ms).
constexpr int SAMPLE_IDLE_PER_BUSY = 3;

struct Sender
{
  bool armed = false;
  sf::UdpSocket* socket = nullptr;
  int chan = -1;
  std::vector<Peer> peers;
  // The newest local input GekkoNet took, the instant its read stands for, and a field then.
  int last_frame = -1;
  Clock::time_point last_instant{};
  double field_us = 0;
  std::array<u8, INPUT_BYTES> last_input{};
  // What the peers know of the controller: the newest input or suggestion they were sent.
  std::array<u8, INPUT_BYTES> known{};
  // The frame of the newest suggestion sent, and how many for that frame changed only analog.
  int sugg_frame = -1;
  int analog_sent = 0;
  // follow: the frame of the newest crossing sent, and when the newest suggestion for it went.
  int cross_frame = -1;
  Clock::time_point last_sent{};
  u32 lie_rng = 0x2545F491;
  u32 seq = 0;
  SenderStats stats;
};
std::mutex s_mutex;
Sender s_sender;

// SI channel `chan`'s controller from `source` was `st` at host time `t`. Only the source the game
// reads counts: an adapter can be plugged in while the player plays a mapped controller.
void OnReport(int chan, HostPadSource source, const GCPadStatus& st, Clock::time_point t)
{
  std::lock_guard lk(s_mutex);
  Sender& x = s_sender;
  if (!x.armed || chan != x.chan || x.last_frame < 0 || x.peers.empty() ||
      !HostDecodeReads(chan, source))
  {
    return;
  }
  ++x.stats.reports;
  // A read at or after `t` took it (a frame GekkoNet has).
  if (t <= x.last_instant)
    return;
  // Each frame's read is a field after the last: the first read after `t` is the next frame's,
  // unless that read came already and its frame was not taken yet (a frame that ran late).
  const double after_us = std::chrono::duration<double, std::micro>(t - x.last_instant).count();
  const int k = std::max(1, static_cast<int>(std::ceil(after_us / x.field_us)));
  if (k > 2)
  {
    ++x.stats.too_late;
    return;
  }
  std::array<u8, INPUT_BYTES> in;
  if (!HostDecodeLatest(chan, source, st, in.data()))
  {
    ++x.stats.undecoded;
    return;
  }
  if (in == x.known)
    return;
  const int frame = x.last_frame + k;
  // gfPadStatus's buttons (+2, +3), P+'s light trigger presses included.
  const bool buttons = in[2] != x.known[2] || in[3] != x.known[3];
  const int analog_sent = frame == x.sugg_frame ? x.analog_sent : 0;
  const Rules rules = RulesMode();
  if (rules == Rules::All)
  {
    if (!buttons && analog_sent >= ANALOG_PER_FRAME)
    {
      ++x.stats.held_back;
      return;
    }
  }
  else if (Meaningful(x.known.data(), in.data()))
  {
    ++x.stats.crossings;
    x.cross_frame = frame;
  }
  else if (rules == Rules::Follow && x.cross_frame == frame && t - x.last_sent >= FOLLOW_GAP)
  {
    ++x.stats.follow_ups;
  }
  else
  {
    ++x.stats.held_back;
    return;
  }
  x.last_sent = t;
  std::array<u8, INPUT_BYTES> wire = in;
  if (Lies())
  {
    x.lie_rng = x.lie_rng * 1664525u + 1013904223u;
    wire[2] ^= 0x01;  // A
    wire[6] = wire[2];
    wire[0x30] = static_cast<u8>(x.lie_rng >> 24);
  }
  const auto d = Encode(frame, x.seq++, wire.data());
  // Tests (PPR_GPRB_SUGGEST_SEND=0): everything up to the send, nothing sent (the sampler's cost
  // without the traffic).
  static const bool sends = [] {
    const char* e = std::getenv("PPR_GPRB_SUGGEST_SEND");
    return !(e && *e == '0');
  }();
  if (sends)
  {
    for (const Peer& p : x.peers)
      (void)x.socket->send(d.data(), d.size(), p.ip, p.port);
  }
  ++x.stats.sent;
  x.known = in;
  x.sugg_frame = frame;
  x.analog_sent = analog_sent + (buttons ? 0 : 1);
}

void OnAdapterReport(int chan, const GCPadStatus& st, Clock::time_point t)
{
  OnReport(chan, HostPadSource::Adapter, st, t);
}

// The mapped controller's sampler (see SampleHz).
std::thread s_sampler;
std::atomic<bool> s_sampler_run{false};

void SamplerLoop(int chan)
{
  Common::SetCurrentThreadName("GPRB suggest sampler");
  // Its own input channel: a relative input (a mouse's motion) gives each channel the motion since
  // that channel's last update, so the SI poll's is left alone.
  g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Suggestions);
  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(1.0 / SampleHz()));
  auto next = Clock::now();
  while (s_sampler_run.load(std::memory_order_relaxed))
  {
    next += period;
    std::this_thread::sleep_until(next);
    const auto start = Clock::now();
    if (start - next > 10 * period)
      next = start;  // fell far behind (a suspended process): no burst of catch-up samples
    bool any_peer;
    {
      std::lock_guard lk(s_mutex);
      any_peer = !s_sender.peers.empty();
    }
    // The game reads the adapter (its reports come on their own) or nothing yet, or no peer takes
    // suggestions.
    if (!any_peer || !HostDecodeReads(chan, HostPadSource::Mapped) ||
        !g_controller_interface.IsInit() || !Pad::IsInitialized())
    {
      next = start + std::chrono::milliseconds(50);
      continue;
    }
    g_controller_interface.UpdateInput();
    // As the CPU thread before an SI poll: the controllers count only while the game window has
    // focus, unless background input is on.
    Core::UpdateInputGate(!Config::Get(Config::MAIN_INPUT_BACKGROUND_INPUT),
                          Config::Get(Config::MAIN_LOCK_CURSOR));
    const GCPadStatus st = Pad::GetStatus(chan);
    const auto now = Clock::now();
    const auto busy = now - start;
    {
      std::lock_guard lk(s_mutex);
      SenderStats& c = s_sender.stats;
      const u64 us = std::chrono::duration_cast<std::chrono::microseconds>(busy).count();
      ++c.samples;
      c.sample_us += us;
      c.sample_us_max = std::max(c.sample_us_max, us);
    }
    // Stamped after the read: a later time can only tag a later frame, which the change is in too.
    OnReport(chan, HostPadSource::Mapped, st, now);
    next = std::max(next, now + SAMPLE_IDLE_PER_BUSY * busy);
  }
}
}  // namespace

namespace
{
// The lines P+ acts on, in gfPadStatus units (the stick against its origin, s8: +0x30 main x, +0x31
// main y, +0x32 C x, +0x33 C y), measured in the game (harness/tools/pplus_stick_lines.py):
// a value at or past `at` (at > 0: v >= at, at < 0: v <= at) is inside the line.
struct Line
{
  u8 off;
  s8 at;
};
constexpr Line LINES[] = {
    // Main stick x: walk and forward tilt 28, dash 71, forward smash 78.
    {0x30, 28}, {0x30, -28}, {0x30, 71}, {0x30, -71}, {0x30, 78}, {0x30, -78},
    // Main stick y: up/down tilt 28, tap jump 68, crouch -61, up/down smash 71.
    {0x31, 28}, {0x31, -28}, {0x31, 68}, {0x31, -61}, {0x31, 71}, {0x31, -71},
    // C-stick: smashes 51 (nothing below it).
    {0x32, 51}, {0x32, -51}, {0x33, 51}, {0x33, -51},
};
// The analog triggers (+0x34 L, +0x35 R, against the origin): P+'s light shield.
constexpr u8 TRIGGER_AT = HOST_DECODE_LIGHT_PRESS;

bool Inside(const Line& l, const u8* pad)
{
  const s8 v = static_cast<s8>(pad[l.off]);
  return l.at > 0 ? v >= l.at : v <= l.at;
}

// gfPadStatus's buttons: Start, Y, X, B, A (+2) and L, R, Z (+3), not the D-pad.
u16 ActionButtons(const u8* pad)
{
  return static_cast<u16>(((pad[2] & 0x1F) << 8) | (pad[3] & 0x70));
}

u32 AnalogError(const u8* a, const u8* b)
{
  const auto axis = [&](int off) {
    return static_cast<u32>(std::abs(static_cast<s8>(a[off]) - static_cast<s8>(b[off])));
  };
  const auto trigger = [&](int off) {
    if (a[off] < TRIGGER_AT && b[off] < TRIGGER_AT)
      return 0u;
    return static_cast<u32>(std::abs(a[off] - b[off]));
  };
  return std::max(axis(0x30), axis(0x31)) + std::max(axis(0x32), axis(0x33)) +
         std::max(trigger(0x34), trigger(0x35));
}

// Which side of each line (LINES, then the two triggers' light shield) the pad is on.
u32 Zones(const u8* pad)
{
  u32 z = 0;
  int bit = 0;
  for (const Line& l : LINES)
    z |= static_cast<u32>(Inside(l, pad)) << bit++;
  z |= static_cast<u32>(pad[0x34] >= TRIGGER_AT) << bit++;
  z |= static_cast<u32>(pad[0x35] >= TRIGGER_AT) << bit++;
  return z;
}
static_assert(std::size(LINES) + 2 <= 32);

// How the judge weighs sticks and triggers (PPR_GPRB_SUGGEST_JUDGE): "bytes", Slippi's, a value
// further off than the last input's; else (default) "zones", on the wrong side of a line P+ acts on
// where the last input was on the right one. A stick that jitters by a unit makes about every other
// suggestion "further off" by bytes, and its honest player's suggestions were dropped for good
// (harness: a walk held with a +-1 jitter, ignored within 40 s at 40 ms).
bool JudgeByBytes()
{
  static const bool v = [] {
    const char* e = std::getenv("PPR_GPRB_SUGGEST_JUDGE");
    return e && std::string(e) == "bytes";
  }();
  return v;
}
}  // namespace

bool Meaningful(const u8* prev, const u8* next)
{
  if (ActionButtons(prev) != ActionButtons(next))
    return true;
  for (const int t : {0x34, 0x35})
  {
    if ((prev[t] >= TRIGGER_AT) != (next[t] >= TRIGGER_AT))
      return true;
  }
  for (const Line& l : LINES)
  {
    if (Inside(l, prev) != Inside(l, next))
      return true;
  }
  return false;
}

unsigned int WorseCost(const u8* previous, const u8* suggested, const u8* real)
{
  // Slippi's costs: a wrong button the last input had right, 3; else, with the buttons as right as
  // the last input's, a stick or trigger worse than it (JudgeByBytes), 1.
  const u16 previous_wrong = ActionButtons(previous) ^ ActionButtons(real);
  const u16 suggested_wrong = ActionButtons(suggested) ^ ActionButtons(real);
  if (suggested_wrong & ~previous_wrong)
    return 3;
  if (suggested_wrong != previous_wrong)
    return 0;
  if (JudgeByBytes())
    return AnalogError(suggested, real) > AnalogError(previous, real) ? 1 : 0;
  const u32 real_zones = Zones(real);
  return ((Zones(suggested) ^ real_zones) & ~(Zones(previous) ^ real_zones)) != 0 ? 1 : 0;
}

std::array<u8, DATAGRAM_SIZE> Encode(s32 frame, u32 seq, const u8* input)
{
  std::array<u8, DATAGRAM_SIZE> d{};
  d[0] = DATAGRAM;
  d[1] = VERSION;
  const u32 f = static_cast<u32>(frame);
  for (int i = 0; i < 4; ++i)
  {
    d[2 + i] = static_cast<u8>(f >> (8 * i));
    d[6 + i] = static_cast<u8>(seq >> (8 * i));
  }
  std::memcpy(d.data() + 10, input, INPUT_BYTES);
  return d;
}

bool Decode(const u8* data, std::size_t size, s32* frame, u32* seq, const u8** input)
{
  if (size != DATAGRAM_SIZE || data[0] != DATAGRAM || data[1] != VERSION)
    return false;
  u32 f = 0, q = 0;
  for (int i = 0; i < 4; ++i)
  {
    f |= static_cast<u32>(data[2 + i]) << (8 * i);
    q |= static_cast<u32>(data[6 + i]) << (8 * i);
  }
  *frame = static_cast<s32>(f);
  *seq = q;
  *input = data + 10;
  return true;
}

bool Enabled()
{
  static const bool on = [] {
    const char* e = std::getenv("PPR_GPRB_SUGGEST");
    return !(e && *e == '0');
  }();
  return on;
}

void Arm(sf::UdpSocket* socket, int chan)
{
  Disarm();
  if (!socket || chan < 0 || chan >= 4)
    return;
  {
    std::lock_guard lk(s_mutex);
    s_sender = {};
    s_sender.armed = true;
    s_sender.socket = socket;
    s_sender.chan = chan;
  }
  if (!Enabled())
    return;
  GCAdapter::SetReportHook(OnAdapterReport);
  s_sampler_run = true;
  s_sampler = std::thread(SamplerLoop, chan);
}

void Disarm()
{
  GCAdapter::SetReportHook(nullptr);
  s_sampler_run = false;
  if (s_sampler.joinable())
    s_sampler.join();
  std::lock_guard lk(s_mutex);
  s_sender.armed = false;
  s_sender.socket = nullptr;
  s_sender.peers.clear();
}

void SetPeers(std::vector<Peer> peers)
{
  std::lock_guard lk(s_mutex);
  if (s_sender.armed)
    s_sender.peers = std::move(peers);
}

void NoteLocalInput(int frame, Clock::time_point instant, const u8* input, double field_us)
{
  std::lock_guard lk(s_mutex);
  Sender& x = s_sender;
  if (!x.armed || frame <= x.last_frame)
    return;
  if (x.last_frame >= 0 && std::memcmp(x.last_input.data(), input, INPUT_BYTES) != 0)
    ++x.stats.local_changes;
  x.last_frame = frame;
  x.last_instant = instant;
  x.field_us = field_us;
  std::memcpy(x.last_input.data(), input, INPUT_BYTES);
  // The peers know this input, unless a suggestion for a later frame (a newer state) went out.
  if (x.sugg_frame <= frame)
    x.known = x.last_input;
}

SenderStats Stats()
{
  std::lock_guard lk(s_mutex);
  return s_sender.stats;
}
}  // namespace Gprb::Suggest
