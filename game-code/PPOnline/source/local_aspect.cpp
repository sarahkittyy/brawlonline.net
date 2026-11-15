// Any screen shape (docs/game-code.md, Any screen shape): LOCAL.aspect (Dolphin, every frame) is the
// shape this machine shows the picture at, and the game draws for it. A screen wider than the
// cameras' 16:9 (21:9) shows more of the stage at the sides, a narrower one (4:3, 16:10) more above
// and below; the 16:9 picture every machine would draw is the middle of it, with the HUD in it, and
// nothing is stretched.
//
// Only the last step of drawing changes: the projection a camera hands to the graphics chip.
// Brawl's cameras (gfCameraManager's, the camera controllers, the scene roots' g3d cameras) are
// the match's state: the camera frames the fighters with its aspect, and a screen KO places the
// fighter from the camera, which is why every machine plays with the same 16:9 ones
// (PinScreenSetting, OnlineMatch::setupMatch). They are not touched, nor read differently: the
// two functions that send a camera's projection to GX (GXSetProjection keeps its six numbers in
// the GX state, drawn with at the next primitive) run as they are, and the numbers are then scaled
// there: x by 1/ratio on a wider screen, y by ratio on a narrower one. The 3D scene, the HUD's
// models, the name tags over the fighters, the pause screen and the menus are all drawn in
// perspective through those two, so they come out in place with one rule; the bubble of a fighter
// off the 16:9 picture sits at that picture's edge, where the match's camera puts it.
//
// One thing does not belong in the middle: the HUD's own edges. On a narrower screen the 16:9
// picture is a band across the middle, and the damage panels and the timer would hang where its
// bottom and top are, away from the screen's. In a match they are drawn at the screen's edges
// instead: the routine every model is drawn with (DrawResMdlDirectly) is wrapped, and for a HUD
// model that sits near the picture's bottom or top the projection in the GX state is moved down or
// up by the band's distance from the edge for that one draw. A HUD model is one with MuObject's
// own callback; the name tag and the bubble that follow a fighter have IfPlayer's, and stay with
// the fighter. Which HUD models: the timer's, and a player's panel pieces (the models its IfPlayer
// holds) that sit at the picture's bottom. Again only the GX state is written.
#include "local_aspect.h"
#include "online.h"
#include "ppom.h"

namespace LocalAspect {

    // The GX state (*(r2 - 0x5B70), GXSetProjection 0x801F50EC / GXSetViewport 0x801F5484): the
    // projection's kind at +0x528 (0 perspective, 1 orthographic), its six numbers at +0x52C
    // ({m00, m02 or m03, m11, m12 or m13, m22, m23}) and the viewport's width and height at
    // +0x54C / +0x550.
    static const u32 GX_PROJECTION_KIND = 0x528, GX_PROJECTION = 0x52C, GX_VIEWPORT_SIZE = 0x54C;
    // +0x5FC: what GX sends before the next primitive; GXSetProjection sets this bit.
    static const u32 GX_DIRTY = 0x5FC, GX_DIRTY_PROJECTION = 0x08000000;

    typedef void (*CameraFn)(void* camera);
    static CameraFn s_origSetGX;          // gfCamera::setGX 0x80018DE4
    static CameraFn s_origG3dProjection;  // nw4r::g3d::Camera::GXSetProjection 0x801A80CC
    // nw4r::g3d::DrawResMdlDirectly 0x801A64B0 (eight arguments, all in registers): the seventh
    // is the model's own draw data, ScnMdl +0x144, when a ScnMdl draws (0 from a ScnMdlSimple).
    typedef void (*DrawFn)(void*, void*, void*, void*, void*, void*, u8*, void*);
    static DrawFn s_origDraw;
    static const u32 SCN_MDL_DRAW_DATA = 0x144, SCN_MDL_VTABLE = 0x804663C8;
    // ScnObj: its view matrix at +0x6C (3x4: y at +0x1C, z at +0x2C), its callback at +0xD4.
    static const u32 SCN_VIEW_Y = 0x6C + 0x1C, SCN_VIEW_Z = 0x6C + 0x2C, SCN_CALLBACK = 0xD4;
    static const u32 MU_OBJECT_CALLBACK_VTABLE = 0x80455D18;   // the callback every MuObject model has

    // This frame's (frame(), once a frame): the screen's width over the width the cameras draw
    // for, 1 = draw as the game does; and in a match on a narrower screen how far the 16:9
    // picture's top and bottom are from the screen's, in GX's -1..1, else 0. And how far edge()
    // has moved the projection.
    static float s_ratio = 1.0f;
    static float s_edge;
    static float s_moved;
    // The timer's models this frame: g_IfMngr (*0x805A02D0) +0x44 is the HUD's centre object, whose
    // first seven MuObjects (+0x10..+0x28; a MuObject's ScnMdl is its +0xC) are the timer's digits
    // and its colon. The colon's model is placed at the picture's centre and drawn up there, so
    // where a model is does not say it is the timer's.
    static const u32 IF_MNGR = 0x805A02D0, MNGR_CENTRE = 0x44, CENTRE_TIMER = 0x10, MU_OBJECT_MODEL = 0xC;
    static const int TIMER_MODELS = 7;
    static u32 s_timer[TIMER_MODELS];

    // A player's panel: g_IfMngr +0x4C..+0x58 are the four IfPlayers, whose MuObjects are its
    // +0x28..+0xEC (the damage and its digits, the portrait, the stock icons and count; and the
    // name tag, the bubble and the arrow that follow the fighter, which have IfPlayer's callbacks).
    static const u32 MNGR_PLAYERS = 0x4C, PLAYER_OBJECTS = 0x28, PLAYER_OBJECTS_END = 0xF0;
    static const u32 CALLBACK_OBJECT = 0x8;   // a MuObject's callback's MuObject

    using Online::isPtr;

    static void findTimer()
    {
        for (int i = 0; i < TIMER_MODELS; i++) s_timer[i] = 0;
        const u32 mngr = *(u32*)IF_MNGR;
        const u32 centre = isPtr(mngr) ? *(u32*)(mngr + MNGR_CENTRE) : 0;
        if (!isPtr(centre)) return;
        for (int i = 0; i < TIMER_MODELS; i++) {
            const u32 object = *(u32*)(centre + CENTRE_TIMER + 4 * i);
            if (isPtr(object)) s_timer[i] = *(u32*)(object + MU_OBJECT_MODEL);
        }
    }

    // gfCameraManager (*0x8059FFC0, made at boot): +0x738 the aspect its cameras are set for (16/9;
    // 4/3 with Options > Screen > Standard on the menus) and +0x73C the video mode's scale (the
    // picture's width of the screen's, 0.974 in P+): their product is the shape the cameras draw for.
    static const u32 CAMERA_MANAGER = 0x8059FFC0, MANAGER_ASPECT = 0x738;

    // The screen's width over the width the cameras draw for: 1 = draw as the game does.
    static float screenOverGame()
    {
        const float screen = PPOM::g_block.local.aspect;
        const float* game = *(const float**)CAMERA_MANAGER;
        if (!(screen > 0.0f) || !game) return 1.0f;   // 0: no Dolphin value (NaN fails too)
        game = (const float*)((const u8*)game + MANAGER_ASPECT);
        const float ratio = screen / (game[0] * game[1]);
        // Dolphin's own 16:9 modes show the picture within 1% of the cameras' shape (P+'s 69:40
        // against 1.732): that is the game's picture, drawn as it always was. And no screen is
        // half as wide or three times (32:9 is 2.05).
        if (!(ratio >= 0.5f && ratio <= 3.0f) || (ratio > 0.99f && ratio < 1.01f)) return 1.0f;
        return ratio;
    }

    static void fitProjection(bool cameras)
    {
        const float aspect = s_ratio;
        if (aspect == 1.0f) return;
        u8* gx;
        asm volatile("lwz %0, -0x5b70(2)" : "=r"(gx));
        // A camera's orthographic projection is left as it is: the scene, the HUD and the menus
        // are all drawn in perspective, and what the game draws flat through a camera is a copy of
        // the screen (the fades between scenes), which covers the screen as it is.
        if (cameras && *(u32*)(gx + GX_PROJECTION_KIND) != 0) return;
        // The whole picture only: a camera drawing into a smaller viewport draws a texture.
        const float* size = (const float*)(gx + GX_VIEWPORT_SIZE);
        if (size[0] < 600.0f || size[1] < 440.0f) return;
        float* p = (float*)(gx + GX_PROJECTION);
        if (aspect > 1.0f) {
            const float k = 1.0f / aspect;
            p[0] *= k;
            p[1] *= k;
        } else {
            p[2] *= aspect;
            p[3] *= aspect;
        }
    }

    // Moves the projection in the GX state up (positive) or down by `by` of GX's -1..1. A
    // perspective projection's fourth number is subtracted from y / -z, an orthographic one's
    // added to y.
    static void moveProjection(float by)
    {
        u8* gx;
        asm volatile("lwz %0, -0x5b70(2)" : "=r"(gx));
        float* p = (float*)(gx + GX_PROJECTION);
        if (*(u32*)(gx + GX_PROJECTION_KIND) == 0) p[3] -= by;
        else p[3] += by;
        *(u32*)(gx + GX_DIRTY) |= GX_DIRTY_PROJECTION;
    }

    void frame()
    {
        s_ratio = screenOverGame();
        s_edge = s_ratio < 1.0f && Online::inScene("scMelee") ? 1.0f - s_ratio : 0.0f;
        if (s_edge != 0.0f) findTimer();
    }

    float wider() { return s_ratio > 1.0f ? s_ratio : 1.0f; }

    void fit()
    {
        fitProjection(false);
        s_moved = 0.0f;
    }

    void edge(int side)
    {
        const float to = (float)side * s_edge;
        if (to == s_moved) return;
        moveProjection(to - s_moved);
        s_moved = to;
    }

    // Where a model drawn now belongs on a narrower screen: -1 the bottom edge, 1 the top, 0 where
    // it is. The top: the timer's models. The bottom: a player's panel pieces, which are the
    // models of its IfPlayer's MuObjects that sit at the 16:9 picture's bottom (an IfPlayer's
    // other pieces, and every other HUD object's models wherever they are, the countdown and
    // GAME! too, stay in the picture). The HUD's models are in the HUD camera's space; the
    // picture's half height there is the depth over the projection's y scale (the GX state's,
    // which fitProjection made smaller by the band's height).
    static int edgeOf(const u8* scn)
    {
        if (!isPtr((u32)scn) || *(u32*)scn != SCN_MDL_VTABLE) return 0;
        const u32 callback = *(u32*)(scn + SCN_CALLBACK);
        if (!isPtr(callback) || *(u32*)callback != MU_OBJECT_CALLBACK_VTABLE) return 0;
        for (int i = 0; i < TIMER_MODELS; i++) {
            if ((u32)scn == s_timer[i]) return 1;
        }
        u8* gx;
        asm volatile("lwz %0, -0x5b70(2)" : "=r"(gx));
        if (*(u32*)(gx + GX_PROJECTION_KIND) != 0) return 0;
        const float depth = -*(const float*)(scn + SCN_VIEW_Z);
        if (!(depth > 1.0f)) return 0;
        // y in the 16:9 picture's -1..1: the damage panels are at -0.71, the stock icons at -0.67.
        const float y = *(const float*)(scn + SCN_VIEW_Y) * ((const float*)(gx + GX_PROJECTION))[2] /
                        (depth * s_ratio);
        if (!(y < -0.5f)) return 0;
        const u32 object = *(u32*)(callback + CALLBACK_OBJECT);
        const u32 mngr = *(u32*)IF_MNGR;
        if (!isPtr(mngr)) return 0;
        for (u32 i = 0; i < 4; i++) {
            const u32 player = *(u32*)(mngr + MNGR_PLAYERS + 4 * i);
            if (!isPtr(player)) continue;
            for (u32 at = PLAYER_OBJECTS; at < PLAYER_OBJECTS_END; at += 4) {
                if (*(u32*)(player + at) == object) return -1;
            }
        }
        return 0;
    }

    static void hkDraw(void* a, void* b, void* c, void* d, void* e, void* f, u8* drawData, void* h)
    {
        const int side = s_edge != 0.0f ? edgeOf(drawData - SCN_MDL_DRAW_DATA) : 0;
        if (side == 0) {
            s_origDraw(a, b, c, d, e, f, drawData, h);
            return;
        }
        moveProjection((float)side * s_edge);
        s_origDraw(a, b, c, d, e, f, drawData, h);
        moveProjection(-(float)side * s_edge);
    }

    static void hkSetGX(void* camera)
    {
        s_origSetGX(camera);
        fitProjection(true);
    }

    static void hkG3dProjection(void* camera)
    {
        s_origG3dProjection(camera);
        // A Camera without its data sets nothing (the function returns at once): the projection
        // in the GX state is then an earlier one, already fitted.
        if (*(void**)camera) fitProjection(true);
    }

    void install(CoreApi* api)
    {
        api->syReplaceFunc(0x80018DE4, reinterpret_cast<void*>(hkSetGX), (void**)&s_origSetGX);
        api->syReplaceFunc(0x801A80CC, reinterpret_cast<void*>(hkG3dProjection), (void**)&s_origG3dProjection);
        api->syReplaceFunc(0x801A64B0, reinterpret_cast<void*>(hkDraw), (void**)&s_origDraw);
    }
}
