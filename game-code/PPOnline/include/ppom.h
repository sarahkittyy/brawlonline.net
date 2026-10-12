#pragma once
// Game <-> Dolphin channel (docs/backend-design.md section 5.2).
//
// One static block inside the plugin's data. Dolphin (Source/Core/Core/Online/GameBridge.cpp)
// finds it through the game's own OSModuleInfo list (0x800030C8): the module with our REL id
// (20560, Makefile RELID; do not change it), then the "PPOM" + version header in that module's
// data sections. Once per boot; afterwards it checks the magic word each frame.
// All multi-byte fields are big-endian (the game's native order). Text is UTF-16BE.
//
// Servicing (Dolphin, at the frame-end boundary, never during a netplay session):
//   - requests are consumed in order (reqRead); CLEANUP_CONNECTION has no answer;
//   - at most one response per frame, and only once the game has taken the previous one
//     (respSeen == respCount), so a response is never overwritten unread;
//   - FIND_OPPONENT is answered with a GET_MATCH_STATE payload; the game then polls
//     GET_MATCH_STATE (one outstanding poll at a time) while it searches or is connected.
//
//   MAILBOX  game writes requests, Dolphin writes responses. Only used outside matches.
//            Excluded from rollback state and desync hashes.
//   LOCAL    per machine, right after the mailbox and excluded from rollback with it: Dolphin's
//            view of this player's session (local port, opponent ready, disconnected) and the
//            game's lock-in on the online CSS (LockIn), which Dolphin sends to the opponent.
//   SESSION  the same on every machine of a session: the lobby and the setup of the next game
//            (players, stage), written by Dolphin only outside matches, so it is constant
//            (and equal on both machines) while a match runs under rollback.
//   DEBUG    plugin diagnostics (MuMsg::printIndex call log, counters). Never read by Dolphin
//            in production; the harness uses it to find message ids.
//
// The layout below is the contract; tools/gamecode/ppom.py mirrors it.

#include <types.h>

namespace PPOM {

    const u32 MAGIC = 0x50504F4D; // "PPOM"
    // 4: 3-4 player matches (docs/nplayer/setup.md): each player's team, the stage pickers, the
    // elimination order, the per-port "gone" flags and the teams switch in SESSION (0x220), the
    // lock-in's team in LOCAL.
    // 5: rooms (docs/rooms-game-interface.md): CMD_ROOM, LOCAL screen/roomJoin/room, the room view
    // in SESSION's spare bytes. No size or offset of v4 changed.
    // 6: the match's rules (docs/design/room-rules.md section 3): SESSION grows to 0x280 with Rules
    // at +0x220 and the room's ruleset and set (RoomPlay) at +0x240. No other size or offset changed.
    const u16 VERSION = 6;

    // Slippi command bytes (EXI_DeviceSlippi.h), kept for familiarity (design 5.3).
    enum Cmd {
        CMD_NONE = 0x00,
        CMD_GET_MATCH_STATE = 0xB3,
        CMD_FIND_OPPONENT = 0xB4,
        CMD_OPEN_LOGIN = 0xB6,
        CMD_UPDATE = 0xB8,
        CMD_GET_ONLINE_STATUS = 0xB9,
        CMD_CLEANUP_CONNECTION = 0xBA,
        CMD_FETCH_CODE_SUGGESTION = 0xBE,
        CMD_GET_RANK = 0xE3,
        CMD_GP_COMPLETE_STEP = 0xC0,
        CMD_GP_FETCH_STEP = 0xC1,
        CMD_ROOM = 0xD0,          // rooms: RoomRequest, answered with RoomStatus
        CMD_RULESETS = 0xD1,      // v6: the Ruleset line's list, answered with RulesetList
    };

    // Online modes, Slippi order (Online.s: Ranked, Unranked, Direct, Teams).
    enum Mode { MODE_RANKED = 0, MODE_UNRANKED = 1, MODE_DIRECT = 2, MODE_TEAMS = 3 };

    // mmState values = Slippi's ProcessState (SlippiMatchmaking.h).
    enum MmState {
        MM_IDLE = 0,
        MM_INITIALIZING = 1,
        MM_MATCHMAKING = 2,
        MM_OPPONENT_CONNECTING = 3,
        MM_CONNECTION_SUCCESS = 4,
        MM_ERROR = 5,
    };

    const int CODE_LEN = 9;   // "ABCD#123" + NUL, UTF-16
    const int NAME_LEN = 16;
    const int ERROR_LEN = 120;
    const int TAG_CHARS = 5;  // a name tag's characters (the save's tag records, PortValues)

    // 0xB4 FIND_OPPONENT request payload.
    struct FindOpponent {
        u8 mode;          // Mode
        u8 lockedChar;    // CSS id of the locked-in character (0xFF = none yet)
        u8 costume;
        u8 team;
        u16 code[CODE_LEN];   // opponent's connect code (Direct/Teams), else empty
    };

    // 0xB3 GET_MATCH_STATE response payload (also the answer to FIND_OPPONENT).
    struct MatchState {
        u8 mmState;       // MmState
        u8 role;          // 0 none, 1 host, 2 guest
        u8 sessionPhase;  // 0 none, 1 connecting, 2 transferring, 3 plugged
        u8 percent;       // transfer progress
        u16 peerName[NAME_LEN];
        u16 peerCode[CODE_LEN];
        u16 _pad;
        u16 errorText[ERROR_LEN];
    };

    // Slippi's scroll constants for FETCH_CODE_SUGGESTION (TextEntryScreen/AutoComplete.s).
    enum Scroll { SCROLL_NONE = 0, SCROLL_OLDER = 1, SCROLL_NEWER = 2, SCROLL_RESET = 3 };

    // 0xBE FETCH_CODE_SUGGESTION request payload (Slippi handleNameEntryLoad): the recent code
    // (direct-codes.json / teams-codes.json of the logged-in account) that starts with what was
    // typed. One in flight at a time.
    struct CodeSuggestionRequest {
        u8 mode;          // MODE_DIRECT or MODE_TEAMS: which history
        u8 scroll;        // Scroll: 1 older (L), 2 newer (R), 3 reset (opened, typed, deleted)
        u8 inputLen;      // committed characters
        u8 _pad;
        u32 index;        // index of the current suggestion (from the last answer; 0 before any)
        u16 input[CODE_LEN];   // the committed characters, NUL after inputLen
    };                    // 0x1C

    // 0xBE response payload (cmd 0xBE, status 0).
    struct CodeSuggestion {
        u8 found;         // 1: `code` is a recent code starting with the input (case-insensitive)
        u8 len;           // characters in `code`
        u8 _pad[2];
        u32 index;        // the suggestion's index in the history (newest = 0); else the request's
        u16 code[CODE_LEN];    // found: the whole code, upper-case ASCII; else the input echoed
    };                    // 0x1C

    // 0xE3 GET_RANK response payload (Slippi's RankInfo, ExiSlippi.h:159-167): this player's
    // rating, an Elo number (there are no rank tiers), and the change of the last ranked set.
    enum RankState { RANK_UNKNOWN = 0, RANK_FETCHING = 1, RANK_READY = 2 };
    struct RankInfo {
        u8 state;         // RankState: unknown (logged out, not read), a set's result on its way, ready
        u8 hasChange;     // 1: `change` is the last ranked set's
        u8 _pad[2];
        float rating;
        u32 setsPlayed;
        float change;
    };                    // 0x10

    // Ranked's game setup (Dolphin Online/GameSetup.h; Slippi's GP_FETCH_STEP / GP_COMPLETE_STEP):
    // stage strikes 1-2-1 for game 1, then the winner's two bans, the loser's pick and the characters
    // (winner first). 0xC0 GP_COMPLETE_STEP request payload: {u8 stage kind} (a strike or pick).
    enum StepType { STEP_NONE = 0, STEP_STRIKE = 1, STEP_PICK = 2, STEP_CHAR = 3, STEP_DONE = 4 };
    struct GameStep {     // 0xC1 GP_FETCH_STEP response payload
        u8 active;        // a ranked game setup runs (else the CSS works as in Unranked)
        u8 type;          // StepType
        u8 myTurn;        // this player strikes / picks / chooses now
        u8 count;         // selections left in the step
        u8 toSss;         // a stage step: be on the stage select
        u8 mayLock;       // CSS: START may lock in
        u8 nKinds;        // entries in kinds
        u8 seconds;       // seconds left in the step
        u8 kinds[40];     // the stages still selectable (srStageKind)
        u16 text[64];     // the line to show ("" = the game's own)
        u8 timeUp;        // CSS: lock in now with the character on the CSS (no START needed)
        u8 _pad[3];
    };                    // 0xB4

    // 0xB9 GET_ONLINE_STATUS response payload.
    struct OnlineStatus {
        u8 state;         // 0 logged out, 1 ok, 2 update required
        u8 _pad;
        u16 name[NAME_LEN];
        u16 code[CODE_LEN];
        // The name tag of the player's last lock-in (Dolphin keeps it across boots in
        // Online/last-tag.json), empty for none; still v5 (a Dolphin without it sends zeros).
        u16 lastTag[TAG_CHARS];
    };

    // ---- Rooms (docs/rooms-game-interface.md) ----

    enum RoomOp {
        ROOM_POLL = 0,      // nothing; answers the status line
        ROOM_CREATE = 1,    // arg: 1 public, 0 private
        ROOM_JOIN = 2,      // code: the room code as typed
        ROOM_LEAVE = 3,
        ROOM_SLOT = 4,      // host: arg = in-game port 0-3, arg2 = 1 open / 0 close (an occupied one: kick)
        ROOM_TEAMS = 5,     // host: arg = 1 Teams on / 0 off
        ROOM_PUBLIC = 6,    // host: arg = 1 public / 0 private
        ROOM_TEAM = 7,      // arg = this player's colour (TEAM_RED..TEAM_GREEN)
        ROOM_RULES = 8,     // v6, host: RoomRulesRequest (the room's rules as the host edited them)
        ROOM_SET = 9,       // v6, host: arg = 1 resume / 0 restart the room's paused set
        ROOM_RULESET = 10,  // v6, host: code = a ruleset code (6 letters), empty: P+'s default
        ROOM_BEST_OF = 11,  // v6, host: arg = the set's length, 0 (no set) / 3 / 5 / 7 (the room's
                            // rules become Custom); not while a set is on
    };

    // CMD_ROOM request payload.
    struct RoomRequest {
        u8 op;            // RoomOp
        u8 arg;
        u8 arg2;
        u8 _pad;
        u16 code[CODE_LEN];   // ROOM_JOIN: ASCII or full-width UTF-16, NUL-terminated
        u16 _pad2;
    };                    // 0x18

    enum RoomPhase { RP_NONE = 0, RP_JOINING = 1, RP_IN = 2 };

    // CMD_ROOM response payload (cmd CMD_ROOM, status 0), for every op.
    struct RoomStatus {
        u8 phase;         // RoomPhase
        u8 error;         // 1: `text` is an error (red)
        u8 localPort;     // as Local.localPort
        u8 _pad;
        u32 serial;       // changes whenever text, error or the room changes
        u16 text[64];     // the status line, NUL-terminated ("" = the game's own)
    };                    // 0x88

    // Local.screen (game -> Dolphin): where the game is; busy (4-7) refuses a launcher join.
    enum Screen {
        SCREEN_UNKNOWN = 0,
        SCREEN_MENUS = 1,         // menus the room CSS can be reached from
        SCREEN_ONLINE_CSS = 2,    // an online CSS, not searching
        SCREEN_ROOM = 3,          // the room CSS
        SCREEN_ONLINE_BUSY = 4,   // an online CSS searching, connecting or connected (a set)
        SCREEN_MATCH = 5,         // any match
        SCREEN_OFFLINE = 6,       // single-player, offline Versus
        SCREEN_OTHER = 7,
    };

    // SessionPlayer.roomSlot bits and Session.roomFlags.
    enum RoomSlotBits { SLOT_OPEN = 1, SLOT_TAKEN = 2, SLOT_READY = 4, SLOT_HOST = 8, SLOT_IN_GAME = 0x10 };
    enum RoomFlags { RF_IN = 1, RF_PUBLIC = 2, RF_TEAMS = 4 };
    enum RoomStatusKind { RS_WAITING = 0, RS_STARTING = 1, RS_IN_GAME = 2 };
    enum RoomMode { RM_1V1 = 0, RM_FFA = 1, RM_TEAMS = 2 };

    const int REQ_SLOTS = 4;
    const int REQ_PAYLOAD = 0x78;
    const int RESP_PAYLOAD = 0x1F8;

    struct Request {
        u32 seq;          // 1, 2, 3... (0 = empty slot)
        u8 cmd;
        u8 _pad[3];
        u8 payload[REQ_PAYLOAD];
    };                    // 0x80

    struct Response {
        u32 seq;          // seq of the request this answers (0 = none yet)
        u8 cmd;           // payload type (e.g. GET_MATCH_STATE for a FIND_OPPONENT answer)
        u8 status;        // 0 ok, else error
        u8 _pad[2];
        u8 payload[RESP_PAYLOAD];
    };                    // 0x200

    struct Mailbox {
        u32 reqWrite;     // game: number of requests posted (slot = (n-1) % REQ_SLOTS)
        u32 reqRead;      // Dolphin: number of requests consumed
        u32 respCount;    // Dolphin: incremented after each response is fully written
        u32 respSeen;     // game: last respCount it acted on
        Request req[REQ_SLOTS];
        Response resp;
    };

    // ---- Port values (design 5.1, "port values"; Orca's NameTags.h) ----

    // A player's name tag and its controls, as P+ keeps them in the save's tag records
    // (g_GameGlobal+0x28 -> records, tag i at +0xE0 + i * 0x124: name u16[5] at +0x00, rumble at
    // +0x0C, the controls layout at +0x14..+0x40: GameCube 12 bytes (L, R, Z, D-pad up, side, down,
    // A, B, C-stick, Y, X, then flags: tap jump 0x80), Wii Remote 8, with Nunchuk 12, Classic 13).
    // Each player's own values travel with their lock-in; the match applies each port's layout on
    // both machines (online_match.cpp, the ipPadConfig hook), so everyone keeps their own buttons.
    const int LAYOUT_SIZE = 0x2D;
    enum PortValueFlags { PV_TAG = 1 };   // the values are a tag's (else: the game's defaults)

    struct PortValues {
        u8 flags;         // PortValueFlags
        u8 rumble;        // the tag's rumble byte (local only; not applied online)
        u16 tag[TAG_CHARS];          // the tag's name (UTF-16, as the save keeps it)
        u8 layout[LAYOUT_SIZE];      // the tag's controls layout
        u8 _pad[3];
    };                    // 0x3C

    // ---- LOCAL ----

    // LockIn.stagePick of the player who picks the next stage (Direct's loser): locked in with
    // the character, the stage select comes once every player is locked in, and the setup waits
    // for its pick (Dolphin's GameplaySession STAGE_PENDING).
    const u16 STAGE_PENDING = 0xFFFE;

    // The player's lock-in on the online CSS (game -> Dolphin). Slippi's
    // MSRB_IS_LOCAL_PLAYER_READY plus its SET_MATCH_SELECTIONS payload.
    struct LockIn {
        u32 seq;          // game: bumped on every change (0 = never written)
        u8 ready;         // 1 = locked in for `game`
        u8 cssChar;       // CSS id (diagnostics)
        u8 charKind;      // gmCharacterKind (random resolved)
        u8 costume;       // colour number
        u16 stagePick;    // stage kind picked on the stage select (Direct loser), 0xFFFF none,
                          // STAGE_PENDING: the loser locked in, its stage select still to come
        u8 asl;           // P+ alternate-stage buttons held for that pick
        u8 game;          // game number (1-based) this lock-in is for
    };                    // 0x0C

    enum LocalState { LS_NONE = 0, LS_CONNECTING = 1, LS_CONNECTED = 2, LS_IN_MATCH = 3 };

    struct Local {
        u32 seq;          // Dolphin: bumped after each update
        u8 state;         // LocalState
        u8 localPort;     // in-game port of this player (0 = P1), 0xFF none
        u8 remoteReady;   // the opponent is locked in for the next game
        u8 disconnected;  // the opponent left or went silent (Slippi ONLINE_INPUTS result 3)
        u16 peerName[NAME_LEN];
        LockIn lockIn;    // written by the game
        u8 lockTeam;      // written by the game with the lock-in: the player's team colour
                          // (TEAM_RED..TEAM_GREEN) for a team battle, TEAM_NONE none
        u8 lockPad;       // game: the controller port (0-3) whose START locked in, with the
                          // lock-in (its seq covers it); the match reads that controller
        u8 screen;        // game: Screen, every frame (rooms: launcher joins, game-status.json)
        u8 roomJoin;      // Dolphin: bumped when it accepted a launcher join and sent it: go to
                          // the room CSS
        u8 room;          // Dolphin: RoomPhase
        // The launcher's Direct invite (docs/rooms-game-interface.md, still v5: these were
        // reserved): Dolphin bumps directJoin when it accepted one (and left any room); the game
        // then goes to the Direct CSS with the other player's code, directPrefix (2-4 ASCII
        // letters, upper case, NUL-padded) + '#' + directNumber (1-9999), and searches it on
        // START without the keypad. A zero prefix: a plain Direct CSS.
        u8 directJoin;
        char directPrefix[4];
        u16 directNumber;
        PortValues own;   // written by the game with the lock-in (its seq covers both)
        u8 hudDisconnected;  // game: 1 once it draws DISCONNECTED in the match (Dolphin's OSD
                             // stands in only when the game does not); 0 at each match setup
        u8 desynced;      // Dolphin: the session ended the match because the two machines'
                          // game states differ, until the match scene is left
        u8 roomCreate;    // Dolphin: bumped when it accepted a launcher create (still v5, was
                          // reserved) and sent room-create itself (after leaving any room): go to
                          // the room CSS as WITH FRIENDS > Create Room, without a ROOM_CREATE
        u8 quitLeft;      // Dolphin: 1 = this player left a 3-4 player match with the pause
                          // screen's quit (Session.quitPorts) and the session ended for this
                          // machine only; the game ends its match (no DISCONNECTED). Until the
                          // next match starts
    };                    // 0x80

    // ---- SESSION ----

    const int SESSION_PLAYERS = 4;   // by in-game port; 2 to 4 present, gaps allowed (P1, P3)

    // Team colours (Brawl's team battle: gmPlayerInitData+0x0B; Slippi's order).
    enum Team { TEAM_RED = 0, TEAM_BLUE = 1, TEAM_GREEN = 2, TEAM_COUNT = 3, TEAM_NONE = 0xFF };

    struct SessionPlayer {
        u8 present;
        u8 charKind;      // gmCharacterKind
        u8 costume;       // colour number
        u8 team;          // Team in a team battle (Session.teams), TEAM_NONE in a free-for-all
        u16 name[NAME_LEN];
        u16 code[CODE_LEN];
        u8 picksStage;    // Dolphin: 1 = this player picks the next game's stage on the stage
                          // select (the last game's loser; both after a 1v1 draw)
        u8 out;           // game, every frame of a match: the order in which this port was
                          // eliminated (1 = first out; ports out on the same frame share it),
                          // 0 = still in. Dolphin reads it at the game's end for the placings.
        // Rooms (Dolphin, while in a room and no match runs): the room's view of this slot.
        u8 roomSlot;      // RoomSlotBits
        u8 roomTeam;      // the player's colour (TEAM_*), TEAM_NONE empty
        u8 roomChar;      // gmCharacterKind of the player's lock-in while SLOT_READY, else 0xFF
        u8 roomCostume;
        u8 _pad2[4];
        PortValues pv;    // the player's name tag and controls (from their lock-in)
        u32 _pad3;
    };                    // 0x80

    enum SessionState { SS_NONE = 0, SS_LOBBY = 1, SS_MATCH_READY = 2 };

    // The match's rules (Dolphin, with the rest of SESSION: the server's get-ticket-resp `rules`,
    // checked by Dolphin's Online/MatchRules). OnlineMenu::applyRules puts them into the match;
    // without them (valid 0: an older server) it plays P+'s competitive set.
    enum RuleStyle { STYLE_TIME = 0, STYLE_STOCK = 1, STYLE_COIN = 2 };   // gmSetRule::m_rule
    struct Rules {
        u8 valid;         // 1: the session's rules
        u8 style;         // RuleStyle
        u8 stocks;        // 1-99
        u8 timeMinutes;   // MORE RULES' time limit, every style (stock: 0 none)
        u8 damageRatio;   // times 10 (5-20)
        u8 teamAttack;
        u8 pause;         // 1 NORMAL, 0 HOLD (P+: START held 50 frames)
        u8 itemFrequency; // 0 none .. 3 high
        u8 itemSwitch[8]; // the ITEM SWITCH screen's switches (the menu record's +0x818)
        u8 hazards;       // 1 on
        u8 autoLCancel;   // MORE RULES' AUTO L-CANCEL (set rule +0xB)
        u8 inputAssist;   // MORE RULES' INPUT ASSIST (set rule +0xC, P+'s 3-frame buffer)
        u8 _reserved[13];
    };                    // 0x20

    // CMD_RULESETS response payload (v6): the player's recent and own published rulesets, for the
    // room's Ruleset line (Dolphin asks mm again when its list is older than 5 s).
    enum ListState { LIST_NONE = 0, LIST_LOADING = 1, LIST_READY = 2, LIST_FAILED = 3 };
    struct RulesetListEntry {
        char code[8];     // 6 letters, NUL padded
        char name[24];    // printable ASCII, NUL ended
    };
    struct RulesetList {
        u8 state;         // ListState
        u8 count;
        u16 serial;       // changes with every new list
        RulesetListEntry entries[15];
    };                    // 0x1E4

    // CMD_ROOM payload for ROOM_RULES (v6): the room's rules as the host left P+'s rules menu,
    // in SESSION's layout (`valid` 1). Dolphin checks every value and sends room-rules.
    // On the room CSS (Dolphin, from room-state; docs/design/room-rules.md): what the room plays and
    // its set, for the room CSS to show (the ruleset's name, the score) and the host's set controls.
    enum StageChoice { CHOICE_LOSER_PICKS = 0, CHOICE_STRIKE = 1, CHOICE_RANDOM = 2 };
    enum SetFlags { SET_PAUSED = 1, SET_CAN_RESUME = 2, SET_FINISHED = 4 };
    struct RoomPlay {
        u8 valid;         // 1: in a room
        u8 bestOf;        // the ruleset's: 0 no set, 3 / 5 / 7
        u8 stageChoice;   // StageChoice (no set)
        u8 custom;        // 1: custom rules (edited by the host)
        char code[8];     // the published ruleset's code (6 letters), NUL padded; "" none
        char name[32];    // its name ("P+ default", "Custom", ...), printable ASCII, NUL ended
        u8 setOn;         // 1: the room has a set (on, paused or finished)
        u8 setGame;       // the set's game being played or next
        u8 score[2];      // wins by side (side 0: the lower slot)
        u8 sides[2];      // each side's in-game port
        u8 setFlags;      // SetFlags
        u8 sideSlots[2];  // each side's room slots (bit s = slot s), 0 without a set
        u8 winner;        // the winning side (0 / 1) once the set is finished, else 0xFF
        u8 _reserved[10];
    };                    // 0x40

    struct RoomRulesRequest {
        u8 op;            // ROOM_RULES
        u8 _pad[3];
        Rules rules;
    };                    // 0x24

    // Why the next game cannot be set up (Session.setupError; the CSS shows the text).
    enum SetupError {
        SE_NONE = 0,
        SE_SAME_TEAM = 1,   // "Pick different teams": a team battle with everyone on one colour
        SE_NO_TEAM = 2,     // "Pick a team": a team battle and a player without a colour
    };

    struct Session {
        u32 seq;          // Dolphin: bumped after each update
        u8 state;         // SessionState
        u8 mode;          // Mode of the search that made the session
        u8 game;          // the next game (1-based); its setup once state == SS_MATCH_READY
        u8 lastWinner;    // in-game port of the last game's winner; 0xFE draw, 0xFF none
        u16 stageKind;    // stage of the next game (srStageKind)
        u8 asl;           // P+ alternate-stage buttons for it
        u8 numPlayers;    // the highest present port + 1 (ports below it may be empty)
        SessionPlayer players[SESSION_PLAYERS];   // by in-game port (P1 = the host/decider)
        // A player who dropped out of a running match: Dolphin's session writes gone[port] = 1
        // at the start of every frame from the agreed frame on (also in resimulated frames,
        // like an input; never cleared during the match). This block is part of the rolled-back
        // state, so a rollback to before that frame takes the flag back with it. At the start
        // of each frame the game removes that port's fighter (OnlineMatch::tickMatch).
        u8 gone[SESSION_PLAYERS];
        u8 teams;         // 1: the next game is a team battle (each player's `team`)
        u8 setupError;    // SetupError: the setup of the next game is refused
        u8 outCount;      // game, during a match: the highest `out` given so far
        // Rooms (Dolphin, while in a room and no match runs; docs/rooms-game-interface.md).
        // `game` is then the room's next game number (lock in with it), also with state SS_NONE,
        // and players[i].name/code are the room members'.
        u8 roomFlags;     // RoomFlags
        char roomCode[4]; // ASCII, no NUL
        u8 roomHost;      // the host's in-game port, 0xFF none
        u8 roomStatus;    // RoomStatusKind
        u8 roomMode;      // RoomMode
        // A 3-4 player match (game, every frame; rolled back with the match, cleared at its
        // setup): the pause screen's quit (L+R+A+START) takes only the player who paused out of
        // the match. Bit p: port p quit. Its fighter leaves as a dropped player's does, and its
        // Dolphin leaves the session once the quit's frame is final (LOCAL.quitLeft). A quit that
        // leaves one player or team in ends the game by itself (Brawl's game set); that player
        // stays in the session until it ends: bit p of quitStays too.
        u8 quitPorts;     // 0x21B
        u8 quitStays;     // 0x21C
        // A quit that ends the match for everyone (a 1v1, or a 2v2 set; game, in the frame of the
        // quit, rolled back with the match, cleared at its setup; still v6, these were reserved):
        // quitBy = the quitter's in-game port + 1 (0 none), quitRunback = 1 when L+R+Y were held
        // with it (P+'s salty runback, L+R+A+Y+START). Dolphin decides what the quit means: Ranked
        // a lost game, a room's set a game that did not happen (a runback: played again on the
        // same stage), Unranked and Direct a runback restarts the same match.
        u8 quitBy;        // 0x21D
        u8 quitRunback;   // 0x21E
        u8 _reserved[1];
        Rules rules;      // 0x220 (v6)
        RoomPlay play;    // 0x240 (v6)
    };                    // 0x280

    // The last MuMsg::printIndex calls (a ring). 16, not 32: the plugin's Syringe heap block has
    // no room for more (docs/game-code.md, heap budget).
    const int DEBUG_LOG = 16;
    struct PrintLog {
        u32 lr;           // caller of MuMsg::printIndex
        u32 msg;          // MuMsg*
        u16 window;
        s16 line;
        u32 data;         // msbin pointer
    };

    struct Debug {
        u32 frames;       // per-frame tick count (gfSceneManager::process)
        u32 printCount;   // number of MuMsg::printIndex calls seen
        u32 overrides;    // number of texts replaced
        u32 lastError;
        u32 cfg;          // feature flags, see Cfg (harness may write for experiments)
        u32 menuState;    // plugin's online-menu state machine (OnlineMenu::State)
        u32 scratch[16];
        PrintLog log[DEBUG_LOG];
        // Tests (CFG_TEST_GONE): from game frame testGoneFrame on (g_GameFrame's frame counter,
        // rolled back with the match), the ports in the bit mask testGonePorts & 0xF are set gone,
        // as Dolphin's session does for a dropped player, and (testGonePorts >> 16) frames later
        // the ports in (testGonePorts >> 8) & 0xF. Written before the match only (this block is
        // rolled back with it).
        u32 testGoneFrame;
        u32 testGonePorts;
    };

    enum Cfg {
        CFG_TEXT = 1 << 0,        // relabel texts
        CFG_WIFI_HOOKS = 1 << 1,  // fake Nintendo WFC/Wiimmfi login (Brawlback Gen 1 NetMenu hooks)
        CFG_LOG = 1 << 2,         // log MuMsg::printIndex calls
        CFG_CSS_AUTOWIDTH = 1 << 3, // shrink the CSS status line to fit (experiment)
        CFG_SSS_LEGAL = 1 << 4,   // every stage select offers only the legal stages (debug; for Ranked strikes later)
        CFG_TEST_DISCONNECT = 1 << 5, // tests: act as if disconnected in the next scMelee frame (cleared when seen)
        CFG_TEST_RULES = 1 << 6,  // tests: the online ruleset keeps the set rule's stocks and times
                                  // (short games; both machines must have written the same)
        CFG_TEST_OPEN_RULES = 1 << 8,  // tests: open the room's rules menu on the room CSS, as
                                       // the RULES button does (cleared when seen)
        CFG_TEST_GONE = 1 << 7,   // tests: Debug.testGone* set SESSION's gone flags, and the
                                  // in-match removal and elimination order also run in a local
                                  // Versus match (a sync test)
    };

    struct Block {
        u32 magic;
        u16 version;
        u16 size;             // sizeof(Block)
        u16 mailboxOff, mailboxSize;
        u16 sessionOff, sessionSize;
        u16 localOff, localSize;
        u16 debugOff, debugSize;
        u32 _reserved[3];     // header = 0x24 bytes
        Mailbox mailbox;
        Local local;          // must follow the mailbox (one range excluded from rollback)
        Session session;
        Debug debug;
    };

    // The lock-in (LOCAL): write it and bump its sequence.
    void writeLockIn(bool ready, u8 cssChar, u8 charKind, u8 costume, u16 stagePick, u8 asl, u8 game,
                     const PortValues* pv, u8 pad);

    extern Block g_block;

    // Game side API.
    u32 post(u8 cmd, const void* payload, u32 size);    // returns seq
    const Response* pollResponse();                     // new response since last poll, or NULL
    const Response* lastResponse();

    // UTF-16 helpers.
    void asciiToU16(u16* dst, const char* src, int max);
    void u16ToAscii(char* dst, const u16* src, int max);
}
