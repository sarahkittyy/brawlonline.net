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
    const u16 VERSION = 3;

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

    // 0xB9 GET_ONLINE_STATUS response payload.
    struct OnlineStatus {
        u8 state;         // 0 logged out, 1 ok, 2 update required
        u8 _pad;
        u16 name[NAME_LEN];
        u16 code[CODE_LEN];
    };

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
    const int TAG_CHARS = 5;
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

    // The player's lock-in on the online CSS (game -> Dolphin). Slippi's
    // MSRB_IS_LOCAL_PLAYER_READY plus its SET_MATCH_SELECTIONS payload.
    struct LockIn {
        u32 seq;          // game: bumped on every change (0 = never written)
        u8 ready;         // 1 = locked in for `game`
        u8 cssChar;       // CSS id (diagnostics)
        u8 charKind;      // gmCharacterKind (random resolved)
        u8 costume;       // colour number
        u16 stagePick;    // stage kind picked on the stage select (Direct loser), 0xFFFF none
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
        u32 _reserved[3];
        PortValues own;   // written by the game with the lock-in (its seq covers both)
        u8 hudDisconnected;  // game: 1 once it draws DISCONNECTED in the match (Dolphin's OSD
                             // stands in only when the game does not); 0 at each match setup
        u8 _reserved2[3];
    };                    // 0x80

    // ---- SESSION ----

    const int SESSION_PLAYERS = 4;   // a 1v1 session uses two; four for code-based Teams later

    struct SessionPlayer {
        u8 present;
        u8 charKind;      // gmCharacterKind
        u8 costume;       // colour number
        u8 _pad;
        u16 name[NAME_LEN];
        u16 code[CODE_LEN];
        u8 _pad2[0x0A];
        PortValues pv;    // the player's name tag and controls (from their lock-in)
        u32 _pad3;
    };                    // 0x80

    enum SessionState { SS_NONE = 0, SS_LOBBY = 1, SS_MATCH_READY = 2 };

    struct Session {
        u32 seq;          // Dolphin: bumped after each update
        u8 state;         // SessionState
        u8 mode;          // Mode of the search that made the session
        u8 game;          // the next game (1-based); its setup once state == SS_MATCH_READY
        u8 lastWinner;    // in-game port of the last game's winner; 0xFE draw, 0xFF none
        u16 stageKind;    // stage of the next game (srStageKind)
        u8 asl;           // P+ alternate-stage buttons for it
        u8 numPlayers;
        SessionPlayer players[SESSION_PLAYERS];   // by in-game port (P1 = the host/decider)
        u32 _reserved;
    };                    // 0x210

    const int DEBUG_LOG = 32;
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
                     const PortValues* pv);

    extern Block g_block;

    // Game side API.
    u32 post(u8 cmd, const void* payload, u32 size);    // returns seq
    const Response* pollResponse();                     // new response since last poll, or NULL
    const Response* lastResponse();

    // UTF-16 helpers.
    void asciiToU16(u16* dst, const char* src, int max);
    void u16ToAscii(char* dst, const u16* src, int max);
}
