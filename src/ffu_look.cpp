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
    constexpr int LOOK_LIGHTING = 1, LOOK_GLOW = 2, LOOK_SHARP = 4;

    std::atomic<int> requestedFlags{-1};   // -1: never told (the look's default: everything on)
    bool tuned = false;                     // the look's values are in FFNx's lighting now
    int lightingAtStart = -1;
    // FFNx's own values when the look took over (put back as the battle ends: FFNx reads its config again only at a new
    // field, not at the same one, and a reload would make its shadow map again: Sonnet review, 2026-10-22)
    float savedMetallic, savedSpecular, savedRoughness, savedLight, savedAmbient;
}

// flags: bit 0 the battle lighting, bit 1 the glow, bit 2 the anti-aliasing (both ffu_glow.cpp).
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

void ffuSurveyFrame();   // ffu_survey.cpp

void ffuLookFrame()
{
    ffuSurveyFrame();
    if (ff8)
        return;
    bool battle = getmode_cached()->driver_mode == MODE_BATTLE;
    if (lightingAtStart < 0 && battle)   // (at the first battle: FFNx turns its lighting off while its start logo shows)
        lightingAtStart = enable_lighting ? 1 : 0;
    if (lightingAtStart != 1)
        return;
    int flags = requestedFlags.load();
    bool want = battle && (flags < 0 || (flags & LOOK_LIGHTING));
    if (want)
    {
        if (!tuned)
        {
            savedMetallic = lighting.getMetallic();
            savedSpecular = lighting.getSpecular();
            savedRoughness = lighting.getRoughness();
            savedLight = lighting.getLightIntensity();
            savedAmbient = lighting.getAmbientIntensity();
        }
        // Every frame: FFNx reads its config again at each new battle (Lighting::draw), after this.
        lighting.setMetallic(0.0f);
        lighting.setSpecular(0.1f);
        lighting.setRoughness(0.85f);
        lighting.setLightIntensity(1.85f);
        lighting.setAmbientIntensity(1.09f);
        if (!tuned)
            ffnx_info("ffu-look: FF7's battle lighting in the shared look (matte, main light 1.7x the ambient)\n");
        tuned = true;
    }
    else if (tuned)
    {
        if (battle)
            lighting.reload();   // switched off mid-battle: FFNx's installed config for this battle, once
        else
        {
            lighting.setMetallic(savedMetallic);
            lighting.setSpecular(savedSpecular);
            lighting.setRoughness(savedRoughness);
            lighting.setLightIntensity(savedLight);
            lighting.setAmbientIntensity(savedAmbient);
        }
        tuned = false;
        ffnx_info("ffu-look: FF7's battle lighting back to FFNx's own config\n");
    }
}
