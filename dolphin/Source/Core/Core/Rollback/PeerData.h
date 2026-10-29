// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Checks for everything the other player's machine sends to the gameplay session (control
// messages, lock-ins, the host's match setup, sync block and init block, and the pad bytes
// GekkoNet hands over). The peer is a stranger from matchmaking: none of it is trusted. A value
// that can reach emulated memory, or be used as an index or size, is checked here first, so that
// the worst a malicious peer can do is end the session (as leaving would), never write arbitrary
// bytes into the game or crash Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Gprb::PeerData
{
// Limits for control messages ('C' packets, or 'F' pieces put back together): the session's own
// messages are under 8 KiB (the host's state at the barrier, with the task order, about 5.5 KiB).
constexpr size_t MAX_CONTROL_SIZE = 16 * 1024;
constexpr int MAX_JSON_DEPTH = 8;
constexpr size_t MAX_NAME_LEN = 64;
// gfTaskScheduler task lists (TASK_LISTS in GameplaySession.cpp: 17 + 40 + 14 lists).
constexpr size_t MAX_TASK_LISTS = 128;
constexpr size_t MAX_TASKS_PER_LIST = 4096;
constexpr size_t MAX_TASK_NAME_LEN = 64;
// Control messages on the wire. A UDP datagram larger than the path's MTU (1472 bytes of payload
// on a 1500-byte MTU) travels as IP fragments, and some routers, CGNATs and ISPs drop fragments:
// the host's state message at the barrier (its sync block and task order, about 5.5 KB) never reached
// such a joiner, which then dropped the host after 7.2 s of silence (prod 2026-10-13). So a
// control message whose 'C' packet would be larger than MAX_DATAGRAM goes as 'F' packets:
// 'F', the message's id (u32, little-endian), the piece's index and the number of pieces (u8
// each), then the piece. The receiver puts the pieces back together (ControlReassembler); a lost
// piece loses that copy of the message, as a lost 'C' packet does (state is resent every 50 ms).
constexpr size_t MAX_DATAGRAM = 1200;
constexpr size_t FRAGMENT_HEADER = 1 + 4 + 1 + 1;
constexpr size_t FRAGMENT_PIECE = MAX_DATAGRAM - FRAGMENT_HEADER;
constexpr size_t MAX_FRAGMENTS = (MAX_CONTROL_SIZE + FRAGMENT_PIECE - 1) / FRAGMENT_PIECE;
// Sources (address and port) with a message in progress; the least recently heard is forgotten.
constexpr size_t MAX_REASSEMBLY_SOURCES = 8;

// The datagrams for one control message `text` (at most MAX_CONTROL_SIZE): one 'C' packet when
// it fits in MAX_DATAGRAM, else its 'F' packets with message id `id`.
std::vector<std::vector<u8>> ControlDatagrams(std::string_view text, u32 id);

// GekkoNet sends several small messages a frame to each peer (the frame's inputs, an ack, the
// checksum of a confirmed frame, a ping now and then), each its own 'G' datagram: about four a
// frame, and at 60 frames a second the 28 bytes of UDP/IP header each cost more than the
// messages. To a peer whose state says it takes them ("gb"), the messages of one session update
// go as one 'B' datagram: 'B', then per message its length (u16, little-endian, at least 1) and
// the message as a 'G' packet carries it. Older builds get 'G' packets.
constexpr size_t MAX_GEKKO_BUNDLE = MAX_DATAGRAM;
constexpr size_t MAX_BUNDLED_MESSAGES = 64;

// Appends GekkoNet message `msg` to the 'B' datagram `bundle` (starting it when empty). False,
// with `bundle` unchanged, when the result would be larger than MAX_GEKKO_BUNDLE or hold more than
// MAX_BUNDLED_MESSAGES messages, or `msg` is empty.
bool AppendGekkoBundle(std::vector<u8>& bundle, const u8* msg, size_t size);

// The GekkoNet messages of a 'B' datagram (`data` starts with the 'B'). nullopt for a malformed
// one: a length of 0 or past the end, or more than MAX_BUNDLED_MESSAGES messages.
std::optional<std::vector<std::vector<u8>>> SplitGekkoBundle(const u8* data, size_t size);

// Puts 'F' packets back together, per source. Everything in a piece is checked: a malformed
// piece is dropped, a piece of another message (id or count) than the one in progress from that
// source starts over, and memory is bounded (MAX_REASSEMBLY_SOURCES messages of at most
// MAX_CONTROL_SIZE).
class ControlReassembler
{
public:
  // One 'F' packet (`data` starts with the 'F') from `source`: the whole message once this piece
  // completes it, else nullopt.
  std::optional<std::string> Add(u64 source, const u8* data, size_t size);
  void Clear() { m_partials.clear(); }
  size_t InProgress() const { return m_partials.size(); }

private:
  struct Partial
  {
    u64 source = 0;
    u32 id = 0;
    u8 count = 0;
    u32 have = 0;  // bit i: piece i arrived
    u64 touched = 0;
    std::array<std::string, MAX_FRAGMENTS> pieces;
  };
  std::vector<Partial> m_partials;
  u64 m_clock = 0;
};

// Frames GekkoNet may queue on the CSS before a match polls them.
constexpr size_t MAX_QUEUED_GEKKO_PACKETS = 1024;
// A pong's round trip larger than this is not a measurement.
constexpr double MAX_RTT_MS = 5000.0;

// True when `text` is at most `max_depth` arrays/objects deep. picojson parses recursively with
// no depth limit, so a deeply nested message would overflow the network thread's stack.
bool JsonDepthOk(std::string_view text, int max_depth = MAX_JSON_DEPTH);

// True when `text` is safe to hand to picojson: at most `max_depth` deep (above), and no number
// outside a string that overflows a double. Dolphin's picojson calls std::abort() for a number it
// parses to infinity ("1e999"), so a single such message would end the process.
bool JsonSafeToParse(std::string_view text, int max_depth = MAX_JSON_DEPTH);

// Parses `text` (a control message) with the size and depth limits. nullopt if it is too large,
// too deep, not JSON, or not an object.
std::optional<picojson::object> ParseControl(std::string_view text);

// Exactly `bytes` bytes as 2 * `bytes` hex digits, else nullopt (never throws).
std::optional<std::vector<u8>> ParseHex(std::string_view hex, size_t bytes);

// A JSON number that is a whole number in [0, max], else nullopt. (A cast of a double outside
// the target type's range is undefined behaviour, so nothing is cast before this check.)
std::optional<u64> JsonUInt(const picojson::value* v, u64 max);

// A peer's cursor on an online stage select, its state's "sc": [page, x, y], page below
// `max_page`, x and y whole numbers within +-`range` (1/64 units). Else nullopt.
struct SssCursorValues
{
  u8 page = 0;
  s16 x = 0, y = 0;
};
std::optional<SssCursorValues> ParseSssCursor(const picojson::value* v, u8 max_page, s16 range);

// gmCharacterKind values the online character select can lock in (docs/brawl-memory-map.md):
// the playable kinds 0x00-0x11 and 0x13-0x2B, and P+'s Roy 0x32, Mewtwo 0x33 and Knuckles 0x35.
// Not P+'s hold-shield forms 0x2C (Giga Bowser) and 0x2D (Wario-Man): they are not played online.
bool ValidCharKind(u32 kind);
// Colour numbers: the character select's own bound (game-code online_menu.cpp selectedCostume),
// with at most one of P+'s alt costume bits above it (ppom.h COSTUME_ALT_Z 0x40 / COSTUME_ALT_R
// 0x80: Z or R held on the START that locked in).
constexpr u32 MAX_COSTUME = 0x1F;
constexpr u32 COSTUME_ALT_Z = 0x40;
constexpr u32 COSTUME_ALT_R = 0x80;
bool ValidCostume(u32 costume);
// A lock-in made with the coin on RANDOM (LockIn.css): its character is drawn at lock-in, and
// that player sees only RANDOM until the match. What the game shows of a player (Ranked's line
// and stage select cards, a room's panels) carries COSTUME_RANDOM instead (ppom.h), never in a
// lock-in itself.
constexpr u8 CSS_RANDOM = 0x29;
constexpr u32 COSTUME_RANDOM = 0x20;
// A lock-in's team: Brawl's three team colours 0-2 (gmPlayerInitData::m_teamNo; 0 red, 1 blue,
// 2 green), or 0xFF for none.
bool ValidTeam(u32 team);
// In-game ports (the session's player slots): 0-3.
constexpr int MAX_PORTS = 4;
bool ValidPort(u64 port);
// The gone marker (docs/nplayer/session.md): GekkoNet hands this input for a player that left a
// 3-4 player match, from the agreed frame on. The marker sits in gfPadStatus padding (+0x39..+0x3B,
// between the error byte and the controller type), which the game never reads; local input has
// those bytes cleared before it is sent, so no real input carries it. TakeGoneMarker reports
// whether `pad` carries it and clears it (the pad is then a neutral, connected GameCube controller).
constexpr size_t GONE_MARK_OFFSET = 0x39;
constexpr std::array<u8, 3> GONE_MARK{'G', 'O', 'N'};
void SetGoneMarker(u8* pad);
bool TakeGoneMarker(u8* pad);
void ClearGoneMarkerBytes(u8* pad);
// srStageKind values a versus match can be played on (0x01-0x7F without menus, results and
// single-player modes; P+ v3.2's stage select uses 0x01-0x33, 0x37 and 0x40-0x52). The game
// plugin checks the exact list against P+'s stage select tables (game-code online_match.cpp);
// this rejects what is never a stage.
bool ValidStageKind(u32 kind);

// The host's gmGlobalModeMelee init block (0x20 bytes): both machines build it from the same
// SESSION, and only the stage variant (+0x14, Smashville's lighting from the console clock) may
// differ. Returns the block to use: `mine` with the host's variant, or `mine` unchanged when the
// host's block differs anywhere else or its variant is out of range (`*rejected` then true; the
// barrier's setup check then ends the session if the setup really differs).
constexpr size_t INIT_BLOCK_SIZE = 0x20;
constexpr size_t INIT_BLOCK_VARIANT = 0x14;
constexpr u8 MAX_STAGE_VARIANT = 0x0F;
std::vector<u8> MergeInitBlock(const std::vector<u8>& mine, const std::vector<u8>& host,
                               bool* rejected);

// Team battles recorded before 2026-10-12 (v0.1.65 and older; game-code 262b064b removed it): the
// plugin gave every player in a team battle their team's costume (teamCostume), not the one they
// locked in, and the recording's setup key has those costumes (gmPlayerInitData +0x05
// m_colorFileNo). Builds since play the locked-in costume, so playback restores the old rule's
// (docs/design/replays.md §6). A character's costume list (P+'s CSS slot table, 0x80585B00 +
// slot * 0x10, +8): per costume its colour and its file number, ending with colour 0x0C.
struct CostumeEntry
{
  u8 colour = 0;
  u8 file = 0;
};
constexpr u8 COSTUME_LIST_END = 0x0C;
constexpr size_t MAX_COSTUME_LIST = 0x20;
// The old rule: `costume` itself if it has the team's colour (the player's own shade of it), else
// the list's first costume of that colour; `costume` unchanged when the list has none.
size_t LegacyTeamCostume(const std::vector<CostumeEntry>& list, u8 team_colour, size_t costume);
// One present player of the playback's team battle: the recorded SESSION's costume and team, the
// file number the recording played (its setup key), and the costume lists of the CSS slots of the
// player's character (P+ has more than one slot for some kinds; the game's own pick among them,
// exchangeGmCharacterKind2MuSelchkind, is the one that gives the recorded file).
struct LegacyCostumePlayer
{
  bool present = false;
  u8 costume = 0;
  u8 team = 0;
  u8 recorded_file = 0;
  std::vector<std::vector<CostumeEntry>> lists;
};
struct CostumePick
{
  u8 color_no = 0;  // gmPlayerInitData +0x06 m_colorNo
  u8 file = 0;      // +0x05 m_colorFileNo
};
// Per present player the old rule's costume, when it gives every present player the recorded
// file; nullopt otherwise (a recording whose costumes the old rule does not explain: left as is,
// and the barrier's setup check refuses it). `team_colours`: the colour of each team (red, blue,
// green; 0x805A21F0). A SESSION costume with P+'s alt bits is never the old rule's (no build that
// had it had alts).
std::optional<std::array<std::optional<CostumePick>, MAX_PORTS>>
LegacyTeamCostumes(const std::array<LegacyCostumePlayer, MAX_PORTS>& players,
                   const std::array<u8, 3>& team_colours);

// The fighter start point table (4 words): the host's must be a permutation of ours.
bool IsPermutation(const std::array<u32, 4>& mine, const std::array<u32, 4>& theirs);

// One gfPadStatus (0x40 bytes, guest byte order) as GekkoNet delivers it for any player. Applied
// to every player's input on both machines (the same bytes give the same result), before the
// pad slots are written into the game:
//  - controller type (+0x3C, a u32) outside GameCube/Classic/Wii Remote/Nunchuk (0-3) -> GameCube
//    (it decides whether the record goes into the port's GameCube or Wii Remote pad);
//  - error (+0x38) outside NONE/NO_CONTROLLER/NOT_READY/TRANSFER -> NONE;
//  - the motion floats (+0x18..+0x2C) not finite or beyond +-1e6 -> 0.
constexpr size_t PAD_SIZE = 0x40;
void SanitizePad(u8* pad);

// Port values (ppom.h PortValues, 0x3C bytes) from the peer: only the flag the game knows
// (PV_TAG) is kept, the rumble byte (local only) is cleared, a picked stock count (stock
// control) above MAX_PICKED_STOCKS and a slot colour above MAX_SLOT_COLOUR are none, and of the
// controller byte only PV_WII (the player plays on a Wii Remote: their match controller is it).
// The controls layout itself is sanitised by the game (online_match.cpp layoutByte), the same on
// both machines.
constexpr u8 MAX_PICKED_STOCKS = 99;
// P+'s slot colours 0-12 as PortValues.slotColour 1-13 (ppom.h SLOT_COLOURS; 0 none).
constexpr u8 MAX_SLOT_COLOUR = 13;
template <size_t N>
void SanitizePortValues(std::array<u8, N>& pv)
{
  static_assert(N == 0x3C);
  pv[0] &= 0x01;      // flags: PV_TAG
  pv[1] = 0;          // rumble
  // The stock count picked on the STOCKS box (the rules' stock control; 0 none).
  if (pv[0x39] > MAX_PICKED_STOCKS)
    pv[0x39] = 0;
  // The slot colour picked on the CSS (L / R on the player box; 0 none).
  if (pv[0x3A] > MAX_SLOT_COLOUR)
    pv[0x3A] = 0;
  // The controller: PV_WII.
  pv[0x3B] &= 0x01;
}

// A player's special-mode picks (ppom.h PortExtra, 0x48 bytes; docs/design/special-rules.md) from
// the peer: hp (u16 at +0) above MAX_PICKED_HP is none; the All-Star roster (count at +2, then
// ROSTER_MAX entries of {gmCharacterKind, P+'s colour, the CSS's costume, CSS id} from +8, P+'s
// All-Star table's bytes) keeps its entries up to the first one the game cannot load (a
// character, colour, costume or CSS id out of range), its count at most that, and every other
// byte zero.
constexpr u16 MAX_PICKED_HP = 999;
constexpr size_t ROSTER_MAX = 16;
constexpr size_t PX_HP = 0, PX_ROSTER_COUNT = 2, PX_ROSTER = 8, PX_ENTRY = 4;
// An entry's colour byte: the costume's file, FitX<NN>.pac (P+'s costume list gives it: not the
// costume number, Falcon's costume 17 is file 44, bug #184), or P+'s AltZ / AltR mark (0x3D /
// 0x3E). Up to 99, the files' two digits.
constexpr u8 MAX_ROSTER_COLOUR = 99;
bool ValidRosterColour(u8 colour);
constexpr u8 MAX_CSS_ID = 0x3F;
template <size_t N>
void SanitizePortExtra(std::array<u8, N>& px)
{
  static_assert(N == PX_ROSTER + ROSTER_MAX * PX_ENTRY);
  std::array<u8, N> out{};
  const u16 hp = static_cast<u16>(px[PX_HP] << 8 | px[PX_HP + 1]);
  if (hp <= MAX_PICKED_HP)
  {
    out[PX_HP] = px[PX_HP];
    out[PX_HP + 1] = px[PX_HP + 1];
  }
  size_t count = 0;
  for (; count < ROSTER_MAX && count < px[PX_ROSTER_COUNT]; ++count)
  {
    const u8* e = &px[PX_ROSTER + count * PX_ENTRY];
    if (!ValidCharKind(e[0]) || !ValidRosterColour(e[1]) || e[2] > MAX_COSTUME || e[3] > MAX_CSS_ID)
      break;
    std::copy_n(e, PX_ENTRY, &out[PX_ROSTER + count * PX_ENTRY]);
  }
  out[PX_ROSTER_COUNT] = static_cast<u8>(count);
  px = out;
}

std::string TruncateName(const std::string& name);

// What two machines' setup keys (GameplaySession.cpp SetupKey, whose layout this follows: keep
// them in step) differ in, one line per field ("P2 costume 15, peer 17"), at most `max_lines`
// and then a line saying how many more. The layout is walked by `mine`'s own flags (teams, stock
// control); where the peer's differ, that field is named first and later offsets may not line up.
// Empty when the keys are the same.
std::vector<std::string> SetupKeyDiff(const std::vector<u8>& mine, const std::vector<u8>& theirs,
                                      size_t max_lines = 16);

// Where P+'s Code Menu values (4 bytes a line) start in a setup key: after the init block, the
// players (5 bytes each in a team battle), the pad layouts, the set rule (then under stock control
// each player's handicap), the hazard bit and the item switch. nullopt for a shorter key.
std::optional<size_t> SetupKeyCodeMenuAt(const std::vector<u8>& key);

// A replay's Code Menu value lines (docs/design/replays.md §6.1 step 7): what the barrier does with one
// whose value differs from the recording's. The recorder's machine had the lines its plugin wrote
// (an older plugin wrote other values: Autosave Replays ON before 2026-10-18).
enum class ReplayCodeMenuLine : u8
{
  Compared,  // read by P+'s codes in the match: must be the recording's, as between live peers
  Ours,      // never read in the match (P+'s replay autosave, the menu's activation): ours kept
  Recorded,  // only drawn (the slot colours): the recording's, written in before the match loads
};
// `slot_colour`: the line is one of the four the slot colour pointers (0x804E02F4) point at.
ReplayCodeMenuLine ClassifyReplayCodeMenuLine(std::string_view text, bool slot_colour);
// A Recorded line's values: P+'s slot colours 0-12.
constexpr u32 MAX_RECORDED_SLOT_COLOUR = MAX_SLOT_COLOUR - 1;

// The recording's setup key as playback compares it with `here` (both SetupKey; `lines`: this
// game's value lines in key order): the values of the lines that are not Compared taken from
// `here`. Unchanged when the two keys' Code Menu values do not start at the same place or either
// is too short for the lines.
std::vector<u8> ReplaySetupKey(const std::vector<u8>& here, const std::vector<u8>& recorded,
                               const std::vector<ReplayCodeMenuLine>& lines);

// Per line (`lines` as above) the recorded value to write in before the match: the Recorded lines
// whose recorded value is a slot colour; nullopt for the others or when the key is too short.
std::vector<std::optional<u32>> RecordedCodeMenuValues(const std::vector<u8>& recorded,
                                                       const std::vector<ReplayCodeMenuLine>& lines);

// Why a peer's session gave up (its "leave" message's "why"), for our logs and status: printable
// ASCII only (anything else becomes '?'), at most MAX_LEAVE_REASON_LEN characters.
constexpr size_t MAX_LEAVE_REASON_LEN = 160;
std::string LeaveReason(std::string_view why);

// Ranked's match quality (GameplaySession's UpdateMatchQuality): Slippi's poor-performance
// termination (Ishiiruka 3.6.3 and 3.6.4, CEXISlippi::handlePoorMatchPerformance). Every
// QUALITY_INTERVAL_FRAMES (2.5 s) of a game adds a debt for how slowly that interval ran and how
// high its round trip was, the worse of the two; a healthy interval pays 1 back. At
// QUALITY_ABORT_DEBT the game is aborted and the set voided, nobody rated (the user's,
// 2026-10-29). One hitch is survivable; a game at 52 frames a second (ratio 1.15) is aborted in
// about 20 s, as is one at a round trip of QUALITY_PING_MILD_MS or more.
constexpr s64 QUALITY_INTERVAL_FRAMES = 150;
constexpr int QUALITY_ABORT_DEBT = 30;
// After a minute of the game (Slippi: frame 3600) every tier is 15% further out.
constexpr s64 QUALITY_LENIENT_AFTER_FRAMES = 3600;
// Round trips, ms. Slippi's tiers are 90 / 120 / 200; ours start above Ranked's 150 ms pairing
// cap (mm, 9a6f024d), whose GeoIP estimate can be 30 ms off the real round trip, so that every
// pair matchmaking makes on purpose is playable.
constexpr double QUALITY_PING_MILD_MS = 180;
constexpr double QUALITY_PING_BAD_MS = 220;
constexpr double QUALITY_PING_SEVERE_MS = 300;
// One interval's debt (negative: paid back). `ratio`: its wall time over the 150 frames' own
// (2.5 s at 59.94 Hz); `avg_ping_ms`: its average round trip (0: none measured; the ratio then
// covers it); `lenient`: past QUALITY_LENIENT_AFTER_FRAMES.
int QualityDebt(double ratio, double avg_ping_ms, bool lenient);
}  // namespace Gprb::PeerData
