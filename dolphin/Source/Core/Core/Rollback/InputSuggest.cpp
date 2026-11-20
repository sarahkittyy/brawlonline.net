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
// - cross (default): only a change that can start an action in P+ (Meaningful, on P+'s input
//   map).
// - follow: cross, and after a crossing for a frame, any later change before that frame's read, at
//   most one every FOLLOW_GAP. With the 16 lines measured from idle
//   they caught what the coarse lines missed; with P+'s whole map they show nothing more and send
//   about 1.7x as many datagrams on sticks that move between reads (an offline model of the
//   suggestions on 1,536 ranked replays).
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
    return s == "all" ? Rules::All : s == "follow" ? Rules::Follow : Rules::Cross;
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
// P+ 3.2's input map, read off the game's code (ipPadConfig, soController, the common Fighter.pac's
// PSA and P+'s codes on top) and checked in the game (harness/tools/pplus_stick_lines.py): the
// controller values the game can tell apart, in gfPadStatus units (the sticks against their
// origin, s8: +0x30 main x, +0x31 main y, +0x32 C x, +0x33 C y; the triggers, u8: +0x34 L, +0x35 R).
//
// The main stick becomes a float in ipPadConfig (0x8004819C with P+'s "Directional Input Capped at
// Distance of 1" and "General Directional Input Multiplier 0.010325"): a vector longer than 100 is
// scaled to 100 and truncated, then each axis is times 0.010325, clamped to +-1, and 0 when
// |v| <= 0.28. Each line is the first value past a threshold the game compares that float with
// (the common fighter parameters, IC-Basic, as P+ sets them; none falls exactly on a value). x
// lines count both ways (the game also reads x against the fighter's facing): dead zone 28, 0.3 30
// and 0.325 32 (C-stick attacks), flick 0.35 34 (soController counts the frames since a stick
// passed it: a dash needs fewer than 2), 0.375 37, pivot 0.475 47, 0.6 59, run 0.62 61, 0.7 68
// (shield), dash 0.73 71, roll and tumble wiggle 0.75 73, forward smash 0.8 78, 0.85 83, 0.945 92.
constexpr int X_LINES[] = {28, 30, 32, 34, 37, 47, 59, 61, 68, 71, 73, 78, 83, 92};
// y up: dead zone 28, 0.3 30, 0.55 54, 0.6 59, tap jump from a dash or run 0.635 62, tap jump 0.7
// 68, up smash 0.73 71.
constexpr int Y_UP[] = {28, 30, 54, 59, 62, 68, 71};
// y down (y <= -line): dead zone 28, -0.375 37, 0.55 54, 0.6 59, crouch -0.625 61, -0.66 64, fast
// fall -0.665 65, UCF shield drop -0.675 66, -0.7 68, platform drop -0.71 69, down smash -0.73 71,
// spot dodge -0.75 73, UCF spot dodge -0.8 78.
constexpr int Y_DOWN[] = {28, 37, 54, 59, 61, 64, 65, 66, 68, 69, 71, 73, 78};
// The C-stick (0x8004897C) is an input while cx^2 + cy^2 > 2500 (a diagonal from 36, 36); as the
// default attack stick it presses A with the main stick at trunc(32 * its direction). P+'s shield
// C-stick buffers read cx and cy alone (times 0.011775, against +-0.6: 51).
constexpr int C_RADIUS2 = 2500;
constexpr int C_AXIS = 51;
// The analog triggers: (min(v, 150) / 150) at 0.3 is a new L or R press (P+'s light shield), 45.
constexpr u8 TRIGGER_AT = HOST_DECODE_LIGHT_PRESS;
static_assert(TRIGGER_AT == 45);

template <std::size_t N>
s8 LinesPassed(int v, const int (&lines)[N])
{
  int n = 0;
  for (const int l : lines)
    n += v >= l;
  return static_cast<s8>(n);
}
s8 XZone(int x)
{
  return x >= 0 ? LinesPassed(x, X_LINES) : static_cast<s8>(-LinesPassed(-x, X_LINES));
}
s8 YZone(int y)
{
  return y >= 0 ? LinesPassed(y, Y_UP) : static_cast<s8>(-LinesPassed(-y, Y_DOWN));
}

// Where a pad is in the map: per axis, the lines it is past (negative: on the minus side).
struct Zones
{
  s8 x, y;      // main stick (after the cap)
  s8 c;         // C-stick input (1) or not (0)
  s8 cx, cy;    // the direction of the attack it makes (while c)
  s8 cax, cay;  // cx, cy against +-51
  s8 l, r;      // triggers at 45
};

Zones ZonesOf(const u8* pad)
{
  Zones z{};
  int x = static_cast<s8>(pad[0x30]);
  int y = static_cast<s8>(pad[0x31]);
  if (x * x + y * y > 10000)
  {
    // The game scales with frsqrte's estimate: at a line's very edge on a long diagonal this can
    // be a unit off.
    const float s = 100.0f / std::sqrt(static_cast<float>(x * x + y * y));
    x = static_cast<int>(static_cast<float>(x) * s);
    y = static_cast<int>(static_cast<float>(y) * s);
  }
  z.x = XZone(x);
  z.y = YZone(y);
  const int cx = static_cast<s8>(pad[0x32]);
  const int cy = static_cast<s8>(pad[0x33]);
  const int c2 = cx * cx + cy * cy;
  if (c2 > C_RADIUS2)
  {
    // Its attack's direction: which of the main stick's axes leave the dead zone (32 * a
    // direction's component from 28: within 29 degrees of an axis). The finer lines at that
    // scale would only tell an exactly level C-stick (32) from one a unit off (31).
    const float k = 100.0f / std::sqrt(static_cast<float>(c2));
    const int mx = static_cast<int>(static_cast<float>(cx) * k * 0.32f);
    const int my = static_cast<int>(static_cast<float>(cy) * k * 0.32f);
    z.c = 1;
    z.cx = static_cast<s8>(mx >= X_LINES[0] ? 1 : mx <= -X_LINES[0] ? -1 : 0);
    z.cy = static_cast<s8>(my >= Y_UP[0] ? 1 : my <= -Y_DOWN[0] ? -1 : 0);
  }
  z.cax = static_cast<s8>(cx >= C_AXIS ? 1 : cx <= -C_AXIS ? -1 : 0);
  z.cay = static_cast<s8>(cy >= C_AXIS ? 1 : cy <= -C_AXIS ? -1 : 0);
  z.l = pad[0x34] >= TRIGGER_AT;
  z.r = pad[0x35] >= TRIGGER_AT;
  return z;
}

bool SameZones(const Zones& a, const Zones& b)
{
  return a.x == b.x && a.y == b.y && a.c == b.c && a.cx == b.cx && a.cy == b.cy &&
         a.cax == b.cax && a.cay == b.cay && a.l == b.l && a.r == b.r;
}

// One axis of the judge: `s` (suggested) is on the wrong side of a line, seen from `r` (real), that
// `p` (the last input) was on the right side of.
bool AxisWorse(int p, int s, int r)
{
  if (s == r)
    return false;
  if (p == r || (s > r) != (p > r))
    return true;
  return std::abs(s - r) > std::abs(p - r);
}

// gfPadStatus's buttons: Start, Y, X, B, A (+2) and L, R, Z and the D-pad (+3; P+'s taunts).
u16 Buttons(const u8* pad)
{
  return static_cast<u16>(((pad[2] & 0x1F) << 8) | (pad[3] & 0x7F));
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
  return Buttons(prev) != Buttons(next) || !SameZones(ZonesOf(prev), ZonesOf(next));
}

unsigned int WorseCost(const u8* previous, const u8* suggested, const u8* real)
{
  // Slippi's costs: a wrong button the last input had right, 3; else, with the buttons as right as
  // the last input's, a stick or trigger worse than it (JudgeByBytes), 1.
  const u16 previous_wrong = Buttons(previous) ^ Buttons(real);
  const u16 suggested_wrong = Buttons(suggested) ^ Buttons(real);
  if (suggested_wrong & ~previous_wrong)
    return 3;
  if (suggested_wrong != previous_wrong)
    return 0;
  if (JudgeByBytes())
    return AnalogError(suggested, real) > AnalogError(previous, real) ? 1 : 0;
  const Zones p = ZonesOf(previous), s = ZonesOf(suggested), r = ZonesOf(real);
  const bool worse = AxisWorse(p.x, s.x, r.x) || AxisWorse(p.y, s.y, r.y) ||
                     AxisWorse(p.c, s.c, r.c) || AxisWorse(p.cx, s.cx, r.cx) ||
                     AxisWorse(p.cy, s.cy, r.cy) || AxisWorse(p.cax, s.cax, r.cax) ||
                     AxisWorse(p.cay, s.cay, r.cay) || AxisWorse(p.l, s.l, r.l) ||
                     AxisWorse(p.r, s.r, r.r);
  return worse ? 1 : 0;
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
