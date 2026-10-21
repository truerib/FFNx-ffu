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
    constexpr int LOOK_LIGHTING = 1, LOOK_GLOW = 2;

    std::atomic<int> requestedFlags{-1};   // -1: never told (the look's default: everything on)
    bool tuned = false;                     // the look's values are in FFNx's lighting now
    int lightingAtStart = -1;
}

// flags: bit 0 the battle lighting, bit 1 the glow (ffu_glow.cpp).
extern "C" __declspec(dllexport) void ffu_look(int flags)
{
    requestedFlags.store(flags);
}

bool ffuLookGlow()
{
    int flags = requestedFlags.load();
    return flags < 0 || (flags & LOOK_GLOW);
}

void ffuLookFrame()
{
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
        lighting.reload();   // FFNx's installed config, once
        tuned = false;
        ffnx_info("ffu-look: FF7's battle lighting back to FFNx's own config\n");
    }
}
