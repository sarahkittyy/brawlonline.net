"""Game knowledge for Project+ v3.2 on Super Smash Bros. Brawl NTSC-U Rev 1 (RSBE01).

This module is stdlib-only and has two halves:

* Pure parsers that take a ``read_mem(addr, length) -> bytes`` callable and turn raw emulated
  memory into scene / match / menu state. They never write memory and never block. They are
  unit-tested offline against synthetic memory (``harness/tests/test_brawl.py``).
* Recipes that drive a ``Driver`` (see the Protocol below; ``ppharness.client.HarnessClient``
  satisfies it) from boot to a 1v1 Versus match with closed-loop controller input.

Every address carries a provenance tag in ``ADDRESSES`` (and in ``docs/brawl-memory-map.md``):

* ``dol-verified``  checked against the bytes of the Rev 1 ``main.dol`` (or a Rev 1 ``.rel``,
                    which are byte-identical to Rev 2's).
* ``decomp``        from ``brawl-decomp`` ``config/RSBE01_02/symbols.txt`` (Rev 2 symbols; Rev 1
                    shares the function layout).
* ``headers``       a struct offset from BrawlHeaders / BrawlHeaders-sammi (reverse engineered).
* ``orca``/``brawlback``/``pplus``  taken from that project's code (Orca's were found live under
                    its harness on Rev 2 + P+ v3.2; P+'s are hard-coded in its v3.2 codeset).
* ``live``          a heap object or field that only exists at runtime: NEEDS LIVE VERIFICATION.

Sources (all under ``refs/``): orca-netplay (Harness.cpp, Orca/UX/*.cpp|h, Data/Sys/Orca/*,
Tools/orca/inputs/*), Project-Plus-Dolphin-brawlback (Rollback/RollbackManager.cpp,
HLE/HLE_Misc.cpp), brawlback-asm, BrawlHeaders(-sammi), brawl-decomp, and the P+ v3.2 codeset
source on the SD card (``Project+/Source/**``, ``NETPLAY.TXT``).
"""
from __future__ import annotations

import enum
import math
import struct
import zlib
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, Iterable, List, Mapping, Optional, Protocol, Sequence, Tuple

ReadMem = Callable[[int, int], bytes]

# =============================================================================================
# Address registry
# =============================================================================================


@dataclass(frozen=True)
class Addr:
    addr: int
    what: str
    source: str
    verify: str  # "dol-verified" | "decomp" | "headers" | "orca" | "brawlback" | "pplus" | "live"


ADDRESSES: Dict[str, Addr] = {}


def _a(name: str, addr: int, what: str, source: str, verify: str) -> int:
    ADDRESSES[name] = Addr(addr, what, source, verify)
    return addr


# ---- Globals in the DOL's .sbss (pointers whose *values* are runtime) -----------------------
# r13 (SDA base) = 0x805A4420: __init_registers @0x8000422C `lis r13,0x805A; ori r13,r13,0x4420`.
SCENE_MANAGER_PTR = _a(
    "SCENE_MANAGER_PTR", 0x805A0060, "gfSceneManager* (getInstance @0x8002D018 = lwz r3,-0x43C0(r13))",
    "decomp lbl_805A0060; orca Harness.cpp:199; brawlback RollbackManager.cpp:335", "dol-verified")
GAME_GLOBAL_PTR = _a(
    "GAME_GLOBAL_PTR", 0x805A00E0, "GameGlobal* g_GameGlobal (-0x4340(r13))",
    "decomp symbols.txt:30675; P+ BootToCSS.asm", "dol-verified")
PAD_SYSTEM_PTR = _a(
    "PAD_SYSTEM_PTR", 0x805A0040, "gfPadSystem* g_gfPadSystem", "decomp symbols.txt:30651", "decomp")
APPLICATION_PTR = _a(
    "APPLICATION_PTR", 0x8059FFAC, "gfApplication* g_gfApplication", "decomp symbols.txt:30624", "decomp")
MTRAND_DEFAULT = _a(
    "MTRAND_DEFAULT", 0x805A00B8, "mtRand g_mtRand {vtable, s32 seed @+4} (8 bytes)",
    "decomp symbols.txt:30669 + splits.txt; BrawlHeaders mt_prng.h", "decomp")
MTRAND_OTHER = _a(
    "MTRAND_OTHER", 0x805A0420, "mtRand g_mtRandOther (8 bytes)", "BrawlHeaders RSBE01.lst:26", "decomp")
HEAP_INFOS = _a(
    "HEAP_INFOS", 0x80494958, "HeapInfo g_HeapInfos[0x48] {name*, gfMemoryPool*, size, arena}",
    "decomp symbols.txt:27842 (25 lis/addi refs in Rev1 DOL); BrawlHeaders gf_memory_pool.h", "dol-verified")
# gfApplication+0x100 is incremented once per main-loop iteration: 0x800174FC lwz r3,0x100(r23);
# addi r0,r3,1; 0x80017504 stw r0,0x100(r23) (r23 = this from mainLoop @0x80016EE0). That store is
# the frame hook Orca and Brawlback use (FrameHookWord 0x90170100).
APP_FRAME_COUNTER_OFF = 0x100
FRAME_HOOK = _a("FRAME_HOOK", 0x80017504, "end of the game loop: stw r0,0x100(r23)",
                "orca PPLUS32.ini FrameHook; brawlback HLE.cpp:126", "dol-verified")
CSS_SLOT_TABLE_VANILLA = _a(
    "CSS_SLOT_TABLE_VANILLA", 0x80455458,
    "16-byte CSS slot entries indexed by MuSelchkind; byte0 = gmCharacterKind, byte1 = alt "
    "(0x0C Bowser -> 0x2C Giga, 0x15 Wario -> 0x2D WarioMan). P+ copies it to 0x80585B00",
    "Rev1 main.dol bytes; P+ ProjectM/CSS.ASM 'Move CSS Slots'", "dol-verified")

# ---- Heap objects at fixed addresses in P+ v3.2 (hard-coded by P+'s own codes) -------------
FT_ENTRY_MANAGER = _a(
    "FT_ENTRY_MANAGER", 0x80624780, "ftEntryManager {ftEntry* entries @0, u32 count @4} (System heap)",
    "P+ Community/FSMeter.asm; brawlback RollbackManager.cpp:278 / EXIBrawlback.h:145", "live")
FT_MANAGER = _a(
    "FT_MANAGER", 0x80629A00, "ftManager (System heap)", "P+ FSMeter.asm, IC-Basics.asm", "live")
G_FT_ENTRY_MANAGER_PTR = _a(
    "G_FT_ENTRY_MANAGER_PTR", 0x80B87C48, "sora_melee .bss g_ftEntryManager (should hold 0x80624780)",
    "P+ NETPLAY.TXT .alias; decomp rels/sora_melee bss+0x2E88 with the module at 0x8070A940", "live")
GAME_FRAME = _a(
    "GAME_FRAME", 0x901812A0, "GameFrame {+4 frameCounter, +0xC frameDelta, +0x14 persistentFrameCounter}",
    "BrawlHeaders RSBE01.lst:21 g_GameFrame; brawlback RollbackManager.cpp:28", "live")
SC_MELEE_OBJ = _a(
    "SC_MELEE_OBJ", 0x90FF50C0, "scMelee object (also reachable as the current scene in a match)",
    "BrawlHeaders RSBE01.lst:22", "live")
GAME_GLOBAL_OBJ = _a("GAME_GLOBAL_OBJ", 0x90181300, "*g_GameGlobal", "orca Results.h:24", "live")
MODE_MELEE_OBJ = _a("MODE_MELEE_OBJ", 0x90180F20, "gmGlobalModeMelee", "orca Results.h; BrawlHeaders RSBE01.lst:24", "live")
RESULT_INFO_OBJ = _a("RESULT_INFO_OBJ", 0x9017F420, "gmResultInfo", "orca Results.h", "live")
SET_RULE_OBJ = _a("SET_RULE_OBJ", 0x9017F360, "gmSetRule (P+ 'Default Settings Modifier' writes it)",
                  "orca Results.h; P+ NETPLAY.TXT:353", "live")
RECORD_MENU_DATA = _a("RECORD_MENU_DATA", 0x9017BE50, "gmGlobalRecord+0x810 menu data (+0 item frequency)",
                      "orca RSBE01.patches / OnlineRules.cpp:59", "live")

# ---- P+ codeset data (static addresses in the codeset/.bss area) --------------------------
RSS_EXDATA = _a("RSS_EXDATA", 0x8042C4E8, "P+ stage-switch data (0x320 bytes, loaded from pf/stage/switch/SwitchFF.rss on netplay)",
                "P+ Source/Netplay/Net-Random.asm; orca RankedPPlus.h", "pplus")
RSS_PAGES = _a("RSS_PAGES", 0x8042C524, "STAGE_PAGES: per page {u8 count, u8 slot[39]}, stride 0x28, 5 pages",
               "P+ Net-Random.asm", "pplus")
RSS_SLOT_KINDS = _a("RSS_SLOT_KINDS", 0x8042C5EC, "slot -> {u8 stage kind, u8 cosmetic} (STAGE_SLOTS_COSMETIC)",
                    "P+ Net-Random.asm; orca RankedPPlus.cpp:71", "pplus")
SSS_CURRENT_PAGE = _a("SSS_CURRENT_PAGE", 0x80496000, "P+ CURRENT_PAGE (u8)", "P+ Net-Random.asm", "pplus")
CSS_ROSTER = _a("CSS_ROSTER", 0x80680DE0, "P+ CSS icon order: 43 MuSelchkind bytes",
                "P+ ProjectM/CSS.ASM 'CSS Roster Data v3&K'", "pplus")
CODE_MENU_BASE = _a("CODE_MENU_BASE", 0x804E0000, "P+ Code Menu data (pf/menu3/dnet.cmnu, 0x2520 bytes)",
                    "Brawlback memLocations.txt (data.cmnu read to 0x804e0000); SD card", "pplus")
CODE_MENU_SIZE = 0x2520
CODE_MENU_STATE = _a("CODE_MENU_STATE", 0x804E0034, "Code Menu state word (4 = menu drawn)",
                     "P+ Net-CodeMenu.asm 'Print Code Menu' (cmpwi r31,4)", "live")
DEBUG_FLAGS = _a("DEBUG_FLAGS", 0x80583FF4, "P+ debug bytes 0x80583FF4..FFF (Code Menu copies its Debug Mode "
                 "lines here every frame)", "P+ Net-CodeMenu.asm:1167-1190, Debug/modifiedDebug.asm", "pplus")
DEBUG_MODE_HALF = _a("DEBUG_MODE_HALF", 0x80583FFE, "u16, bit 0 = Debug Mode on (byte 0x80583FFF)",
                     "P+ Debug/modifiedDebug.asm 'Debug Start Input'", "pplus")
BOOT_PADS = _a("BOOT_PADS", 0x805BA684, "pads BootToCSS reads at sqBoot::setNext (+0x40*port, u32 buttons)",
               "P+ Project+/BootToCSS.asm", "live")

# ---- Struct offsets -------------------------------------------------------------------------
# gfSceneManager (BrawlHeaders gf_scene.h): prev/current/next scene @0/4/8, prev/current/next
# sequence @0xC/0x10/0x14; gfScene / gfSequence: name char* @0.
SM_CURRENT_SCENE = 0x04
SM_NEXT_SCENE = 0x08
SM_CURRENT_SEQUENCE = 0x10
SM_PROCESS_STEP = 0x288
# GameGlobal (gm_global.h)
GG_MODE_MELEE = 0x08
GG_SEL_CHAR_DATA = 0x10
GG_SEL_STAGE_DATA = 0x14
GG_RESULT_INFO = 0x18
GG_SET_RULE = 0x1C
GG_RECORD = 0x24
# gmGlobalModeMelee (gm_global_mode_melee.h): gmMeleeInitData @0x08, players[7] @0x98 x 0x5C
MM_INIT = 0x08
MM_STAGE_KIND = 0x1A          # u16 (init+0x12); Orca reads the low byte at +0x1B
MM_ITEM_FREQUENCY = 0x16      # u8 (init+0x0E)
MM_TIME_LIMIT_FRAMES = 0x20   # s32 (init+0x18); orca Results.h "+0x20 time limit in frames"
MM_PLAYERS = 0x98
MM_PLAYER_SIZE = 0x5C
MM_SIZE = 0x320
# gmPlayerInitData
PI_CHARACTER = 0x00  # gmCharacterKind
PI_STATE = 0x01      # 0 human, 1 CPU, 3 none (orca OnlineRules.cpp:36, pplus-cpu-record-probe.txt)
PI_STOCKS = 0x04
PI_COLOR = 0x05
PI_CONTROLLER = 0x07
PLAYER_HUMAN, PLAYER_CPU, PLAYER_NONE = 0, 1, 3
# gmSelCharData: players[7] @0xB8 (same gmPlayerInitData layout)
SCD_PLAYERS = 0xB8
# gmSetRule (gm_set_rule.h; orca OnlineRules.cpp:43)
RULE_MODE = 0x02         # low 3 bits: 0 time, 1 stock, 2 coin
RULE_TIME_MINUTES = 0x03
RULE_STOCKS = 0x04
RULE_HANDICAP = 0x05
RULE_DAMAGE_RATIO = 0x06  # x10
RULE_STAGE_CHOICE = 0x07
RULE_STOCK_MINUTES = 0x08
RULE_TEAM_ATTACK = 0x09
RULE_PAUSE = 0x0A
RULE_MENU_PAD = 0x24     # P+ BootToCSS: gmGetMenuDecisionPad reads this
# gmResultInfo (gm_result_info.h; orca Results.h)
RI_RULE = 0x01
RI_STAGE = 0x0C          # u16
RI_NUM_WINNERS = 0x0F
RI_WINNING_PLAYER = 0x1F
RI_PLAYERS = 0x24
RI_PLAYER_SIZE = 0x2AC
RI_DECISION = 0x1378     # 1 time up, 2 win, 3 team win, 9 no contest
# scMelee (sc_melee.h) / stOperatorRule(Melee)
SCM_OPERATOR_READY_GO = 0x48
SCM_OPERATOR_RULE_MELEE = 0x58
OPR_IS_GAME_SET = 0x74
OPR_IS_START = 0xDF
OPR_REMAINING_FRAMES = 0xE0
OPR_FRAMES_ELAPSED = 0xE4
OPR_FRAME_COUNTER = 0xE8
OPR_DECISION = 0x254
# ftEntry (ft_entry.h, size 0x244); ftOwner / ftOwnerData; Fighter; modules
FTE_SIZE = 0x244
FTE_ENTRY_ID = 0x04
FTE_ACTIVE_INSTANCE = 0x0A   # u8 (P+ FSMeter.asm uses it the same way)
FTE_OWNER = 0x28
FTE_INSTANCES = 0x30         # {ftKind u32, Fighter*} x 4
FTE_PLAYER_NO = 0x58
FTE_CHARACTER_KIND = 0x5C
OWNER_DATA = 0x00
OWNER_DAMAGE = 0x24          # f32: ftOwner getDamage = sora_melee .text+0x111850 `lwz r3,0(r3); lfs f1,0x24(r3)`
OWNER_STOCKS = 0x34          # s32: getStockCount = .text+0x111B2C `lwz r3,0(r3); lwz r3,0x34(r3)`
FIGHTER_ACCESSER = 0x60      # StageObject::m_moduleAccesser (soExternalValueAccesser::getLr @.text+0x8CAB4)
FIGHTER_KIND = 0x110         # ftKind (P+ DoubleCherry.asm: lwz 0x110(newFighter))
ACC_ENUMERATION = 0xD8       # soModuleAccesser -> soModuleEnumeration* (verified in getLr/getStatusKind)
ENUM_MOTION = 0x08
ENUM_POSTURE = 0x0C          # verified (getLr)
ENUM_DAMAGE = 0x38           # P+ DoubleCherry.asm
ENUM_STATUS = 0x70           # verified (getStatusKind @.text+0x8CBF4)
ENUM_KINETIC = 0x7C
POSTURE_POS = 0x0C           # Vec3f; Brawlback reads x @+0xC, y @+0x10
POSTURE_PREV_POS = 0x18
POSTURE_LR = 0x40
STATUS_KIND = 0x34
MOTION_FRAME = 0x40
MOTION_KIND = 0x58
# CSS: scSelctCharacter scene +0x400 -> muSelCharTask; +0x44 + 4*port -> muSelCharPlayerArea
CSS_SCENE_TASK = 0x400
CSS_TASK_AREAS = 0x44
AREA_HAND = 0x1A8
AREA_KIND = 0x1B4            # 0 empty, 1 human, 2 CPU (orca pplus-cpu-record-probe.txt)
AREA_CHARACTER = 0x1B8       # MuSelchkind under the hand while held, else the token's (0x28 none)
AREA_CONTROLLER = 0x1DC
AREA_IN_HAND = 0x1F8         # u8
AREA_FLYING = 0x1F9          # u8
HAND_TARGET = 0x80           # orca OnlineRules.h:84-98
HAND_X = 0x90
HAND_Y = 0x94                # y up
HAND_BUTTON = 0xAC
HAND_PANEL = 0xB0
CSS_HAND_NOTHING, CSS_HAND_BUTTON, CSS_HAND_GRID, CSS_HAND_OWN_TOKEN, CSS_HAND_EXIT = 0, 1, 2, 3, 4
CSS_HAND_TAKING, CSS_HAND_GRID_HOLDING, CSS_HAND_PLACING = 6, 7, 8
CSS_AREA_EMPTY, CSS_AREA_HUMAN, CSS_AREA_CPU = 0, 1, 2
# SSS: scSelStage scene +0x3AC -> muSelectStageTask (orca BrawlStages.h/.cpp, RankedPPlus.h)
SSS_SCENE_TASK = 0x3AC
SSS_CURSOR = 0x200
CURSOR_X = 0x3C
CURSOR_Y = 0x40              # y up
SSS_STATE = 0x224            # 0 choosing, 2 taken (screen exits ~6 frames later)
SSS_PAGE = 0x228
SSS_HOVERED_ITEM = 0x244     # 0 nothing, stage items, 0x35 page button, 0x36 random, 0x37 back
SSS_SELECTED = 0x248         # page position under the cursor, -1 none ("current selection", P+ Net-Random.asm)
SSS_CLOCK = 0x250            # frames since the screen opened
SSS_TAKEN_KIND = 0x258       # chosen stage kind, copied out when the screen exits
SSS_CONTROLLER = 0x278       # 0xF0 any port, else that port
SSS_STATE_CHOOSING, SSS_STATE_TAKEN = 0, 2
SSS_ITEM_PAGE, SSS_ITEM_RANDOM, SSS_ITEM_BACK = 0x35, 0x36, 0x37

# =============================================================================================
# IDs
# =============================================================================================

# MuSelchkind (CSS icon ids; BrawlHeaders mu/menu.h). These are what AREA_CHARACTER holds.
CSS_ID: Dict[str, int] = {
    "mario": 0x00, "donkey_kong": 0x01, "link": 0x02, "samus": 0x03, "zero_suit_samus": 0x04,
    "yoshi": 0x05, "kirby": 0x06, "fox": 0x07, "pikachu": 0x08, "luigi": 0x09,
    "captain_falcon": 0x0A, "ness": 0x0B, "bowser": 0x0C, "peach": 0x0D, "zelda": 0x0E,
    "sheik": 0x0F, "ice_climbers": 0x10, "marth": 0x11, "game_and_watch": 0x12, "falco": 0x13,
    "ganondorf": 0x14, "wario": 0x15, "meta_knight": 0x16, "pit": 0x17, "olimar": 0x18,
    "lucas": 0x19, "diddy_kong": 0x1A, "pokemon_trainer": 0x1B, "charizard": 0x1C,
    "squirtle": 0x1D, "ivysaur": 0x1E, "king_dedede": 0x1F, "lucario": 0x20, "ike": 0x21,
    "rob": 0x22, "jigglypuff": 0x23, "toon_link": 0x24, "wolf": 0x25, "snake": 0x26,
    "sonic": 0x27, "none": 0x28, "random": 0x29,
}
CSS_NONE = 0x28
CSS_RANDOM = 0x29
# P+ "Hold Shield for Special Fighter" (ProjectM/CSS.ASM): shield on Wario/Ice Climbers/Bowser
# selects these slots (0x80585B00 + 0x10*id: 0x36 = 2D 17.. WarioMan, 0x37 = 11 FF.. solo Popo
# ("Sopo"), 0x38 = 2C 0C.. Giga Bowser).
CSS_PPLUS_WARIOMAN = 0x36
CSS_PPLUS_SOPO = 0x37
CSS_PPLUS_GIGA_BOWSER = 0x38
# P+ v3.2 CSS icon order (CSS_ROSTER, 43 bytes). The P+-only ids (0x2A-0x2E, 0x30) are clones
# (Mewtwo, Roy, Knuckles, independent Pokemon); their exact names need live verification.
PPLUS_CSS_ROSTER = bytes([
    0x00, 0x09, 0x0D, 0x15, 0x05, 0x0C, 0x01, 0x1A, 0x0A, 0x07, 0x13, 0x25, 0x02, 0x24, 0x0E,
    0x0F, 0x14, 0x08, 0x23, 0x2E, 0x2B, 0x2C, 0x2A, 0x20, 0x03, 0x04, 0x0B, 0x19, 0x06, 0x16,
    0x1F, 0x11, 0x2D, 0x21, 0x12, 0x22, 0x10, 0x17, 0x29, 0x18, 0x26, 0x27, 0x30])

# gmCharacterKind (gm_lib.h): what gmPlayerInitData.m_characterKind / ftEntry+0x5C hold.
CHAR_KIND: Dict[str, int] = {
    "mario": 0x00, "donkey_kong": 0x01, "link": 0x02, "samus": 0x03, "zero_suit_samus": 0x04,
    "yoshi": 0x05, "kirby": 0x06, "fox": 0x07, "pikachu": 0x08, "luigi": 0x09,
    "captain_falcon": 0x0A, "ness": 0x0B, "bowser": 0x0C, "peach": 0x0D, "zelda": 0x0E,
    "sheik": 0x0F, "ice_climbers": 0x10, "marth": 0x13, "game_and_watch": 0x14, "falco": 0x15,
    "ganondorf": 0x16, "wario": 0x17, "meta_knight": 0x18, "pit": 0x19, "olimar": 0x1A,
    "lucas": 0x1B, "diddy_kong": 0x1C, "king_dedede": 0x23, "lucario": 0x24, "ike": 0x25,
    "rob": 0x26, "jigglypuff": 0x27, "toon_link": 0x28, "wolf": 0x29, "snake": 0x2A,
    "sonic": 0x2B, "giga_bowser": 0x2C, "warioman": 0x2D,
}
CHAR_GIGA_BOWSER = 0x2C
CHAR_WARIOMAN = 0x2D
# Byte 0 of each 16-byte entry of the CSS slot table at 0x80455458, read from the Rev 1 DOL:
# CSS_TO_CHAR_KIND[MuSelchkind] = gmCharacterKind (0xFF: Pokemon Trainer, resolved per Pokemon).
CSS_TO_CHAR_KIND = bytes([
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
    0x0F, 0x10, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0xFF, 0x1D, 0x1F,
    0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B])

# ftKind (ft_entry.h): the live fighter's kind (Fighter+0x110, ftEntry instances).
FT_KIND: Dict[str, int] = {
    "mario": 0x00, "donkey_kong": 0x01, "link": 0x02, "samus": 0x03, "yoshi": 0x04, "kirby": 0x05,
    "fox": 0x06, "pikachu": 0x07, "luigi": 0x08, "captain_falcon": 0x09, "ness": 0x0A,
    "bowser": 0x0B, "peach": 0x0C, "zelda": 0x0D, "sheik": 0x0E, "popo": 0x0F, "nana": 0x10,
    "marth": 0x11, "game_and_watch": 0x12, "falco": 0x13, "ganondorf": 0x14, "wario": 0x15,
    "meta_knight": 0x16, "pit": 0x17, "zero_suit_samus": 0x18, "olimar": 0x19, "lucas": 0x1A,
    "diddy_kong": 0x1B, "pokemon_trainer": 0x1C, "charizard": 0x1D, "squirtle": 0x1E,
    "ivysaur": 0x1F, "king_dedede": 0x20, "lucario": 0x21, "ike": 0x22, "rob": 0x23,
    "jigglypuff": 0x25, "mewtwo": 0x26, "roy": 0x27, "toon_link": 0x29, "wolf": 0x2C,
    "knuckles": 0x2D, "snake": 0x2E, "sonic": 0x2F, "giga_bowser": 0x30, "warioman": 0x31,
}
FT_GIGA_BOWSER = 0x30
FT_WARIOMAN = 0x31
FT_NON_PLAYABLE = frozenset(range(0x30, 0x37))  # Giga, WarioMan, Alloys 0x32-0x35, MarioD 0x36

# Stage kinds. BF/FD are the same in vanilla srStageKind and in P+'s RSS slot table.
STAGE_KIND: Dict[str, int] = {
    "battlefield": 0x01, "final_destination": 0x02,
    # P+ 2024 legal list (orca RankedPPlus.cpp LEGAL_KINDS; P+ stage ids):
    "pokemon_stadium_2": 0x2E, "smashville": 0x21, "luigis_mansion": 0x04, "temple_of_time": 0x09,
    "green_hill_zone": 0x23, "bowsers_castle": 0x06, "frigate_husk": 0x0C, "dream_land": 0x2D,
}
STAGE_SUBSPACE = 0x3D            # srStageKind Adventure (gm_lib.h)
STAGE_SPECIAL_KINDS = frozenset(range(0x34, 0x3E))  # HomeRun, Edit, Heal, OTrain, TBreak, CRoll, ..., Adventure

# Netplay stage-switch preset P+ v3.2 loads (pf/stage/switch/SwitchFF.rss, bytes 0x3C..0x184 =
# five pages {count, 39 slots} then the slot -> (kind, cosmetic) table). Used only when the live
# table at RSS_EXDATA is empty.
SWITCHFF_PAGES_AND_SLOTS = bytes.fromhex(
    "152005141e210a160c15180104230b081c1a0028020300000000000000000000000000000000000015240607091f0d27"
    "0f10250e221912261113171b1d2b00000000000000000000000000000000000013312d383b3233362e3d393c34353e2f"
    "372c303a0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000010102020303040405050606070708080909330a492c0c0c0d0d0e0e130f14101511161217131814"
    "19151c161d171e181f19201a211b221c231d241e432629322a33472a2c352d362f3730383139323a2e3bff64ff64373c"
    "402341244225251f4427452846292b34482b0b0b4a2d4b2e4c2f4d304e314f3d503e513f52400000")
assert len(SWITCHFF_PAGES_AND_SLOTS) == 0x148

# =============================================================================================
# Low-level readers
# =============================================================================================


def is_mem_addr(p: int, length: int = 1) -> bool:
    """True if [p, p+length) lies in MEM1 (0x80000000-0x817FFFFF) or MEM2 (0x90000000-0x93FFFFFF)."""
    end = p + length
    return (0x80000000 <= p and end <= 0x81800000) or (0x90000000 <= p and end <= 0x94000000)


def is_obj_ptr(p: int) -> bool:
    """A plausible pointer to a game object: in RAM and 4-aligned."""
    return is_mem_addr(p, 4) and p % 4 == 0


class Mem:
    """Typed big-endian reads over a ``read_mem`` callable. Bad addresses raise ``BadPointer``."""

    def __init__(self, read_mem: ReadMem):
        self._read = read_mem

    def read(self, addr: int, length: int) -> bytes:
        if not is_mem_addr(addr, length):
            raise BadPointer(addr)
        data = self._read(addr, length)
        if len(data) != length:
            raise BadPointer(addr)
        return bytes(data)

    def u8(self, a: int) -> int:
        return self.read(a, 1)[0]

    def s8(self, a: int) -> int:
        return struct.unpack(">b", self.read(a, 1))[0]

    def u16(self, a: int) -> int:
        return struct.unpack(">H", self.read(a, 2))[0]

    def u32(self, a: int) -> int:
        return struct.unpack(">I", self.read(a, 4))[0]

    def s32(self, a: int) -> int:
        return struct.unpack(">i", self.read(a, 4))[0]

    def f32(self, a: int) -> float:
        return struct.unpack(">f", self.read(a, 4))[0]

    def ptr(self, a: int) -> int:
        """Reads a pointer and checks it is a plausible object pointer."""
        p = self.u32(a)
        if not is_obj_ptr(p):
            raise BadPointer(p)
        return p

    def chain(self, base: int, *offsets: int) -> int:
        """``[[[base]+o1]+o2]...``: dereference ``base``, add ``o1``, dereference, ... Returns the
        final pointer (each intermediate must be a valid object pointer)."""
        p = self.ptr(base)
        for o in offsets:
            p = self.ptr(p + o)
        return p

    def cstr(self, a: int, limit: int = 48) -> str:
        if not is_mem_addr(a, 1):
            raise BadPointer(a)
        n = min(limit, 0x81800000 - a if a < 0x90000000 else 0x94000000 - a)
        raw = self.read(a, n)
        end = raw.find(b"\0")
        if end < 0:
            raise BadPointer(a)
        return raw[:end].decode("latin-1")


class BadPointer(Exception):
    """A pointer chain hit an address outside RAM (the structure is not alive)."""

    def __init__(self, addr: int):
        super().__init__(f"bad pointer {addr:#010x}")
        self.addr = addr


def _try(fn: Callable[[], Any], default: Any = None) -> Any:
    try:
        return fn()
    except BadPointer:
        return default


# =============================================================================================
# 1. Scene detection
# =============================================================================================


class Scene(enum.Enum):
    UNKNOWN = "unknown"            # no valid gfSceneManager yet (launcher, early boot)
    STRAP = "strap"                # scStrap: Wii Remote strap / ESRB notice
    BOOT = "boot"                  # scBoot: save check (P+'s 'create save?' prompt lives here)
    TITLE = "title"                # scTitle
    MAIN_MENU = "main_menu"        # muMenuMain: top page and its sub-pages (Group > Brawl is a page)
    CSS = "css"                    # scSelctCharacter (Versus character select)
    SSS = "sss"                    # scSelStage
    LOADING = "loading"            # scMemoryChange (between scenes; memory layout swap)
    IN_MATCH = "in_match"          # scMelee
    RESULTS = "results"            # scVsResult
    PRIZE = "prize"                # scPrizeInfo (unlock announcements; P+ skips them)
    OTHER = "other"                # any other known scene (see SCENE_NAMES)


# Scene/sequence names found as strings in sora_scene.rel / main.dol (Rev 1), plus muMenuMain
# (orca Harness.cpp logs, sora_menu_main). Boot order seen in a P+ log (brawlback-asm
# memLocations.txt): scStrap -> scMemoryChange -> scBoot -> (sqBoot -> sqVsMelee) ->
# scMemoryChange -> scSelctCharacter.
SCENE_NAMES: Dict[str, Scene] = {
    "scStrap": Scene.STRAP, "scBoot": Scene.BOOT, "scTitle": Scene.TITLE,
    "muMenuMain": Scene.MAIN_MENU, "scSelctCharacter": Scene.CSS, "scSelStage": Scene.SSS,
    "scMemoryChange": Scene.LOADING, "scMelee": Scene.IN_MATCH, "scVsResult": Scene.RESULTS,
    "scPrizeInfo": Scene.PRIZE,
}
OTHER_SCENES = frozenset({
    "scAdvDiff", "scAdvEnding", "scAdvGameover", "scAdvLoad", "scAdvMap", "scAdvName",
    "scAdvResult", "scAdvSave", "scAdvSeal", "scAdvSelChar", "scAdvVisual", "scButton",
    "scChallenger", "scCharacterRoll", "scClearGetter", "scCoinShooter", "scDemo", "scEdit",
    "scEditMenu", "scEditSelect", "scEnding", "scFigureGetDemo", "scGameover", "scHowToPlay",
    "scIntro", "scNetFriendList", "scNetTimeResult", "scNetWatchBet", "scQMEntry", "scQMSetting",
    "scReplay", "scSealDisp", "scSealList", "scSelChar", "scSelEvent", "scSimpleResult",
    "scSnapShot", "scSoftList", "scStaffRoll", "scTourEntry", "scTourRule", "scTutorial",
    "scTyFigDisp", "scTyFigDisp2", "scTyFigList", "scVCTrial",
})
# Sequences a netplay Versus session may legitimately be in.
ALLOWED_SEQUENCES = frozenset({"sqBoot", "sqVsMelee", "sqMenuMain", "sqTitle", "sqPrizeCheck"})
# Sequences that mean a non-Versus mode (QA: must be unreachable in netplay).
BANNED_SEQUENCES: Dict[str, str] = {
    "sqAdventure": "Subspace Emissary", "sqSingleBoss": "Boss Battles",
    "sqSingleSimple": "Classic", "sqSingleAllstar": "All-Star", "sqEvent": "Events",
    "sqTargetBreak": "Target Smash", "sqHomerun": "Home-Run Contest", "sqKumite": "Multi-Man",
    "sqTraining": "Training", "sqSpMelee": "Special Brawl", "sqReplay": "Replays",
    "sqEdit": "Stage Builder", "sqDebugDefault": "debug sequence", "sqCoinShooter": "Coin Launcher",
    "sqCoinshooter": "Coin Launcher", "sqChallenger": "challenger approaching",
    "sqTyFigDisp": "Trophy display", "sqButton": "controls", "sqQuMelee": "sqQuMelee",
    "sqToMelee": "sqToMelee", "sqNetAnyOkiraku": "online", "sqNetAnyTeamMelee": "online",
    "sqNetAnyWatch": "online", "sqNetFriendHomerun": "online", "sqNetFriendKumite": "online",
    "sqNetFriendList": "online", "sqNetFriendMelee": "online",
}


@dataclass(frozen=True)
class SceneInfo:
    scene: Scene
    name: str = ""             # current gfScene name ("" if unreadable)
    sequence: str = ""         # current gfSequence name ("" if unreadable)
    next_name: str = ""        # next scene's name if one is queued
    scene_ptr: int = 0
    manager: int = 0

    @property
    def is_banned_mode(self) -> bool:
        return self.sequence in BANNED_SEQUENCES or self.name.startswith("scAdv")


def _plausible_name(s: str, prefixes: Tuple[str, ...]) -> bool:
    return 3 <= len(s) <= 40 and s.isprintable() and s.startswith(prefixes)


def classify_scene(name: str, sequence: str = "") -> Scene:
    """Map a raw scene name (gfScene::m_sceneName) to a ``Scene``."""
    if name in SCENE_NAMES:
        return SCENE_NAMES[name]
    if name in OTHER_SCENES or _plausible_name(name, ("sc", "mu")):
        return Scene.OTHER
    return Scene.UNKNOWN


def read_scene(read_mem: ReadMem) -> SceneInfo:
    """Reads ``[[0x805A0060]+4]+0 -> char*`` (orca Harness.cpp BrawlSceneName, brawlback
    IsCurrentSceneMelee) and ``[[0x805A0060]+0x10]+0`` (the current sequence). Returns
    ``Scene.UNKNOWN`` whenever the chain is not alive (e.g. still in the P+ launcher DOL, where
    0x805A0060 holds unrelated data)."""
    m = Mem(read_mem)
    try:
        manager = m.ptr(SCENE_MANAGER_PTR)
        scene_ptr = m.ptr(manager + SM_CURRENT_SCENE)
        name = m.cstr(m.ptr(scene_ptr))
    except BadPointer:
        return SceneInfo(Scene.UNKNOWN)
    if not _plausible_name(name, ("sc", "mu")):
        return SceneInfo(Scene.UNKNOWN, name="")
    seq = _try(lambda: m.cstr(m.ptr(m.ptr(manager + SM_CURRENT_SEQUENCE))), "")
    if not _plausible_name(seq, ("sq",)):
        seq = ""
    nxt = _try(lambda: m.cstr(m.ptr(m.ptr(manager + SM_NEXT_SCENE))), "")
    if not _plausible_name(nxt, ("sc", "mu")):
        nxt = ""
    return SceneInfo(classify_scene(name, seq), name, seq, nxt, scene_ptr, manager)


def read_frame_counters(read_mem: ReadMem) -> Dict[str, Optional[int]]:
    """``app``: gfApplication+0x100 (every game loop, from boot; dol-verified instruction, pointer
    is runtime). ``game_frame`` / ``persistent``: g_GameFrame +0x4 / +0x14 (live; Brawlback hashes
    ``persistent``)."""
    m = Mem(read_mem)
    return {
        "app": _try(lambda: m.u32(m.ptr(APPLICATION_PTR) + APP_FRAME_COUNTER_OFF)),
        "game_frame": _try(lambda: m.u32(GAME_FRAME + 0x4)),
        "persistent": _try(lambda: m.u32(GAME_FRAME + 0x14)),
    }


# =============================================================================================
# 2. Match state
# =============================================================================================


@dataclass
class PlayerSetup:
    """One gmPlayerInitData (gmGlobalModeMelee players / gmSelCharData players)."""
    port: int
    character: int        # gmCharacterKind
    state: int            # 0 human, 1 CPU, 3 none
    stocks: int
    color: int
    controller: int

    @property
    def present(self) -> bool:
        return self.state in (PLAYER_HUMAN, PLAYER_CPU)


@dataclass
class MatchSetup:
    game_mode: int
    game_rule: int        # 0 time, 1 stock, 2 coin
    num_players: int
    is_stamina: bool
    stage_kind: int
    time_limit_frames: int
    item_frequency: int
    players: List[PlayerSetup]


@dataclass
class PlayerState:
    port: int
    entry_index: int
    entry_id: int = -1
    player_no: int = -1
    character: int = -1            # gmCharacterKind (ftEntry+0x5C)
    ft_kind: int = -1              # active fighter's ftKind (Fighter+0x110)
    human: Optional[bool] = None   # from the match setup (state 0)
    damage: Optional[float] = None
    stocks: Optional[int] = None
    x: Optional[float] = None
    y: Optional[float] = None
    prev_x: Optional[float] = None
    prev_y: Optional[float] = None
    facing: Optional[float] = None  # posture lr: +1 right, -1 left
    action: Optional[int] = None    # soStatusModule status kind (action state)
    motion_kind: Optional[int] = None
    anim_frame: Optional[float] = None

    @property
    def vel_x(self) -> Optional[float]:
        """Velocity derived from position delta (pos - prevPos) for this frame."""
        if self.x is None or self.prev_x is None:
            return None
        return self.x - self.prev_x

    @property
    def vel_y(self) -> Optional[float]:
        if self.y is None or self.prev_y is None:
            return None
        return self.y - self.prev_y


@dataclass
class MatchState:
    scene: SceneInfo
    in_match: bool
    started: bool = False          # stOperatorRuleMelee.m_isStart
    game_set: bool = False         # stOperatorRule.m_isGameSet
    frames_elapsed: Optional[int] = None
    remaining_frames: Optional[int] = None
    decision: Optional[int] = None
    frame_counters: Dict[str, Optional[int]] = field(default_factory=dict)
    setup: Optional[MatchSetup] = None
    players: List[PlayerState] = field(default_factory=list)

    @property
    def ended(self) -> bool:
        """Game set in this snapshot (stOperatorRule.m_isGameSet). The other end condition, the
        scene leaving scMelee after a match, needs history: see ``wait_match_end``."""
        return self.in_match and self.game_set

    @property
    def remaining_seconds(self) -> Optional[float]:
        return None if self.remaining_frames is None else self.remaining_frames / 60.0


def _game_global(m: Mem, off: int) -> int:
    return m.chain(GAME_GLOBAL_PTR, off)


def _read_player_init(m: Mem, base: int, port: int) -> PlayerSetup:
    b = m.read(base + port * MM_PLAYER_SIZE, 8)
    return PlayerSetup(port, b[PI_CHARACTER], b[PI_STATE], struct.unpack(">b", b[4:5])[0],
                       struct.unpack(">b", b[5:6])[0], struct.unpack(">b", b[7:8])[0])


def read_match_setup(read_mem: ReadMem) -> Optional[MatchSetup]:
    """gmGlobalModeMelee (``[[0x805A00E0]+0x08]``, live 0x90180F20): what the next/current match
    will be. Valid from the stage select's exit through the match."""
    m = Mem(read_mem)
    try:
        mm = _game_global(m, GG_MODE_MELEE)
        init = m.read(mm + MM_INIT, 0x1C)
        players = [_read_player_init(m, mm + MM_PLAYERS, p) for p in range(4)]
        return MatchSetup(
            game_mode=init[0] >> 2, game_rule=init[1] >> 5, num_players=(init[1] >> 2) & 7,
            is_stamina=bool(init[3] & 0x20), stage_kind=m.u16(mm + MM_STAGE_KIND),
            time_limit_frames=m.s32(mm + MM_TIME_LIMIT_FRAMES), item_frequency=m.u8(mm + MM_ITEM_FREQUENCY),
            players=players)
    except BadPointer:
        return None


def read_set_rule(read_mem: ReadMem) -> Optional[Dict[str, int]]:
    """gmSetRule (``[[0x805A00E0]+0x1C]``): the Rules menu. P+ NETPLAY.TXT 'Default Settings
    Modifier' sets stock / 4 stocks / 8 minutes / team attack on / pause on at boot."""
    m = Mem(read_mem)
    try:
        r = _game_global(m, GG_SET_RULE)
        b = m.read(r, 0x0C)
    except BadPointer:
        return None
    return {"rule": b[RULE_MODE] & 7, "time_minutes": b[RULE_TIME_MINUTES], "stocks": b[RULE_STOCKS],
            "handicap": b[RULE_HANDICAP], "damage_ratio_x10": b[RULE_DAMAGE_RATIO],
            "stage_choice": b[RULE_STAGE_CHOICE], "stock_time_minutes": b[RULE_STOCK_MINUTES],
            "team_attack": b[RULE_TEAM_ATTACK], "pause": b[RULE_PAUSE]}


def _fighter_of_entry(m: Mem, entry: int) -> int:
    active = m.u8(entry + FTE_ACTIVE_INSTANCE) & 3
    return m.ptr(entry + FTE_INSTANCES + 8 * active + 4)


def read_entry(m: Mem, entry: int, index: int) -> PlayerState:
    ps = PlayerState(port=-1, entry_index=index)
    ps.entry_id = _try(lambda: m.s32(entry + FTE_ENTRY_ID), -1)
    ps.player_no = _try(lambda: m.s32(entry + FTE_PLAYER_NO), -1)
    ps.port = ps.player_no if 0 <= ps.player_no < 4 else index
    ps.character = _try(lambda: m.u32(entry + FTE_CHARACTER_KIND), -1)
    try:
        data = m.chain(entry + FTE_OWNER, OWNER_DATA)
        ps.damage = m.f32(data + OWNER_DAMAGE)
        ps.stocks = m.s32(data + OWNER_STOCKS)
    except BadPointer:
        pass
    try:
        fighter = _fighter_of_entry(m, entry)
    except BadPointer:
        return ps
    ps.ft_kind = _try(lambda: m.u32(fighter + FIGHTER_KIND), -1)
    try:
        enum_ = m.chain(fighter + FIGHTER_ACCESSER, ACC_ENUMERATION)
    except BadPointer:
        return ps
    posture = _try(lambda: m.ptr(enum_ + ENUM_POSTURE))
    if posture:
        ps.x, ps.y = struct.unpack(">ff", m.read(posture + POSTURE_POS, 8))
        ps.prev_x, ps.prev_y = struct.unpack(">ff", m.read(posture + POSTURE_PREV_POS, 8))
        ps.facing = m.f32(posture + POSTURE_LR)
    status = _try(lambda: m.ptr(enum_ + ENUM_STATUS))
    if status:
        ps.action = m.s32(status + STATUS_KIND)
    motion = _try(lambda: m.ptr(enum_ + ENUM_MOTION))
    if motion:
        ps.anim_frame = m.f32(motion + MOTION_FRAME)
        ps.motion_kind = m.s32(motion + MOTION_KIND)
    return ps


def read_players(read_mem: ReadMem, setup: Optional[MatchSetup] = None, max_entries: int = 4) -> List[PlayerState]:
    """Per-fighter state through ftEntryManager (0x80624780 in P+): entries[i] (stride 0x244) ->
    owner->data (damage f32 @+0x24, stocks s32 @+0x34; both REL-verified accessors) and the active
    Fighter -> moduleAccesser(+0x60) -> enumeration(+0xD8) -> posture(+0xC) / status(+0x70) /
    motion(+0x8). Only entries with a live fighter or owner are returned."""
    m = Mem(read_mem)
    try:
        entries = m.ptr(FT_ENTRY_MANAGER)
        count = m.u32(FT_ENTRY_MANAGER + 4)
    except BadPointer:
        return []
    n = max(0, min(count if 0 < count <= 16 else max_entries, max_entries))
    out = []
    for i in range(n):
        ps = read_entry(m, entries + i * FTE_SIZE, i)
        if ps.damage is None and ps.x is None:
            continue
        if setup is not None and 0 <= ps.port < len(setup.players):
            ps.human = setup.players[ps.port].state == PLAYER_HUMAN
        out.append(ps)
    return out


def read_match_state(read_mem: ReadMem) -> MatchState:
    """Everything a test needs to observe a match. Safe to call in any scene."""
    m = Mem(read_mem)
    info = read_scene(read_mem)
    st = MatchState(scene=info, in_match=info.scene is Scene.IN_MATCH,
                    frame_counters=read_frame_counters(read_mem))
    st.setup = read_match_setup(read_mem)
    if not st.in_match:
        return st
    try:
        rule = m.ptr(info.scene_ptr + SCM_OPERATOR_RULE_MELEE)
        st.game_set = m.u8(rule + OPR_IS_GAME_SET) != 0
        st.started = m.u8(rule + OPR_IS_START) != 0
        st.remaining_frames = m.u32(rule + OPR_REMAINING_FRAMES)
        st.frames_elapsed = m.u32(rule + OPR_FRAMES_ELAPSED)
        st.decision = m.u32(rule + OPR_DECISION)
    except BadPointer:
        pass
    st.players = read_players(read_mem, st.setup)
    return st


def read_results(read_mem: ReadMem) -> Optional[Dict[str, Any]]:
    """gmResultInfo (``[[0x805A00E0]+0x18]``), filled before scVsResult's first frame."""
    m = Mem(read_mem)
    try:
        ri = _game_global(m, GG_RESULT_INFO)
        players = []
        for p in range(4):
            b = m.read(ri + RI_PLAYERS + p * RI_PLAYER_SIZE, 0x18)
            players.append({"port": p, "character": b[0], "state": b[1], "stocks": struct.unpack(">b", b[0xA:0xB])[0],
                            "place": b[0xE], "kos": struct.unpack(">I", b[0x10:0x14])[0],
                            "falls": struct.unpack(">I", b[0x14:0x18])[0]})
        return {"rule": m.u8(ri + RI_RULE), "stage": m.u16(ri + RI_STAGE),
                "winning_player": m.s8(ri + RI_WINNING_PLAYER), "decision": m.u8(ri + RI_DECISION),
                "players": players}
    except BadPointer:
        return None


# ---- Brawlback desync checksum (Project-Plus-Dolphin-brawlback RollbackManager.cpp:278-397) ----

@dataclass(frozen=True)
class ChecksumField:
    name: str
    base: int
    offsets: Tuple[int, ...] = ()   # empty: read ``size`` bytes at ``base``
    size: int = 4


def _bb_pos(port_off: int, axis: int) -> Tuple[int, ...]:
    return (port_off, 0x60, 0xD8, 0x0C, axis)


# Exactly Brawlback's table, in order. Pointer semantics (DeltaSaveSlot.h:93-109): start at
# ``base``; for each offset: dereference, then add the offset; the final address is read.
BRAWLBACK_CHECKSUM_FIELDS: Tuple[ChecksumField, ...] = (
    ChecksumField("persistentFrameCounter", 0x901812A0 + 0x14),
    ChecksumField("p1_damage", 0x80623324), ChecksumField("p2_damage", 0x80623568),
    ChecksumField("p3_damage", 0x806237AC), ChecksumField("p4_damage", 0x806239F0),
    ChecksumField("p1_stocks", 0x80623318), ChecksumField("p2_stocks", 0x8062355C),
    ChecksumField("p3_stocks", 0x806237A0), ChecksumField("p4_stocks", 0x806239E4),
    ChecksumField("p1_x", 0x80624780, _bb_pos(0x34, 0x0C)), ChecksumField("p1_y", 0x80624780, _bb_pos(0x34, 0x10)),
    ChecksumField("p2_x", 0x80624780, _bb_pos(0x278, 0x0C)), ChecksumField("p2_y", 0x80624780, _bb_pos(0x278, 0x10)),
    ChecksumField("p3_x", 0x80624780, _bb_pos(0x4BC, 0x0C)), ChecksumField("p3_y", 0x80624780, _bb_pos(0x4BC, 0x10)),
    ChecksumField("p4_x", 0x80624780, _bb_pos(0x700, 0x0C)), ChecksumField("p4_y", 0x80624780, _bb_pos(0x700, 0x10)),
    ChecksumField("p1_vel", 0x80494F30, size=8), ChecksumField("p2_vel", 0x8049DEE4, size=8),
    ChecksumField("p3_vel", 0x80494F98, size=8), ChecksumField("p4_vel", 0x80495000, size=8),
)
# Ranges Brawlback leaves out of its rollback savestates (RollbackManager.cpp:272-276).
BRAWLBACK_EXCLUDED_RANGES: Tuple[Tuple[int, int, str], ...] = (
    (0x804E7C00, 0xC00, "AX Wii audio buffers"),
    (0x8049A4EA, 0x1400, "AX Wii voice parameter blocks"),
    (0x90000800, 0x12C800, "Brawl framebuffer buffers"),
)


def resolve_checksum_field(read_mem: ReadMem, f: ChecksumField) -> Optional[Tuple[int, int]]:
    """(address, size) or None when a pointer in the chain is null/invalid (Brawlback skips it)."""
    if not f.offsets:
        return (f.base, f.size)
    m = Mem(read_mem)
    addr = f.base
    for off in f.offsets:
        try:
            p = m.u32(addr)
        except BadPointer:
            return None
        if p == 0 or not is_mem_addr(p):
            return None
        addr = p + off
    return (addr, f.size) if is_mem_addr(addr, f.size) else None


def brawlback_checksum_ranges(read_mem: ReadMem) -> List[Tuple[int, int]]:
    """The [addr, len] list to feed ``hash_mem`` to cover exactly Brawlback's checksum fields."""
    out = []
    for f in BRAWLBACK_CHECKSUM_FIELDS:
        r = resolve_checksum_field(read_mem, f)
        if r is not None:
            out.append(r)
    return out


def brawlback_checksum(read_mem: ReadMem) -> int:
    """Brawlback's CalculateBrawlbackDesyncChecksum: zlib CRC32 over the raw big-endian bytes of the
    resolved fields in order; 0 outside scMelee. Same value Brawlback feeds GekkoNet."""
    info = read_scene(read_mem)
    if info.name != "scMelee":
        return 0
    crc = 0
    for addr, size in brawlback_checksum_ranges(read_mem):
        try:
            crc = zlib.crc32(Mem(read_mem).read(addr, size), crc)
        except BadPointer:
            continue
    return crc & 0xFFFFFFFF


# =============================================================================================
# 4. Banned / unintended states
# =============================================================================================


def read_debug_flags(read_mem: ReadMem) -> Optional[bytes]:
    """The 12 P+ debug bytes at 0x80583FF4: +0xB (0x80583FFF) Debug Mode, +0x9 Hitbox Display,
    +0x3 Collision Display, +0x5 Stage Collisions, +0x7 Camera Lock (Net-CodeMenu.asm copies the
    Code Menu's lines here each frame), +0x4 / +0x8 modifiedDebug's display modes."""
    return _try(lambda: Mem(read_mem).read(DEBUG_FLAGS, 12))


def check_banned(read_mem: ReadMem) -> List[str]:
    """Violations of 'netplay Versus only'. Empty list = clean. Covers Subspace / Boss Battles /
    other modes (sequence and scene names, game mode, stage kind), Giga Bowser / Wario-Man (setup,
    CSS panels and live fighters, incl. Final Smash or Code Menu character-switch transforms) and
    the P+ Code Menu debug features."""
    out: List[str] = []
    m = Mem(read_mem)
    info = read_scene(read_mem)
    if info.sequence in BANNED_SEQUENCES:
        out.append(f"sequence {info.sequence} ({BANNED_SEQUENCES[info.sequence]})")
    elif info.sequence and info.sequence not in ALLOWED_SEQUENCES:
        out.append(f"unexpected sequence {info.sequence}")
    if info.name.startswith("scAdv"):
        out.append(f"scene {info.name} (Subspace Emissary)")
    elif info.scene is Scene.OTHER:
        out.append(f"unexpected scene {info.name}")
    setup = read_match_setup(read_mem) if info.scene in (Scene.IN_MATCH, Scene.LOADING, Scene.RESULTS) else None
    if setup is not None and info.scene is Scene.IN_MATCH:
        if setup.game_mode != 0:
            out.append(f"game mode {setup.game_mode:#x} is not Melee/Versus")
        if setup.stage_kind == STAGE_SUBSPACE or setup.stage_kind in STAGE_SPECIAL_KINDS:
            out.append(f"special stage kind {setup.stage_kind:#x}")
        if setup.is_stamina:
            out.append("stamina mode")
    if setup is not None:
        for p in setup.players:
            if p.present and p.character in (CHAR_GIGA_BOWSER, CHAR_WARIOMAN):
                out.append(f"port {p.port + 1} set up as {'Giga Bowser' if p.character == CHAR_GIGA_BOWSER else 'Wario-Man'}")
            elif p.present and 0x2E <= p.character <= 0x3D:
                out.append(f"port {p.port + 1} set up as non-playable character kind {p.character:#x}")
    if info.scene is Scene.IN_MATCH:
        for ps in read_players(read_mem, setup):
            if ps.ft_kind == FT_GIGA_BOWSER:
                out.append(f"port {ps.port + 1} fighter is Giga Bowser")
            elif ps.ft_kind == FT_WARIOMAN:
                out.append(f"port {ps.port + 1} fighter is Wario-Man")
            elif ps.ft_kind in FT_NON_PLAYABLE:
                out.append(f"port {ps.port + 1} fighter kind {ps.ft_kind:#x} is non-playable")
    if info.scene is Scene.CSS:
        for port in range(4):
            a = read_css_area(read_mem, port)
            if a and a.kind != CSS_AREA_EMPTY and a.character in (CSS_PPLUS_GIGA_BOWSER, CSS_PPLUS_WARIOMAN):
                out.append(f"port {port + 1} CSS on {'Giga Bowser' if a.character == CSS_PPLUS_GIGA_BOWSER else 'Wario-Man'}")
    dbg = read_debug_flags(read_mem)
    if dbg is not None:
        if dbg[0xB] & 1:
            out.append("P+ Debug Mode on (0x80583FFF)")
        names = {0x9: "Hitbox Display", 0x3: "Collision Display", 0x5: "Stage Collisions", 0x7: "Camera Lock"}
        for off, nm in names.items():
            if dbg[off]:
                out.append(f"P+ {nm} on (0x{DEBUG_FLAGS + off:08X} = {dbg[off]})")
    state = _try(lambda: m.u32(CODE_MENU_STATE))
    if state == 4:
        out.append("P+ Code Menu open (0x804E0034 == 4)")
    return out


# =============================================================================================
# 5. Desync-relevant regions
# =============================================================================================


@dataclass(frozen=True)
class Heap:
    heap_id: int
    name: str
    start: int
    end: int
    arena: int

    @property
    def size(self) -> int:
        return self.end - self.start


def read_heap_table(read_mem: ReadMem, count: int = 0x48) -> List[Heap]:
    """g_HeapInfos (0x80494958): {name*, gfMemoryPool*, size, arena}. gfMemoryPool's constructor
    (0x80025B78, dol-verified) is placed at the heap's start and stores +4 start, +8 end. Heaps
    move between scenes (each scene has a memory layout), so re-read per scene."""
    m = Mem(read_mem)
    try:
        table = m.read(HEAP_INFOS, 16 * count)
    except BadPointer:
        return []
    out = []
    for i in range(1, count):
        name_p, pool, size, arena = struct.unpack_from(">IIII", table, 16 * i)
        if not is_obj_ptr(pool) or not is_mem_addr(name_p):
            continue
        name = _try(lambda: m.cstr(name_p, 40), "") or ""
        if not name.isprintable():
            continue
        start, end = pool, pool + size
        if size and is_mem_addr(start, size):
            out.append(Heap(i, name, start, end, arena))
    return out


# Heap names as the game spells them (typos included, see brawlback-asm memLocations.txt).
GAMEPLAY_HEAPS: Tuple[str, ...] = (
    "System",            # ftManager 0x80629A00, ftEntryManager 0x80624780, item/effect managers
    "Fighter1Instance", "Fighter2Instance", "Fighter3Instance", "Fighter4Instance",
    "FighterTechqniq", "StageInstance", "ItemInstance", "WeaponInstance", "EnemyInstance",
    "Physics", "InfoInstance", "GameGlobal", "GlobalMode", "WiiPad",
)
# Large but mostly-static heaps worth hashing in a 'full' comparison (resources + module data).
GAMEPLAY_HEAPS_FULL: Tuple[str, ...] = GAMEPLAY_HEAPS + (
    "Effect", "OverlayCommon", "OverlayStage", "OverlayFighter1", "OverlayFighter2",
    "OverlayFighter3", "OverlayFighter4", "Fighter1Resoruce2", "Fighter2Resoruce2",
    "Fighter3Resoruce2", "Fighter4Resoruce2", "StageResource", "IteamResource",
)
MENU_HEAPS: Tuple[str, ...] = ("MenuInstance", "OverlayMenu", "GameGlobal", "GlobalMode", "WiiPad", "System")
# Never meaningful to compare (audio, video, threads, file buffers).
NOISY_HEAPS: Tuple[str, ...] = ("Sound", "CopyFB", "RenderFifo", "Thread", "Network", "Replay",
                                "Tmp", "MeleeFont", "Font", "Font2", "SystemFW")


def _static_gameplay_ranges(read_mem: ReadMem) -> List[Tuple[int, int, str]]:
    m = Mem(read_mem)
    out: List[Tuple[int, int, str]] = [
        (MTRAND_DEFAULT, 8, "g_mtRand"), (MTRAND_OTHER, 8, "g_mtRandOther"),
        (GAME_FRAME, 0x18, "g_GameFrame"),
        (FT_ENTRY_MANAGER, 0x50, "ftEntryManager"), (FT_MANAGER, 0x160, "ftManager"),
    ]
    for name, base, off, size in (("gmGlobalModeMelee", GAME_GLOBAL_PTR, GG_MODE_MELEE, MM_SIZE),
                                  ("gmSetRule", GAME_GLOBAL_PTR, GG_SET_RULE, 0x88)):
        p = _try(lambda: m.chain(base, off))
        if p:
            out.append((p, size, name))
    info = read_scene(read_mem)
    if info.scene is Scene.IN_MATCH and info.scene_ptr:
        out.append((info.scene_ptr, 0x100, "scMelee"))
        rule = _try(lambda: m.ptr(info.scene_ptr + SCM_OPERATOR_RULE_MELEE))
        if rule:
            out.append((rule, 0x25C, "stOperatorRuleMelee"))
    return out


def gameplay_ranges(read_mem: ReadMem, full: bool = False) -> List[Tuple[int, int, str]]:
    """[(addr, len, label)] covering in-match gameplay state, resolved from the live heap table,
    minus Brawlback's excluded ranges. Feed ``[(a, n) for a, n, _ in ...]`` to ``hash_mem``."""
    names = GAMEPLAY_HEAPS_FULL if full else GAMEPLAY_HEAPS
    heaps = {h.name: h for h in read_heap_table(read_mem)}
    ranges = [(h.start, h.size, h.name) for n in names if (h := heaps.get(n)) is not None]
    ranges += _static_gameplay_ranges(read_mem)
    return subtract_ranges(merge_ranges(ranges), [(a, n) for a, n, _ in BRAWLBACK_EXCLUDED_RANGES])


def menu_ranges(read_mem: ReadMem) -> List[Tuple[int, int, str]]:
    """Menu-side state (CSS/SSS/rules/code menu/stage switch) for comparing instances on menus."""
    m = Mem(read_mem)
    heaps = {h.name: h for h in read_heap_table(read_mem)}
    ranges = [(h.start, h.size, h.name) for n in MENU_HEAPS if (h := heaps.get(n)) is not None]
    ranges += [(CODE_MENU_BASE, CODE_MENU_SIZE, "P+ Code Menu"), (RSS_EXDATA, 0x320, "P+ RSS_EXDATA"),
               (DEBUG_FLAGS, 12, "P+ debug flags"), (MTRAND_DEFAULT, 8, "g_mtRand"),
               (MTRAND_OTHER, 8, "g_mtRandOther")]
    mgr = _try(lambda: m.ptr(SCENE_MANAGER_PTR))
    if mgr:
        ranges.append((mgr, 0x320, "gfSceneManager"))
    gg = _try(lambda: m.ptr(GAME_GLOBAL_PTR))
    if gg:
        ranges.append((gg, 0x50, "GameGlobal"))
        for off, size, nm in ((GG_MODE_MELEE, MM_SIZE, "gmGlobalModeMelee"), (GG_SEL_CHAR_DATA, 0x340, "gmSelCharData"),
                              (GG_SET_RULE, 0x88, "gmSetRule")):
            p = _try(lambda: m.ptr(gg + off))
            if p:
                ranges.append((p, size, nm))
    pads = _try(lambda: m.ptr(PAD_SYSTEM_PTR))
    if pads:
        ranges.append((pads, 0xB78, "gfPadSystem"))
    return subtract_ranges(merge_ranges(ranges), [(a, n) for a, n, _ in BRAWLBACK_EXCLUDED_RANGES])


def merge_ranges(ranges: Iterable[Tuple[int, int, str]]) -> List[Tuple[int, int, str]]:
    """Sort and merge overlapping/adjacent ranges (labels joined with '+')."""
    rs = sorted((a, a + n, lbl) for a, n, lbl in ranges if n > 0)
    out: List[List[Any]] = []
    for s, e, lbl in rs:
        if out and s <= out[-1][1]:
            out[-1][1] = max(out[-1][1], e)
            if lbl not in out[-1][2].split("+"):
                out[-1][2] += "+" + lbl
        else:
            out.append([s, e, lbl])
    return [(s, e - s, lbl) for s, e, lbl in out]


def subtract_ranges(ranges: Iterable[Tuple[int, int, str]], holes: Iterable[Tuple[int, int]]) -> List[Tuple[int, int, str]]:
    out = list(ranges)
    for hs, hn in holes:
        he = hs + hn
        nxt = []
        for a, n, lbl in out:
            e = a + n
            if he <= a or hs >= e:
                nxt.append((a, n, lbl))
                continue
            if a < hs:
                nxt.append((a, hs - a, lbl))
            if he < e:
                nxt.append((he, e - he, lbl))
        out = nxt
    return out


# =============================================================================================
# CSS / SSS readers
# =============================================================================================


@dataclass
class CssArea:
    port: int
    area: int
    kind: int                 # 0 empty, 1 human, 2 CPU
    character: int            # MuSelchkind under the hand while held, else the token's
    in_hand: bool
    flying: bool
    hand_target: int = CSS_HAND_NOTHING
    hand_button: int = 0
    hand_x: float = math.nan
    hand_y: float = math.nan

    @property
    def placed(self) -> bool:
        """Token down on a character and still (orca OnlineRules.h CssToken::Down)."""
        return self.kind == CSS_AREA_HUMAN and self.character != CSS_NONE and not self.in_hand and not self.flying


def read_css_area(read_mem: ReadMem, port: int) -> Optional[CssArea]:
    """``[[[[0x805A0060]+4]+0x400]+0x44+4*port]``: muSelCharPlayerArea (orca OnlineRules.cpp:597)."""
    m = Mem(read_mem)
    info = read_scene(read_mem)
    if info.scene is not Scene.CSS:
        return None
    try:
        task = m.ptr(info.scene_ptr + CSS_SCENE_TASK)
        area = m.ptr(task + CSS_TASK_AREAS + 4 * port)
        b = m.read(area + AREA_KIND, 0x48)
    except BadPointer:
        return None
    a = CssArea(port, area, struct.unpack_from(">I", b, 0)[0], struct.unpack_from(">I", b, 4)[0],
                b[AREA_IN_HAND - AREA_KIND] != 0, b[AREA_FLYING - AREA_KIND] != 0)
    hand = _try(lambda: m.ptr(area + AREA_HAND))
    if hand:
        hb = _try(lambda: m.read(hand + HAND_TARGET, 0x34))
        if hb:
            a.hand_target = struct.unpack_from(">I", hb, 0)[0]
            a.hand_x, a.hand_y = struct.unpack_from(">ff", hb, HAND_X - HAND_TARGET)
            a.hand_button = struct.unpack_from(">I", hb, HAND_BUTTON - HAND_TARGET)[0]
    return a


@dataclass
class SssView:
    task: int
    cursor_x: float
    cursor_y: float
    state: int
    page: int
    hovered_item: int
    selected: int              # page position under the cursor, -1 none
    clock: int
    taken_kind: int
    controller: int


def read_sss(read_mem: ReadMem) -> Optional[SssView]:
    """``[[[0x805A0060]+4]+0x3AC]``: muSelectStageTask (orca BrawlStages.cpp:178-200, RankedPPlus.h)."""
    m = Mem(read_mem)
    info = read_scene(read_mem)
    if info.scene is not Scene.SSS:
        return None
    try:
        task = m.ptr(info.scene_ptr + SSS_SCENE_TASK)
        cur = m.ptr(task + SSS_CURSOR)
        cx, cy = struct.unpack(">ff", m.read(cur + CURSOR_X, 8))
        b = m.read(task + SSS_STATE, SSS_CONTROLLER + 4 - SSS_STATE)
    except BadPointer:
        return None

    def w(off: int, signed: bool = False) -> int:
        return struct.unpack_from(">i" if signed else ">I", b, off - SSS_STATE)[0]

    return SssView(task, cx, cy, w(SSS_STATE), w(SSS_PAGE), w(SSS_HOVERED_ITEM), w(SSS_SELECTED, True),
                   w(SSS_CLOCK), w(SSS_TAKEN_KIND), w(SSS_CONTROLLER))


# P+ RankedPPlus.h: "Hovered item values: 0 nothing, 1-0x34 a stage". Assumed position + 1 (live).
SSS_ITEM_BASE = 1


def sss_position_under_cursor(v: SssView) -> int:
    """Page position of the stage under the cursor, or -1. Prefers task+0x248 (P+ Net-Random.asm:
    'Current selection on the stage selection screen', what X strikes); falls back to the hovered
    item (task+0x244) minus ``SSS_ITEM_BASE``. Both need live verification."""
    if 0 <= v.selected < SSS_ITEM_PAGE:
        return v.selected
    if 1 <= v.hovered_item < SSS_ITEM_PAGE:
        return v.hovered_item - SSS_ITEM_BASE
    return -1


def read_stage_pages(read_mem: Optional[ReadMem] = None) -> List[List[int]]:
    """P+ stage-select pages as stage kinds: pages[page][position] = kind (orca RankedPPlus.cpp
    PageZero). Falls back to the SwitchFF.rss constants if memory is unavailable/empty."""
    raw = None
    if read_mem is not None:
        raw = _try(lambda: Mem(read_mem).read(RSS_PAGES, len(SWITCHFF_PAGES_AND_SLOTS)))
    if raw is None or raw[0] == 0 or raw[0] > 39:
        raw = SWITCHFF_PAGES_AND_SLOTS
    kinds = raw[0x28 * 5:]
    pages = []
    for p in range(5):
        count = raw[0x28 * p]
        if count > 39:
            count = 0
        slots = raw[0x28 * p + 1:0x28 * p + 1 + count]
        pages.append([kinds[2 * s] if 2 * s < len(kinds) else -1 for s in slots])
    return pages


def stage_positions(kind: int, read_mem: Optional[ReadMem] = None) -> List[Tuple[int, int]]:
    """[(page, position)] where stage ``kind`` appears on P+'s stage select."""
    return [(p, i) for p, page in enumerate(read_stage_pages(read_mem)) for i, k in enumerate(page) if k == kind]


# =============================================================================================
# Driver protocol and helpers
# =============================================================================================


class Driver(Protocol):
    """What the recipes need from a harness connection (``HarnessClient`` matches it)."""

    def read_mem(self, addr: int, length: int) -> bytes: ...

    def write_mem(self, addr: int, data: bytes) -> Any: ...

    def pad_set(self, port: int, pad: Any = None, *, buttons: Sequence[str] = (),
                main: Sequence[int] = (128, 128), c: Sequence[int] = (128, 128), l: int = 0, r: int = 0) -> Any: ...

    def pad_script(self, port: int, frames: Sequence[Mapping[str, Any]], start: Any = "next") -> Any: ...

    def wait_frame(self, frame: Optional[int] = None, *, input_polls: Optional[int] = None,
                   timeout_ms: int = 10000) -> Any: ...

    def status(self) -> Any: ...


class RecipeError(RuntimeError):
    """A recipe could not reach its goal; ``context`` has the last observed state."""

    def __init__(self, msg: str, context: Any = None):
        super().__init__(msg)
        self.context = context


def _field_of(obj: Any, name: str) -> Any:
    if isinstance(obj, Mapping):
        return obj.get(name)
    return getattr(obj, name, None)


def polls(drv: Driver) -> int:
    return int(_field_of(drv.status(), "input_polls") or 0)


def step(drv: Driver, n: int = 1, timeout_ms: int = 5000) -> int:
    """Let ``n`` pad polls (= game frames) pass. Returns the new poll count."""
    target = polls(drv) + n
    r = drv.wait_frame(input_polls=target, timeout_ms=timeout_ms)
    return int(_field_of(r, "input_polls") or target)


def neutral(drv: Driver, port: int) -> None:
    drv.pad_set(port, buttons=(), main=(128, 128))


def tap(drv: Driver, port: int, buttons: Sequence[str], hold: int = 3, release: int = 3,
        main: Tuple[int, int] = (128, 128)) -> None:
    """Press ``buttons`` for exactly ``hold`` polls then release for ``release`` polls (pad_script
    is scheduled on input polls, so the timing is exact per game frame)."""
    neutral(drv, port)
    drv.pad_script(port, [{"buttons": list(buttons), "main": list(main), "hold": hold},
                          {"buttons": [], "hold": release}])
    step(drv, hold + release)


def wait_scene(drv: Driver, want: Iterable[Scene], timeout_frames: int = 3600,
               every: int = 2, on_frame: Optional[Callable[[SceneInfo], None]] = None) -> SceneInfo:
    """Poll the scene until it is one of ``want``. ``on_frame`` runs each poll (e.g. to tap A)."""
    want = set(want)
    start = polls(drv)
    while True:
        info = read_scene(drv.read_mem)
        if info.scene in want:
            return info
        if on_frame is not None:
            on_frame(info)
        if polls(drv) - start > timeout_frames:
            raise RecipeError(f"timeout waiting for {sorted(s.value for s in want)}", info)
        step(drv, every)


def steer_toward(x: float, y: float, tx: float, ty: float, full_tilt_at: float = 3.0,
                 min_offset: int = 30) -> Tuple[int, int]:
    """Main-stick value moving a cursor at (x, y) toward (tx, ty) (y up). Full tilt from
    ``full_tilt_at`` units out, proportional nearer, never below ``min_offset`` (past the dead zone).
    Same law as orca Queue.cpp SteerToward."""
    dx, dy = tx - x, ty - y
    d = math.hypot(dx, dy)
    if not math.isfinite(d) or d < 0.1:
        return (128, 128)
    mag = max(min_offset, min(100.0, 100.0 * d / full_tilt_at))
    cx, cy = dx / d * mag, dy / d * mag
    larger = max(abs(cx), abs(cy))
    if 0 < larger < min_offset:
        cx, cy = cx * min_offset / larger, cy * min_offset / larger
    return (max(1, min(255, int(round(128 + cx)))), max(1, min(255, int(round(128 + cy)))))


# Positions learned while scanning (shared by every recipe in this process). Keyed by screen.
LEARNED_CSS: Dict[int, Tuple[float, float]] = {}     # MuSelchkind -> hand (x, y) inside its icon
LEARNED_SSS: Dict[Tuple[int, int], Tuple[float, float]] = {}  # (page, position) -> cursor (x, y)


def _serpentine(xs: Tuple[float, float], ys: Sequence[float]) -> List[Tuple[float, float]]:
    pts = []
    for i, y in enumerate(ys):
        a, b = (xs[0], xs[1]) if i % 2 == 0 else (xs[1], xs[0])
        pts += [(a, y), (b, y)]
    return pts


# CSS hand range (orca: grid spans about y -5..16; P+ hand speed 1.0/frame at full tilt).
CSS_SCAN_X = (-30.0, 30.0)
CSS_SCAN_Y = (15.0, 12.0, 9.0, 6.0, 3.0, 0.5, -2.5)
# SSS cursor range x -30..30, y -19.5..19.5 (orca StageCursors.cpp:49); 1.625/frame.
SSS_SCAN_X = (-29.0, 29.0)
SSS_SCAN_Y = (18.0, 15.0, 12.0, 9.0, 6.0, 3.0, 0.0, -3.0, -6.0, -9.0, -12.0, -15.0, -18.0)


# =============================================================================================
# 3. Getting into a match
# =============================================================================================

# ---- (a) memory writes ----------------------------------------------------------------------
# Netplay rule for every write below: both instances must write the same bytes, and only to
# memory the game has not consumed yet. Do it before the netplay session starts if you can;
# otherwise write idempotently *every frame* until the consuming scene starts, on both
# instances, so a rollback that restores pre-write state is re-covered. Until both have written,
# full-RAM hashes differ, so start desync comparisons after the consuming scene begins.


def write_rules(drv: Driver, stocks: int = 4, minutes: int = 8, items_off: bool = True) -> None:
    """gmSetRule: stock mode, ``stocks``, ``minutes`` stock time limit (+ item frequency None in the
    record's menu data, orca RSBE01.patches). P+ NETPLAY.TXT already sets 4 stocks / 8 min at
    boot; this is for asserting or forcing it. Consumed when the CSS/SSS builds the match setup."""
    m = Mem(drv.read_mem)
    rule = m.chain(GAME_GLOBAL_PTR, GG_SET_RULE)
    b = bytearray(m.read(rule, 0x0C))
    b[RULE_MODE] = (b[RULE_MODE] & 0xF8) | 1
    b[RULE_STOCKS] = stocks
    b[RULE_STOCK_MINUTES] = minutes
    drv.write_mem(rule, bytes(b))
    if items_off:
        rec = _try(lambda: m.chain(GAME_GLOBAL_PTR, GG_RECORD))
        if rec:
            drv.write_mem(rec + 0x810, b"\x00")


def seed_css_record(drv: Driver, players: Mapping[int, Tuple[int, int]]) -> None:
    """Pre-place CSS tokens: gmSelCharData players[port] = (gmCharacterKind, state). P+ keeps this
    record ('CSS Selections Preserved in VS Mode', op b 0x3C @ 0x806DCA90) and the CSS builds its
    panels from it (orca pplus-cpu-record-probe.txt). Write before scSelctCharacter starts (e.g.
    every frame of scStrap/scBoot). NEEDS LIVE VERIFICATION that humans come up with the token
    placed; fall back to ``css_pick_character``."""
    m = Mem(drv.read_mem)
    scd = m.chain(GAME_GLOBAL_PTR, GG_SEL_CHAR_DATA)
    for port, (char_kind, state) in players.items():
        drv.write_mem(scd + SCD_PLAYERS + port * MM_PLAYER_SIZE, bytes([char_kind, state]))


def verify_match_setup(read_mem: ReadMem, characters: Mapping[int, int], stage_kind: Optional[int] = None,
                       stocks: int = 4, minutes: Optional[int] = 8) -> List[str]:
    """Check gmGlobalModeMelee after the SSS (scMemoryChange / scMelee). ``characters``: port ->
    gmCharacterKind. Returns mismatches (empty = OK)."""
    s = read_match_setup(read_mem)
    if s is None:
        return ["match setup unreadable"]
    out = []
    if s.game_mode != 0:
        out.append(f"game mode {s.game_mode}")
    if s.game_rule != 1:
        out.append(f"rule {s.game_rule} (want 1 = stock)")
    if stage_kind is not None and (s.stage_kind & 0xFF) != stage_kind:
        out.append(f"stage {s.stage_kind:#x} (want {stage_kind:#x})")
    if minutes is not None and s.time_limit_frames != minutes * 3600:
        out.append(f"time limit {s.time_limit_frames} frames (want {minutes * 3600})")
    for port, ck in characters.items():
        p = s.players[port]
        if p.state != PLAYER_HUMAN:
            out.append(f"port {port + 1} state {p.state} (want human)")
        if p.character != ck:
            out.append(f"port {port + 1} character {p.character:#x} (want {ck:#x})")
        if p.stocks != stocks:
            out.append(f"port {port + 1} stocks {p.stocks} (want {stocks})")
    return out


# ---- (b) controller-input recipes (closed loop) ---------------------------------------------


def boot_to_css(drv: Driver, port: int = 0, timeout_frames: int = 60 * 90) -> SceneInfo:
    """Boot -> Versus CSS. P+ 'Boot Directly to CSS v5.4' (BootToCSS.asm, hook @0x806DD5F8) starts
    sqVsMelee from sqBoot unless a pad holds L/R (Training), Start (title) or Z (Replays) at that
    moment, so: never hold those; tap A on the strap/boot scenes (strap pages, and P+'s 'Create
    save file?' prompt when the NAND has no Brawl save). From the title or main menu, A walks
    Group > Brawl (orca pplus-1v1.txt)."""
    last = [0]

    def nudge(info: SceneInfo) -> None:
        now = polls(drv)
        if now - last[0] < 40:
            return
        last[0] = now
        if info.scene in (Scene.STRAP, Scene.BOOT, Scene.TITLE, Scene.MAIN_MENU, Scene.PRIZE, Scene.UNKNOWN):
            tap(drv, port, ["A"], hold=3, release=3)
        elif info.scene is Scene.RESULTS:
            tap(drv, port, ["A"], hold=3, release=3)

    neutral(drv, port)
    return wait_scene(drv, [Scene.CSS], timeout_frames, every=4, on_frame=nudge)


def wait_css_ready(drv: Driver, ports: Sequence[int], timeout_frames: int = 600) -> None:
    """Wait until the CSS task and the given ports' areas are readable, plus a short settle."""
    start = polls(drv)
    while True:
        if all(read_css_area(drv.read_mem, p) is not None for p in ports):
            step(drv, 20)
            return
        if polls(drv) - start > timeout_frames:
            raise RecipeError("CSS areas never became readable", read_scene(drv.read_mem))
        step(drv, 2)


def css_join(drv: Driver, port: int, pad_port: Optional[int] = None, tries: int = 6) -> CssArea:
    """Make ``port``'s panel human (area kind 1) by tapping A (orca pplus-1v1.txt joins with A)."""
    pp = port if pad_port is None else pad_port
    for _ in range(tries):
        a = read_css_area(drv.read_mem, port)
        if a is not None and a.kind == CSS_AREA_HUMAN:
            return a
        tap(drv, pp, ["A"], hold=3, release=10)
    a = read_css_area(drv.read_mem, port)
    if a is None or a.kind != CSS_AREA_HUMAN:
        raise RecipeError(f"port {port + 1} did not join the CSS", a)
    return a


def css_pick_character(drv: Driver, port: int, css_id: int, pad_port: Optional[int] = None,
                       timeout_frames: int = 1500) -> CssArea:
    """Closed loop: steer ``port``'s hand until the character under it (area+0x1B8) is ``css_id``
    and the hand is over the grid holding the token (hand target 7), wait until the hand is still,
    press A, verify the token is down on ``css_id``. Positions seen while scanning are cached in
    ``LEARNED_CSS`` so later picks steer directly. Never holds B (B held with the token in hand
    backs out of the CSS after ~31 frames)."""
    pp = port if pad_port is None else pad_port
    start = polls(drv)
    a = css_join(drv, port, pp)
    if a.placed and a.character == css_id:
        return a
    if a.placed:  # token down on something else: one-frame B takes it back up
        tap(drv, pp, ["B"], hold=1, release=8)
    route = _serpentine(CSS_SCAN_X, CSS_SCAN_Y)
    wp = 0
    prev: Optional[Tuple[float, float]] = None
    stuck = 0
    while polls(drv) - start < timeout_frames:
        a = read_css_area(drv.read_mem, port)
        if a is None:
            raise RecipeError("left the CSS while picking", read_scene(drv.read_mem))
        if a.placed:
            if a.character == css_id:
                neutral(drv, pp)
                return a
            tap(drv, pp, ["B"], hold=1, release=8)
            continue
        x, y = a.hand_x, a.hand_y
        if a.hand_target == CSS_HAND_GRID_HOLDING and a.character != CSS_NONE \
                and math.isfinite(x) and a.character not in LEARNED_CSS:
            LEARNED_CSS[a.character] = (x, y)
        on_target = a.hand_target == CSS_HAND_GRID_HOLDING and a.character == css_id
        still = prev is not None and abs(x - prev[0]) < 0.05 and abs(y - prev[1]) < 0.05
        if on_target and still:
            drv.pad_set(pp, buttons=(), main=(128, 128))
            tap(drv, pp, ["A"], hold=3, release=6)
            prev = None
            continue
        if on_target:
            LEARNED_CSS[css_id] = (x, y)
            drv.pad_set(pp, buttons=(), main=(128, 128))
        else:
            if css_id in LEARNED_CSS:
                tx, ty = LEARNED_CSS[css_id]
            else:
                tx, ty = route[wp % len(route)]
                moved = prev is not None and math.hypot(x - prev[0], y - prev[1]) > 0.2
                stuck = 0 if moved else stuck + 1
                if math.hypot(tx - x, ty - y) < 1.0 or stuck > 6:  # reached or clamped at an edge
                    wp, stuck = wp + 1, 0
                    if wp >= len(route) * 2:
                        break
            drv.pad_set(pp, buttons=(), main=steer_toward(x, y, tx, ty, full_tilt_at=3.0))
        prev = (x, y)
        step(drv, 1)
    neutral(drv, pp)
    raise RecipeError(f"could not place port {port + 1} on CSS id {css_id:#x}",
                      {"area": read_css_area(drv.read_mem, port), "learned": dict(LEARNED_CSS)})


def css_start(drv: Driver, port: int = 0, timeout_frames: int = 600) -> SceneInfo:
    """Tap Start until the game leaves the CSS (P+ needs >= 2 players in stock mode)."""
    last = [-1000]

    def press(info: SceneInfo) -> None:
        if info.scene is Scene.CSS and polls(drv) - last[0] >= 30:
            last[0] = polls(drv)
            tap(drv, port, ["START"], hold=3, release=3)

    return wait_scene(drv, [Scene.SSS, Scene.LOADING, Scene.IN_MATCH], timeout_frames, every=2, on_frame=press)


def sss_pick_stage(drv: Driver, stage_kind: int, port: int = 0, timeout_frames: int = 1800) -> SssView:
    """Closed loop on P+'s stage select: map ``stage_kind`` to (page, position) through P+'s live
    page tables (RSS_PAGES), steer the game cursor (task+0x200 -> +0x3C/+0x40) until the page
    position under it (task+0x248) matches, wait until still, press A, and confirm the screen took
    the stage (task+0x224 == 2, +0x258). Positions seen are cached in ``LEARNED_SSS``."""
    targets = stage_positions(stage_kind, drv.read_mem)
    if not targets:
        raise RecipeError(f"stage kind {stage_kind:#x} is not on any P+ stage page")
    start = polls(drv)
    while polls(drv) - start < 300:
        v = read_sss(drv.read_mem)
        if v is not None and v.clock >= 40:
            break
        step(drv, 2)
    route = _serpentine(SSS_SCAN_X, SSS_SCAN_Y)
    wp, stuck = 0, 0
    prev: Optional[Tuple[float, float]] = None
    last: Optional[SssView] = None
    pressed = False
    while polls(drv) - start < timeout_frames:
        v = read_sss(drv.read_mem)
        if v is None:
            info = read_scene(drv.read_mem)
            if info.scene in (Scene.LOADING, Scene.IN_MATCH):
                # The screen exits ~6 frames after taking a stage, which can be inside our A tap.
                if not pressed:
                    raise RecipeError("left the SSS without our A press", info)
                return _confirm_stage(drv, stage_kind, last)
            step(drv, 1)
            continue
        last = v
        if v.state == SSS_STATE_TAKEN:
            neutral(drv, port)
            return _confirm_stage(drv, stage_kind, v)
        x, y = v.cursor_x, v.cursor_y
        pos = sss_position_under_cursor(v)
        if pos >= 0 and (v.page, pos) not in LEARNED_SSS:
            LEARNED_SSS[(v.page, pos)] = (x, y)
        page_targets = [t for t in targets if t[0] == v.page] or targets
        on_target = (v.page, pos) in page_targets
        still = prev is not None and abs(x - prev[0]) < 0.05 and abs(y - prev[1]) < 0.05
        if on_target and still:
            pressed = True
            tap(drv, port, ["A"], hold=3, release=6)
            prev = None
            continue
        if on_target:
            drv.pad_set(port, buttons=(), main=(128, 128))
        else:
            known = [LEARNED_SSS[t] for t in page_targets if t in LEARNED_SSS]
            if known:
                tx, ty = known[0]
            else:
                tx, ty = route[wp % len(route)]
                moved = prev is not None and math.hypot(x - prev[0], y - prev[1]) > 0.2
                stuck = 0 if moved else stuck + 1
                if math.hypot(tx - x, ty - y) < 1.5 or stuck > 6:
                    wp, stuck = wp + 1, 0
                    if wp >= len(route) * 2:
                        break
            drv.pad_set(port, buttons=(), main=steer_toward(x, y, tx, ty, full_tilt_at=4.0))
        prev = (x, y)
        step(drv, 1)
    neutral(drv, port)
    raise RecipeError(f"could not pick stage {stage_kind:#x} (targets {targets})",
                      {"sss": read_sss(drv.read_mem), "learned": dict(LEARNED_SSS)})


def _confirm_stage(drv: Driver, stage_kind: int, view: Optional[SssView]) -> Optional[SssView]:
    """After A on the SSS: the taken kind (task+0x258) if still readable, else the match setup's
    stage once it is written. Raises on a mismatch."""
    if view is not None and view.state == SSS_STATE_TAKEN and view.taken_kind not in (0, 0xFFFFFFFF)             and (view.taken_kind & 0xFF) != stage_kind:
        raise RecipeError(f"SSS took stage {view.taken_kind:#x}, wanted {stage_kind:#x}", view)
    return view


def wait_match_start(drv: Driver, timeout_frames: int = 60 * 30) -> MatchState:
    """Wait for scMelee and the end of READY/GO (frames elapsed > 0 or m_isStart)."""
    wait_scene(drv, [Scene.IN_MATCH], timeout_frames, every=4)
    start = polls(drv)
    while polls(drv) - start < timeout_frames:
        st = read_match_state(drv.read_mem)
        if st.in_match and (st.started or (st.frames_elapsed or 0) > 0):
            return st
        step(drv, 4)
    raise RecipeError("match never started", read_match_state(drv.read_mem))


def wait_match_end(drv: Driver, timeout_frames: int = 60 * 60 * 9) -> MatchState:
    """Wait for game set or for scMelee to end."""
    start = polls(drv)
    while polls(drv) - start < timeout_frames:
        st = read_match_state(drv.read_mem)
        if not st.in_match or st.game_set:
            return st
        step(drv, 10)
    raise RecipeError("match did not end", read_match_state(drv.read_mem))


def results_to_css(drv: Driver, port: int = 0, timeout_frames: int = 60 * 30) -> SceneInfo:
    """Leave scVsResult with A taps (then Start, orca inputs) and land on the CSS again."""
    last = [-1000]
    n = [0]

    def press(info: SceneInfo) -> None:
        if info.scene is Scene.RESULTS and polls(drv) - last[0] >= 40:
            last[0] = polls(drv)
            n[0] += 1
            tap(drv, port, ["A"] if n[0] % 3 else ["START"], hold=3, release=3)

    return wait_scene(drv, [Scene.CSS], timeout_frames, every=4, on_frame=press)


def start_match_local(drv: Driver, characters: Mapping[int, str], stage: str = "battlefield",
                      stocks: int = 4, minutes: int = 8, force_rules: bool = False) -> MatchState:
    """One instance driving every port: boot (or results/menus) -> CSS -> pick ``characters``
    ({port: name}) -> Start -> SSS -> ``stage`` -> wait for GO -> verify the match setup.

    For netplay, run ``css_pick_character`` / ``sss_pick_stage`` on each instance for its own port
    concurrently (threads), then only the host presses Start and picks the stage."""
    ports = sorted(characters)
    first = ports[0]
    info = read_scene(drv.read_mem)
    if info.scene is Scene.RESULTS:
        results_to_css(drv, first)
    elif info.scene is not Scene.CSS:
        boot_to_css(drv, first)
    if force_rules:
        write_rules(drv, stocks, minutes)
    wait_css_ready(drv, ports)
    for p in ports:
        css_pick_character(drv, p, CSS_ID[characters[p]])
    css_start(drv, first)
    sss_pick_stage(drv, STAGE_KIND[stage], first)
    st = wait_match_start(drv)
    want = {p: CSS_TO_CHAR_KIND[CSS_ID[characters[p]]] for p in ports}
    problems = verify_match_setup(drv.read_mem, want, STAGE_KIND[stage], stocks, minutes)
    if problems:
        raise RecipeError("match setup mismatch: " + "; ".join(problems), st)
    return st


# =============================================================================================
# Live-verification helper
# =============================================================================================


def probe(read_mem: ReadMem) -> Dict[str, Any]:
    """One snapshot of every parser, for checking the 'live' addresses on a running game."""
    m = Mem(read_mem)
    out: Dict[str, Any] = {"scene": read_scene(read_mem), "frames": read_frame_counters(read_mem)}
    out["g_ftEntryManager"] = _try(lambda: hex(m.u32(G_FT_ENTRY_MANAGER_PTR)))
    out["set_rule"] = read_set_rule(read_mem)
    out["match_setup"] = read_match_setup(read_mem)
    out["css"] = [read_css_area(read_mem, p) for p in range(4)]
    out["sss"] = read_sss(read_mem)
    out["players"] = read_players(read_mem)
    out["stage_pages"] = read_stage_pages(read_mem)
    out["heaps"] = read_heap_table(read_mem)
    out["banned"] = check_banned(read_mem)
    out["brawlback_checksum"] = brawlback_checksum(read_mem)
    return out
