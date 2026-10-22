/****************************************************************************/
//    Copyright (C) 2026 Final Fantasy Universe                             //
//                                                                          //
//    This file is part of FFNx                                             //
//                                                                          //
//    FFNx is free software: you can redistribute it and/or modify          //
//    it under the terms of the GNU General Public License as published by  //
//    the Free Software Foundation, either version 3 of the License         //
//                                                                          //
//    FFNx is distributed in the hope that it will be useful,               //
//    but WITHOUT ANY WARRANTY; without even the implied warranty of        //
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         //
//    GNU General Public License for more details.                          //
/****************************************************************************/

// Final Fantasy Universe: the battle look (docs/battle-look.md), the hub's pause-menu switches for the games' battles.
//
// The bridge passes the hub's `look` line on as `ffu_look(flags)` from its own thread; it is applied on the game thread
// once a frame (ffuLookFrame, from common_flip before FFNx's lighting draws).
//
// Lighting (FF7): FFNx's own lighting (enable_lighting) already lights FF7's battles and casts their shadows. The look
// gives it the shared engine's balance, the same as FF9's battles (mods/ff9-globe BattleScene.cs): matte surfaces
// (metallic 0, low specular, rough), the main light 3.8 times the ambient, about the overall brightness FFNx's installed
// config gives (its light 4.0 at metallic 0.5 and ambient 1.0: a surface half turned to the light keeps its colour).
// Off: FFNx's installed config again (lighting/config.toml, its per-battle groups too). Nothing changes outside
// battles, and nothing at all when FFNx's lighting wasn't on at start (it is set up only then).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "cfg.h"
#include "common.h"
#include "globals.h"
#include "lighting.h"
#include "log.h"
#include "renderer.h"

namespace
{
    constexpr int LOOK_LIGHTING = 1, LOOK_GLOW = 2, LOOK_SHARP = 4, LOOK_CAMERA = 8, LOOK_ATMOSPHERE = 16, LOOK_CINEMA = 32;
    // (the spells' light, Richard's next list item 1: off unless told, unlike the rest, so an older bridge never turns it on)
    constexpr int LOOK_SPELL = 64;
    // (Richard's next list, item 2: rim light and softer shadows; off unless told, as the spells' light)
    constexpr int LOOK_RIM = 128, LOOK_SOFT = 256;
    // (item 3: the impact, off unless told)
    constexpr int LOOK_IMPACT = 512;
    // (item 3's cinematics: depth of field and motion blur while a summon or Limit plays, off unless told)
    constexpr int LOOK_DOF = 1024, LOOK_MOTION = 2048;
    std::atomic<float> spellLevel{1.0f};   // its slider (ffu_look_spell)
    std::atomic<float> cinematicLevel{1.0f};   // the cinematics' slider (ffu_look_cinematic)
    // Richard's sliders (the hub's pause menu, Rule 105): multiples of the look as built: the main light, the light from
    // all around, contact shading, the glow, the haze (ffu_look_levels)
    std::atomic<float> levels[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

    std::atomic<int> requestedFlags{-1};   // -1: never told (the look's default: everything on)
    // what this frame goes by: requestedFlags taken once a frame (ffuLookFrame), so a switch told mid-frame never splits
    // a frame (review: the glow's mask on for half a frame's draws made the whole picture its light)
    std::atomic<int> frameFlags{-1};
    bool tuned = false;                     // the look's values are in FFNx's lighting now
    int lightingAtStart = -1;
    // FFNx's own values when the look took over (put back as the battle ends: FFNx reads its config again only at a new
    // field, not at the same one, and a reload would make its shadow map again: Sonnet review, 2026-10-22)
    float savedMetallic, savedSpecular, savedRoughness, savedLight, savedAmbient;
}

// flags: bit 0 the battle lighting (FF8's: ffu_ff8look.cpp), bit 1 the glow, bit 2 the anti-aliasing (both ffu_glow.cpp),
// bit 3 the stick camera (FF7's held across action cameras: ff7/battle/camera.cpp; FF8's is the bridge's own), bit 4 the
// atmosphere (the haze: ffu_ff8look.cpp, ffu_ff7look.cpp), bit 5 the cinematic bars (FF7's: ffu_ff7look.cpp).
extern "C" __declspec(dllexport) void ffu_look(int flags)
{
    requestedFlags.store(flags);
}

bool ffuLookGlow()
{
    int flags = frameFlags.load();
    return flags < 0 || (flags & LOOK_GLOW);
}

bool ffuLookSharp()
{
    int flags = frameFlags.load();
    return flags < 0 || (flags & LOOK_SHARP);
}

// The sliders, from the bridge's thread (0-2 each; 1: as built).
extern "C" __declspec(dllexport) void ffu_look_levels(float light, float fill, float contact, float glow, float haze)
{
    const float values[5] = { light, fill, contact, glow, haze };
    for (int i = 0; i < 5; i++)
        levels[i].store(values[i] >= 0.0f && values[i] <= 2.0f ? values[i] : 1.0f);
}

// 0 the main light, 1 the light from all around, 2 contact shading, 3 the glow, 4 the haze.
// The spells' light's slider (the bridges' look.c, from the hub's "spell_amount"): a multiple of the light as built, 0-2.
extern "C" __declspec(dllexport) void ffu_look_spell(float level)
{
    spellLevel.store(level >= 0.0f && level <= 2.0f ? level : 1.0f);
}

// The cinematics' slider (the bridges' look.c, from the hub's "cinematic_amount"): a multiple of their depth of field and
// motion blur as built, 0-2.
extern "C" __declspec(dllexport) void ffu_look_cinematic(float level)
{
    cinematicLevel.store(level >= 0.0f && level <= 2.0f ? level : 1.0f);
}

bool ffuLookSpell()
{
    int flags = frameFlags.load();
    return flags >= 0 && (flags & LOOK_SPELL);
}

bool ffuLookRim()
{
    int flags = frameFlags.load();
    return flags >= 0 && (flags & LOOK_RIM);
}

bool ffuLookSoftShadows()
{
    int flags = frameFlags.load();
    return flags >= 0 && (flags & LOOK_SOFT);
}

bool ffuLookImpact()
{
    int flags = frameFlags.load();
    return flags >= 0 && (flags & LOOK_IMPACT);
}

bool ffuLookDof()
{
    int flags = frameFlags.load();
    return flags >= 0 && (flags & LOOK_DOF);
}

bool ffuLookMotion()
{
    int flags = frameFlags.load();
    return flags >= 0 && (flags & LOOK_MOTION);
}

bool ffuFF7Cinematic();   // ffu_ff7look.cpp
bool ffuLookShown();

// The impact (Richard's next list, item 3: "a small camera shake ... on big hits and limits"; docs/battle-look.md
// "Impact"): a unit's HP falling by a tenth of its most or more between frames (a quarter: a hard one), and FF7's summons
// and Limits beginning, shake the battle's 3D picture in FF7's and FF8's passes (before their menus: those stay still).
// FF7's units: FFNx's battle_context's actor_vars (10, HP and its most); FF8's: 7 x 0xD0 from FFNx's
// battle_char_struct_dword_1D27B10, HP +0x18, its most +0x1C (the bridges' own reads: shadow_ff7.c, shadow_ff8.c). A
// battle's first 30 frames only take its units' HP (the last battle's, or a save's, left in the tables: review). FF9's is
// FFUGlobe's (BattleImpact.cs).
namespace
{
    constexpr float HIT = 0.1f, HARD_HIT = 0.25f, SHAKE = 0.006f, HARD_SHAKE = 0.012f, SHAKE_TIME = 0.3f, HARD_SHAKE_TIME = 0.45f;
    int impactHp[10];
    bool impactCinema = false;
    int impactFrames = 0;   // this battle's frames seen (0: none)
    constexpr int IMPACT_SETTLE = 30;
    ULONGLONG shakeFrom = 0;
    float shakeFor = 0.0f, shakeSize = 0.0f;

    float shakeAge()
    {
        return shakeFrom ? float(GetTickCount64() - shakeFrom) / 1000.0f : 1e9f;
    }

    void impactFrame()
    {
        if (!ffuLookImpact() || !ffuLookShown() || getmode_cached()->driver_mode != MODE_BATTLE)
        {
            impactFrames = 0;
            impactCinema = false;
            return;
        }
        const uint8_t *ff8Table = ff8 ? (const uint8_t *)ff8_externals.battle_char_struct_dword_1D27B10 : nullptr;
        if (ff8 ? !ff8Table : !ff7_externals.battle_context)
            return;
        static bool told = false;
        if (!told)
        {
            told = true;
            ffnx_info("FFU impact: the units' table at 0x%X\n", ff8 ? unsigned(uintptr_t(ff8Table)) : unsigned(uintptr_t(ff7_externals.battle_context->actor_vars)));
        }
        const int units = ff8 ? 7 : 10;
        const bool settled = impactFrames >= IMPACT_SETTLE;
        if (impactFrames < IMPACT_SETTLE) impactFrames++;
        float strongest = 0.0f;
        for (int i = 0; i < units; i++)
        {
            const int hp = ff8 ? *(const int *)(ff8Table + 0xD0 * i + 0x18) : ff7_externals.battle_context->actor_vars[i].currentHP;
            const int most = ff8 ? *(const int *)(ff8Table + 0xD0 * i + 0x1C) : ff7_externals.battle_context->actor_vars[i].maxHP;
            if (settled && most > 0 && hp >= 0 && hp < impactHp[i] && impactHp[i] <= most)
                strongest = std::max(strongest, float(impactHp[i] - hp) / float(most));
            impactHp[i] = hp;
        }
        if (!ff8)
        {
            const bool cinema = ffuFF7Cinematic();
            if (cinema && !impactCinema && settled) strongest = std::max(strongest, HARD_HIT);   // (a summon's or a Limit's start)
            impactCinema = cinema;
        }
        if (strongest < HIT)
            return;
        const bool hard = strongest >= HARD_HIT;
        const float size = hard ? HARD_SHAKE : SHAKE;
        if (shakeAge() < shakeFor && size <= shakeSize)
            return;   // (a shake as big or bigger running)
        shakeFrom = GetTickCount64();
        shakeFor = hard ? HARD_SHAKE_TIME : SHAKE_TIME;
        shakeSize = size;
    }
}

// The battle picture's shake now, for FF7's and FF8's passes: x, y its offset (across the picture), z how much it is
// magnified (1: none, so its edges stay in); fading out, two unrelated wobbles (as FF9's: BattleImpact.cs)
void ffuLookShake(float shake[4])
{
    const float age = shakeAge();
    shake[0] = shake[1] = shake[3] = 0.0f;
    shake[2] = 1.0f;
    if (!(age < shakeFor) || !ffuLookImpact())
        return;
    const float fade = 1.0f - age / shakeFor, size = shakeSize * fade * fade;
    shake[0] = size * sinf(age * 37.0f);   // (about 6 and 5 Hz: a jolt, not a buzz)
    shake[1] = size * cosf(age * 29.0f) * 1.3f;
    shake[2] = 1.0f + 2.6f * shakeSize;
}

bool ffuLookShaking()
{
    return ffuLookImpact() && shakeAge() < shakeFor;
}

// Tests: FFU_LOOK_DEBUG=<n> paints FF8's pass's own view (1 what each pixel is taken for, 2 its shadow), 0 none
float ffuLookDebug()
{
    static const float view = [] { const char *value = getenv("FFU_LOOK_DEBUG"); return value ? float(atoi(value)) : 0.0f; }();
    return view;
}

// The sliders: 0 the main light, 1 the light from all around, 2 contact, 3 the glow, 4 the haze, 5 the spells' light.
float ffuLookLevel(int which)
{
    if (which == 5) return spellLevel.load();
    if (which == 6) return cinematicLevel.load();
    return which >= 0 && which < 5 ? levels[which].load() : 1.0f;
}

// The touchpad's zoom in battles (Richard 2026-10-22: "make it so the touch pad on the controller can still zoom in and
// out"): how far a finger moved up the pad since the last frame, in pad heights x 0.5 (+ in), as the hub's own ground
// and flight zoom read it (airship_flight.gd _touch_swipe); a new touch starts where the finger lands. Through our pad
// DLL's ffu_pad_touch (mods/ffu-pad, xinput9_1_0.dll in FF7's and FF8's folders); 0 outside battles, with the look's
// camera off, or without the DLL. The pad is read once a frame (ffuLookFrame) and FF7's battle camera takes that frame's
// swipe when it runs (ffuTouchSwipe): a finger moving while an action camera plays is never added all at once after it.
bool ffuLookCamera();
static float pendingSwipe = 0.0f;

static void touchFrame()
{
    typedef DWORD (WINAPI *touch_fn)(DWORD *down, float *y, DWORD *number);
    static touch_fn touch = nullptr;
    static bool looked = false;
    static DWORD lastNumber = 0;
    static float lastY = 0.0f;
    static bool lastDown = false;
    if (!looked)
    {
        looked = true;
        HMODULE pad = GetModuleHandleA("xinput9_1_0.dll");
        if (!pad) pad = LoadLibraryA("xinput9_1_0.dll");
        touch = pad ? (touch_fn)(void *)GetProcAddress(pad, "ffu_pad_touch") : nullptr;
        ffnx_info("FFU look: the touchpad %s\n", touch ? "zooms the battle camera" : "can't zoom (no ffu_pad_touch in xinput9_1_0.dll)");
    }
    DWORD down = 0, number = 0;
    float y = 0.0f;
    if (!touch || touch(&down, &y, &number) != 0) down = 0;
    const bool same = down && lastDown && number == lastNumber;
    const float swipe = same ? (y - lastY) * 0.5f : 0.0f;
    lastDown = down != 0;
    lastNumber = number;
    lastY = y;
    pendingSwipe = getmode_cached()->driver_mode == MODE_BATTLE && ffuLookCamera() ? swipe : 0.0f;
}

float ffuTouchSwipe()
{
    const float swipe = pendingSwipe;
    pendingSwipe = 0.0f;
    return swipe;
}

bool ffuLookAtmosphere()
{
    int flags = frameFlags.load();
    return flags < 0 || (flags & LOOK_ATMOSPHERE);
}

// Tests: FFU_LOOK_SPLIT=1 shows each part of the look (FF8's lighting and haze, FF7's haze and bars, the glow, FXAA) on
// the picture's right half only, the game's own on the left: one frame, before and after (docs/battle-look.md).
float ffuLookSplit()
{
    static const float split = [] { const char *value = getenv("FFU_LOOK_SPLIT"); return value && !strcmp(value, "1") ? 0.5f : 0.0f; }();
    return split;
}

bool ffuLookCinema()
{
    int flags = frameFlags.load();
    return flags < 0 || (flags & LOOK_CINEMA);
}

bool ffuLookCamera()
{
    int flags = frameFlags.load();
    return flags < 0 || (flags & LOOK_CAMERA);
}

bool ffuLookLighting()
{
    int flags = frameFlags.load();
    return flags < 0 || (flags & LOOK_LIGHTING);
}

void ffuSurveyFrame();    // ffu_survey.cpp
void ffuFF8LookFrame();   // ffu_ff8look.cpp
void ffuFF7LookFrame();   // ffu_ff7look.cpp
void ffuFF7LookFrameEnd();

namespace
{
    // (the main light 3.8 times the ambient: Richard 2026-10-22, "I can't see any other changes", at 1.7 times; at 2.6 a
    // pair of the same idle frame still differed by 2.6 of 255, the sides away from the light about as FFNx lit them)
    constexpr float METALLIC = 0.0f, SPECULAR = 0.1f, ROUGHNESS = 0.85f, LIGHT = 2.5f, AMBIENT = 0.65f;
    std::atomic<bool> shown{true};   // the game's window is shown (a hidden battle, a mixed party's guest, gets no look)

    bool lookWanted()
    {
        int flags = frameFlags.load();
        return getmode_cached()->driver_mode == MODE_BATTLE && (flags < 0 || (flags & LOOK_LIGHTING)) && shown.load();
    }

    float putLight = LIGHT, putAmbient = AMBIENT;   // what the look last put (the sliders move it)

    bool lookInPlace()
    {
        return lighting.getMetallic() == METALLIC && lighting.getSpecular() == SPECULAR && lighting.getRoughness() == ROUGHNESS
            && lighting.getLightIntensity() == putLight && lighting.getAmbientIntensity() == putAmbient;
    }

    void put(float metallic, float specular, float roughness, float light, float ambient)
    {
        lighting.setMetallic(metallic);
        lighting.setSpecular(specular);
        lighting.setRoughness(roughness);
        lighting.setLightIntensity(light);
        lighting.setAmbientIntensity(ambient);
    }

    // FFNx's own values for this battle, taken after its lighting read its config for it (put back if the look is
    // switched off mid-battle: a reload would make its shadow map again, about a second's stall: Rules 59, 89)
    float battleMetallic, battleSpecular, battleRoughness, battleLight, battleAmbient;
}

// The game's window is shown (all the battle look's parts: ffu_glow.cpp, ffu_ff8look.cpp)
bool ffuLookShown()
{
    return shown.load();
}

// FFU_LOOK_FRAMES=1 (dev): each second of a battle's first 20 (then every 5 s), its frames' times at the flip, and the
// GPU's time for the last frame (bgfx's stats): where a battle's start lags (Richard 2026-10-22, "FF8s battles are very
// laggy when they start").
static void logBattleFrames()
{
    static int on = -1;
    if (on < 0)
    {
        const char *value = getenv("FFU_LOOK_FRAMES");
        on = value && value[0] == '1';
    }
    if (!on) return;
    static LARGE_INTEGER frequency, last, battleFrom, secondFrom;
    static int frames = 0, slow = 0;
    static double worst = 0.0, total = 0.0, gpuWorst = 0.0, gpuTotal = 0.0;
    LARGE_INTEGER now;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    const bool battle = getmode_cached()->driver_mode == MODE_BATTLE;
    auto ms = [&](LARGE_INTEGER a, LARGE_INTEGER b) { return double(b.QuadPart - a.QuadPart) * 1000.0 / double(frequency.QuadPart); };
    if (!battle)
    {
        battleFrom.QuadPart = 0;
        last = now;
        return;
    }
    if (!battleFrom.QuadPart)
    {
        battleFrom = secondFrom = now;
        frames = slow = 0;
        worst = total = gpuWorst = gpuTotal = 0.0;
        ffnx_info("FFU frames: a battle begins\n");
    }
    else
    {
        static bool wasVisible = false;   // (where the window shows, against the frames' times)
        const bool visible = gameHwnd && IsWindowVisible(gameHwnd) && !IsIconic(gameHwnd);
        if (visible != wasVisible)
            ffnx_info("FFU frames: the window %s %.2f s into the battle\n", visible ? "shown" : "hidden", ms(battleFrom, now) / 1000.0);
        wasVisible = visible;
        const double frame = ms(last, now);
        const bgfx::Stats *stats = bgfx::getStats();
        const double gpu = stats && stats->gpuTimerFreq > 0 ? double(stats->gpuTimeEnd - stats->gpuTimeBegin) * 1000.0 / double(stats->gpuTimerFreq) : -1.0;
        frames++;
        total += frame;
        worst = std::max(worst, frame);
        if (frame > 40.0) slow++;
        gpuTotal += gpu;
        gpuWorst = std::max(gpuWorst, gpu);
        const double since = ms(battleFrom, now) / 1000.0;
        if (ms(secondFrom, now) >= (since < 20.0 ? 1000.0 : 5000.0))
        {
            ffnx_info("FFU frames: %.0f s into the battle: %d frames, %.1f ms each, worst %.1f, %d over 40 ms; GPU %.1f ms each, worst %.1f\n",
                      since, frames, total / frames, worst, slow, gpuTotal / frames, gpuWorst);
            secondFrom = now;
            frames = slow = 0;
            worst = total = gpuWorst = gpuTotal = 0.0;
        }
    }
    last = now;
}

// At the flip, before FFNx's lighting draws (common_flip).
void ffuLookFrame()
{
    logBattleFrames();
    if (!ff8) touchFrame();   // (FF7's battle camera's touchpad zoom; FF8's is the bridge's own: camera_ff8.c)
    frameFlags.store(requestedFlags.load());
    // (the first battle of this run warms the look up while it is still hidden, behind the hub's swirl: its targets are
    // made and its passes' pipelines built before the window shows, where FF7 stalled 370 ms; once, and for at most 300
    // hidden frames, as a guest's hidden battle in a mixed party never shows)
    const bool visible = gameHwnd && IsWindowVisible(gameHwnd) && !IsIconic(gameHwnd);
    static bool warmed = false;
    static int warmFrames = 0;
    const bool battleNow = getmode_cached()->driver_mode == MODE_BATTLE;
    const bool warming = !warmed && battleNow && !visible && warmFrames < 300;
    if (warming && warmFrames++ == 0)
        ffnx_info("FFU look: warming up behind the cut (its targets and passes made before the battle shows)\n");
    if ((battleNow && visible) || warmFrames >= 300)
        warmed = true;
    shown.store(visible || warming);
    impactFrame();
    ffuSurveyFrame();
    ffuFF8LookFrame();
    ffuFF7LookFrame();
    if (ff8)
        return;
    bool battle = getmode_cached()->driver_mode == MODE_BATTLE;
    if (lightingAtStart < 0 && battle)   // (at the first battle: FFNx turns its lighting off while its start logo shows)
        lightingAtStart = enable_lighting ? 1 : 0;
    if (lightingAtStart != 1)
        return;
    bool want = lookWanted();
    static bool wasBattle = false;
    const bool entering = battle && !wasBattle;
    wasBattle = battle;
    if (entering)
    {
        // FFNx's values before the battle's own (put back as the battle ends, if the look took over: FFNx reads its
        // config again only at a new field, not on going back to the same one), taken as the battle begins, before
        // FFNx's lighting reads the battle's config
        savedMetallic = lighting.getMetallic();
        savedSpecular = lighting.getSpecular();
        savedRoughness = lighting.getRoughness();
        savedLight = lighting.getLightIntensity();
        savedAmbient = lighting.getAmbientIntensity();
    }
    if (!want && tuned)
    {
        if (battle)
            put(battleMetallic, battleSpecular, battleRoughness, battleLight, battleAmbient);   // switched off mid-battle
        else
            put(savedMetallic, savedSpecular, savedRoughness, savedLight, savedAmbient);
        tuned = false;
        ffnx_info("ffu-look: FF7's battle lighting back to FFNx's own config\n");
    }
}

// After the frame's deferred draws (common_flip; with FFNx's lighting on, all of FF7's battle draws are deferred and
// drawn there).
void ffuLookAfterDeferred()
{
    ffuFF7LookFrameEnd();
}

// Just after FFNx's lighting drew (common_flip): at a new battle FFNx has just read its config for it, so the look's
// values go in now, before the next frame draws (only the battle's first frame, its deferred draws drawn inside
// FFNx's lighting, keeps FFNx's own).
void ffuLookAfterLighting()
{
    if (ff8 || lightingAtStart != 1 || !lookWanted())
        return;
    if (!tuned || !lookInPlace())
    {
        battleMetallic = lighting.getMetallic();
        battleSpecular = lighting.getSpecular();
        battleRoughness = lighting.getRoughness();
        battleLight = lighting.getLightIntensity();
        battleAmbient = lighting.getAmbientIntensity();
    }
    putLight = LIGHT * levels[0].load();
    putAmbient = AMBIENT * levels[1].load();
    put(METALLIC, SPECULAR, ROUGHNESS, putLight, putAmbient);
    if (!tuned)
        ffnx_info("ffu-look: FF7's battle lighting in the shared look (matte, main light 3.8x the ambient)\n");
    tuned = true;
}
