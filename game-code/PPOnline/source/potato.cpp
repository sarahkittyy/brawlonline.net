// Potato mode (docs/game-code.md, Potato mode): with LOCAL.potato set (Dolphin, from the launcher's
// setting), a match draws no shadows and none of the listed stage pieces (backgrounds and scenery).
// The stage the fighters stand on, the fighters, effects and the HUD look as usual.
//
// Only drawing changes. The shadow pass and the hidden models only read the game state, so the
// machines' gameplay stays the same whatever each one draws: the sessions compare the gameplay
// checksum (fighters, RNGs, frame), never these bytes. The switches are the game's own: the scene
// root's shadow bit (the game itself runs without the shadow pass when a stage leaves it clear)
// and nw4r's per-model "don't draw" flags. Both live in rolled-back memory, so tick() sets them
// again every frame of a match (a rollback to before the first frame would bring them back) and
// writes only when one is missing.
#include <string.h>

#include "potato.h"
#include "online.h"
#include "ppom.h"

namespace Potato {

    // g_taskScheduler (.sbss 0x805A0068): gfTaskScheduler's 14 render lists at +0xF8, linked
    // through gfTask+0x18 (gfTaskScheduler::render, decomp gf_task_scheduler.cpp). A gfTask's
    // name is at +0x00 and its category in bits 7-14 of the word at +0x2C.
    static const u32 TASK_SCHEDULER = 0x805A0068;
    static const u32 SCHED_RENDER_LISTS = 0xF8;
    static const int RENDER_LISTS = 14;
    static const u32 TASK_NAME = 0x00, TASK_RENDER_NEXT = 0x18, TASK_BITS = 0x2C;
    static const u32 CATEGORY_GROUND = 0x06;      // gfTask::Category_Ground
    // Ground::m_sceneModels (+0x44): the stage piece's models, in a row.
    static const u32 GROUND_MODELS = 0x44;
    static const int GROUND_MODELS_MAX = 8;
    // The models stage pieces are made of (the vtable of gfModel's nw4r::g3d::ScnMdl). A slot
    // holding anything else ends the row.
    static const u32 SCN_MDL_VTABLE = 0x804663C8;
    // ScnMdl +0xE8: its ResMdl, the stage file's MDL0 block ("MDL0", size, version; the name's
    // offset, from the block's start, follows the section offsets: 11 of them up to version 9,
    // 14 from version 10).
    static const u32 SCN_MDL_RES = 0xE8, MDL0_MAGIC = 0x4D444C30;
    // nw4r::g3d::ScnObj flags (+0xCC): FLAG_DISABLE_DRAW_OPA 0x20 | FLAG_DISABLE_DRAW_XLU 0x40.
    static const u32 SCNOBJ_FLAGS = 0xCC, NO_DRAW = 0x60;
    // g_gfSceneRoot (.sbss 0x8059FF80): +0x464 bit 0x40 = draw the shadow pass (fn_8000F4F0,
    // which gfSceneRoot::renderXlu calls at 0x8000E2A8 only when set). The stage sets it at load.
    static const u32 SCENE_ROOT = 0x8059FF80, ROOT_SHADOW = 0x464, SHADOW_ON = 0x40;

    // Stage pieces that are only background or scenery, each checked in a screenshot with it
    // hidden (P+ v3.2.1's legal stages): the Ground task's name and the model's MDL0 name. P+
    // builds many stages on Final Destination's module, so a ground name alone says little
    // ("grFinalMainBg" is the space behind Final Destination and the floor of another stage);
    // with the model's name a stage or an alternate (ASL) with other models hides nothing.
    struct Hide {
        const char* ground;
        const char* model;
    };
    static const Hide HIDE[] = {
        // Battlefield: the sky and everything in it, the distant land.
        {"grBattleFieldSky", "aStgBattleFieldSky"},
        {"grBattleFieldSun", "bStgBattleFieldTaiyo"},
        {"grBattleFieldMoon", "bStgBattleFieldTsuki"},
        {"grBattleFieldStar", "bStgBattleFieldHoshi"},
        {"grBattleFieldCloud", "cStgBattleFieldCloud"},
        {"grBattleFieldMainBg", "yStgBattleFieldEnkei"},
        {"grBattleFieldFlare", "zzStgBattleFieldFlare"},
        // Final Destination: the space behind the stage.
        {"grFinalMainBg", "StgFinal00space"},
        // P+'s stage on Delfino's slot (0x03): the wall behind the stage, the sky.
        {"grFinalStage", "BackGround"},
        {"grPokeTrainer", "Skybox"},
        // Luigi's Mansion: the moon and sky, King Boo behind the stage.
        {"grFinalMainBg", "Background"},
        {"grPokeTrainer", "KingBoo"},
        // P+'s stage on Mushroomy Kingdom's slot (0x05): the cave behind the stage.
        {"grFinalMainBg", "Background_Static"},
        {"grFinalMainBg", "Background_Deco"},
        // Bowser's Castle: the background's Bowser and Piranha Plant.
        {"grBowserCastleBackground", "Koopa"},
        {"grBowserCastleBackground", "pPlant"},
        // Temple of Time: the temple around the stage, its light rays and embers.
        {"grFinalMainBg", "TempleOfTime"},
        {"grFinalMainBg", "Rays"},
        {"grFinalMainBg", "Embers"},
        // Frigate Husk: the sky and planet, the husk behind the stage, its glow and stars.
        {"grFinalMainBg", "Sky"},
        {"grFinalMainBg", "Husk"},
        {"grFinalMainBg", "Glow"},
        {"grFinalMainBg", "BigStars"},
        // Yoshi's Story (crayon): the sky, the clouds, the hills behind the stage.
        {"grCrayonStatic", "StgCrayon00F"},
        {"grCrayonKumom", "StgCrayon00K"},
        {"grCrayonSeason", "StgCrayon00N"},
        // Fountain of Dreams: the scenery around the stage, the water's reflection. Not its Sky:
        // without it the stage's mint clear colour shows as a flat void (2026-11-01).
        {"grIzumiMainBg", "BG"},
        {"grIzumiMainBg", "MainRing"},
        {"grIzumiMainBg", "StgDXFountain_Reflection_V3"},
        // Smashville: the town and its lit windows, the sky, the passers-by.
        {"grVillageMainBg", "yStgVillageChikeAll"},
        {"grVillageMainBgLight", "StgVillageMadoHikari"},
        {"grVillageSky", "aStgVillageSkyShinya"},
        {"grVillagePerio", "StgVillagePerio"},
        {"grVillageTaxi", "StgVillageTaxi"},
        {"grVilageUFO", "StgVillageUFO"},
        {"grVillageLiveDeco", "yStgVillageLiveKazari"},
        {"grVillageClock", "StgVillageTokei"},
        // Green Hill Zone: the land behind the stage and the friends running in it.
        {"grGreenhillMainBg", "StgGreenhillBack"},
        {"grGreenhillKnuckles", "StgGreenhillAmy"},
        {"grGreenhillSilver", "StgGreenhillSilver"},
        {"grGreenhillTails", "StgGreenhillTails"},
        // Dream Land: the land and sea behind the stage, the floating stars.
        {"grPokeTrainer", "StgDxGreensEnkei"},
        {"grDxGreensEnkei", "StarsPls"},
        // Pokémon Stadium 2: the big screen, the searchlights.
        {"grStadiumAuroraVision", "StgStadium00AuroraVision"},
        {"grStadiumSearchLight", "StgStadium00SeachLight"},
    };
    static const int HIDE_COUNT = sizeof(HIDE) / sizeof(HIDE[0]);

    // The models found for the stage's grounds (matched once per set of grounds, by name).
    static const int HIDDEN_MAX = 32;
    static u32 s_hidden[HIDDEN_MAX];
    static int s_hiddenCount = 0;
    static u32 s_groundsKey = 0;   // walkGrounds' key of the grounds the list is for

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    static bool isModel(u32 mdl) { return isPtr(mdl) && *(u32*)mdl == SCN_MDL_VTABLE; }

    static const char* modelName(u32 mdl)
    {
        u32 res = *(u32*)(mdl + SCN_MDL_RES);
        if (!isPtr(res) || *(u32*)res != MDL0_MAGIC) return NULL;
        u32 sections = *(u32*)(res + 8) >= 10 ? 14 : 11;
        u32 off = *(u32*)(res + 0x10 + 4 * sections);
        // The name is in the stage file's string table, after the MDL0 block.
        if (off == 0 || off >= 0x01000000 || !isPtr(res + off)) return NULL;
        return (const char*)(res + off);
    }

    static void addMatches(u32 ground, const char* groundName)
    {
        u32 list = *(u32*)(ground + GROUND_MODELS);
        if (!isPtr(list)) return;
        for (int i = 0; i < GROUND_MODELS_MAX; i++) {
            u32 mdl = ((u32*)list)[i];
            if (!isModel(mdl)) break;
            const char* name = modelName(mdl);
            if (!name) continue;
            for (int h = 0; h < HIDE_COUNT && s_hiddenCount < HIDDEN_MAX; h++) {
                if (strcmp(groundName, HIDE[h].ground) == 0 && strcmp(name, HIDE[h].model) == 0) {
                    s_hidden[s_hiddenCount++] = mdl;
                    break;
                }
            }
        }
    }

    static int modelCount(u32 ground)
    {
        u32 list = *(u32*)(ground + GROUND_MODELS);
        if (!isPtr(list)) return 0;
        int n = 0;
        while (n < GROUND_MODELS_MAX && isModel(((u32*)list)[n])) n++;
        return n;
    }

    // Walks the render lists' grounds: with `match`, collects the models to hide. Returns a key
    // of the grounds and their models: a stage's grounds come first and get their models over the
    // next frames of its load, so the key changes until all are there.
    static u32 walkGrounds(u32 sched, bool match)
    {
        u32 key = 0, count = 0;
        for (int l = 0; l < RENDER_LISTS; l++) {
            u32 t = *(u32*)(sched + SCHED_RENDER_LISTS + 4 * l);
            for (int n = 0; isPtr(t) && n < 512; n++, t = *(u32*)(t + TASK_RENDER_NEXT)) {
                if (((*(u32*)(t + TASK_BITS) >> 7) & 0xFF) != CATEGORY_GROUND) continue;
                count++;
                key = (key * 31) ^ t ^ (u32)modelCount(t);
                u32 name = *(u32*)(t + TASK_NAME);
                if (match && isPtr(name)) addMatches(t, (const char*)name);
            }
        }
        return key ^ (count << 24);
    }

    void tick()
    {
        if (!PPOM::g_block.local.potato || !Online::inScene("scMelee")) {
            s_groundsKey = 0;
            s_hiddenCount = 0;
            return;
        }
        u32 root = *(u32*)SCENE_ROOT;
        if (isPtr(root)) {
            u8* shadow = (u8*)(root + ROOT_SHADOW);
            if (*shadow & SHADOW_ON) *shadow &= ~SHADOW_ON;
        }
        u32 sched = *(u32*)TASK_SCHEDULER;
        if (!isPtr(sched)) return;
        u32 key = walkGrounds(sched, false);
        if (key != s_groundsKey) {   // the stage's grounds or their models came: match again
            s_groundsKey = key;
            s_hiddenCount = 0;
            walkGrounds(sched, true);
        }
        for (int i = 0; i < s_hiddenCount; i++) {
            if (!isModel(s_hidden[i])) continue;
            u32* flags = (u32*)(s_hidden[i] + SCNOBJ_FLAGS);
            if ((*flags & NO_DRAW) != NO_DRAW) *flags |= NO_DRAW;
        }
    }
}
