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
// (metallic 0, low specular, rough), the main light 1.7 times the ambient, at the overall brightness FFNx's installed
// config gives (its light 4.0 at metallic 0.5 and ambient 1.0: a surface half turned to the light keeps its colour).
// Off: FFNx's installed config again (lighting/config.toml, its per-battle groups too). Nothing changes outside
// battles, and nothing at all when FFNx's lighting wasn't on at start (it is set up only then).

#include <atomic>

#include "cfg.h"
#include "common.h"
#include "globals.h"
#include "lighting.h"
#include "log.h"

namespace
{
    constexpr int LOOK_LIGHTING = 1, LOOK_GLOW = 2, LOOK_SHARP = 4, LOOK_CAMERA = 8;

    std::atomic<int> requestedFlags{-1};   // -1: never told (the look's default: everything on)
    bool tuned = false;                     // the look's values are in FFNx's lighting now
    int lightingAtStart = -1;
    // FFNx's own values when the look took over (put back as the battle ends: FFNx reads its config again only at a new
    // field, not at the same one, and a reload would make its shadow map again: Sonnet review, 2026-10-22)
    float savedMetallic, savedSpecular, savedRoughness, savedLight, savedAmbient;
}

// flags: bit 0 the battle lighting (FF8's: ffu_ff8look.cpp), bit 1 the glow, bit 2 the anti-aliasing (both ffu_glow.cpp),
// bit 3 the stick camera (FF7's held across action cameras: ff7/battle/camera.cpp; FF8's is the bridge's own).
extern "C" __declspec(dllexport) void ffu_look(int flags)
{
    requestedFlags.store(flags);
}

bool ffuLookGlow()
{
    int flags = requestedFlags.load();
    return flags < 0 || (flags & LOOK_GLOW);
}

bool ffuLookSharp()
{
    int flags = requestedFlags.load();
    return flags < 0 || (flags & LOOK_SHARP);
}

bool ffuLookCamera()
{
    int flags = requestedFlags.load();
    return flags < 0 || (flags & LOOK_CAMERA);
}

bool ffuLookLighting()
{
    int flags = requestedFlags.load();
    return flags < 0 || (flags & LOOK_LIGHTING);
}

void ffuSurveyFrame();    // ffu_survey.cpp
void ffuFF8LookFrame();   // ffu_ff8look.cpp

namespace
{
    constexpr float METALLIC = 0.0f, SPECULAR = 0.1f, ROUGHNESS = 0.85f, LIGHT = 1.85f, AMBIENT = 1.09f;
    std::atomic<bool> shown{true};   // the game's window is shown (a hidden battle, a mixed party's guest, gets no look)

    bool lookWanted()
    {
        int flags = requestedFlags.load();
        return getmode_cached()->driver_mode == MODE_BATTLE && (flags < 0 || (flags & LOOK_LIGHTING)) && shown.load();
    }

    bool lookInPlace()
    {
        return lighting.getMetallic() == METALLIC && lighting.getSpecular() == SPECULAR && lighting.getRoughness() == ROUGHNESS
            && lighting.getLightIntensity() == LIGHT && lighting.getAmbientIntensity() == AMBIENT;
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

// At the flip, before FFNx's lighting draws (common_flip).
void ffuLookFrame()
{
    shown.store(gameHwnd && IsWindowVisible(gameHwnd) && !IsIconic(gameHwnd));
    ffuSurveyFrame();
    ffuFF8LookFrame();
    if (ff8)
        return;
    bool battle = getmode_cached()->driver_mode == MODE_BATTLE;
    if (lightingAtStart < 0 && battle)   // (at the first battle: FFNx turns its lighting off while its start logo shows)
        lightingAtStart = enable_lighting ? 1 : 0;
    if (lightingAtStart != 1)
        return;
    bool want = lookWanted();
    if (want && !tuned)
    {
        // FFNx's values before the battle's own (put back as the battle ends: FFNx reads its config again only at a new
        // field, not on going back to the same one)
        savedMetallic = lighting.getMetallic();
        savedSpecular = lighting.getSpecular();
        savedRoughness = lighting.getRoughness();
        savedLight = lighting.getLightIntensity();
        savedAmbient = lighting.getAmbientIntensity();
    }
    else if (!want && tuned)
    {
        if (battle)
            put(battleMetallic, battleSpecular, battleRoughness, battleLight, battleAmbient);   // switched off mid-battle
        else
            put(savedMetallic, savedSpecular, savedRoughness, savedLight, savedAmbient);
        tuned = false;
        ffnx_info("ffu-look: FF7's battle lighting back to FFNx's own config\n");
    }
}

// Just after FFNx's lighting drew (common_flip): at a new battle FFNx has just read its config for it, so the look's
// values go in now, before the next frame draws with FFNx's own (no frame of FFNx's own look at a battle's start).
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
    put(METALLIC, SPECULAR, ROUGHNESS, LIGHT, AMBIENT);
    if (!tuned)
        ffnx_info("ffu-look: FF7's battle lighting in the shared look (matte, main light 1.7x the ambient)\n");
    tuned = true;
}
