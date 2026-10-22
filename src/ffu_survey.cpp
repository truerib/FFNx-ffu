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

// Final Fantasy Universe: a survey of FF8's and FF7's battle draws (docs/battle-look.md "FF8 lighting", "FF7 haze"), for
// a screen-space pass over the game's own depth before its first flat 2D draw. Off unless FFU_FF8_SURVEY=1 (FF8) or
// FFU_LOOK_SURVEY=1 (either). Every draw FFNx makes in a battle frame is counted: screen-space with depth (some rhw
// other than 1), flat (every rhw 1: HUD, menus, flat sprites) or untransformed (FF7's 3D), in the order they come, with
// their blend modes, depth tests and writes and the screen-space 3D draws' depth range; one line every 120 frames
// (FFNx.log, "FFU survey").

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "common.h"
#include "gl.h"
#include "globals.h"
#include "log.h"

namespace
{
    int surveyOn = -1;
    struct Frame
    {
        uint32_t draws = 0, depthDraws = 0, flatDraws = 0, worldDraws = 0;
        int firstFlat = -1, lastDepth = -1, depthAfterFlat = 0, lastWorld = -1, worldAfterFlat = 0;
        uint32_t worldTested = 0, worldWritten = 0;
        uint32_t depthBlend[8] = {}, flatBlend[8] = {};
        uint32_t depthTested = 0, depthWritten = 0, flatTested = 0, flatWritten = 0;
        float zMin = 1e9f, zMax = -1e9f, rhwMin = 1e9f, rhwMax = -1e9f;
        // the flat draws in order (their blend modes), and the adding ones' depth and place on the screen (FF8's spells
        // reach FFNx flat, as its HUD: what tells them apart, for the glow's mask and the spells' light)
        char flatOrder[40] = {};
        int flatOrderLength = 0;
        float addZMin = 1e9f, addZMax = -1e9f, addYMin = 1e9f, addYMax = -1e9f;
        int addFirst = -1;   // (the first adding flat draw's place among the flat ones)
    } frame;
    uint32_t lastSpellLine = 0;
    uint32_t frames = 0;
}

void ffuSurveyDraw(uint32_t vertextype, const struct nvertex *vertices, uint32_t vertexcount, uint32_t blendMode,
                   uint32_t depthTest, uint32_t depthMask)
{
    if (surveyOn < 0)
    {
        const char *value = getenv("FFU_FF8_SURVEY"), *either = getenv("FFU_LOOK_SURVEY");
        surveyOn = (either && !strcmp(either, "1")) ? 1 : (value && !strcmp(value, "1") && ff8) ? 1 : 0;
    }
    if (!surveyOn || getmode_cached()->driver_mode != MODE_BATTLE)
        return;
    const int index = (int)frame.draws++;
    const uint32_t blend = blendMode < 8 ? blendMode : 7;
    if (vertextype != TLVERTEX)
    {
        frame.worldDraws++;
        frame.lastWorld = index;
        if (frame.firstFlat >= 0) frame.worldAfterFlat++;
        if (depthTest) frame.worldTested++;
        if (depthMask) frame.worldWritten++;
        return;
    }
    bool flat = true;
    for (uint32_t i = 0; i < vertexcount; i++)
        if (vertices[i].color.w != 1.0f)   // (rhw)
        {
            flat = false;
            break;
        }
    if (flat)
    {
        if (frame.flatOrderLength < 39) frame.flatOrder[frame.flatOrderLength++] = char('0' + blend);
        if (blend == 1 || blend == 3)
        {
            if (frame.addFirst < 0) frame.addFirst = (int)frame.flatDraws;
            for (uint32_t i = 0; i < vertexcount; i++)
            {
                frame.addZMin = std::min(frame.addZMin, vertices[i]._.z);
                frame.addZMax = std::max(frame.addZMax, vertices[i]._.z);
                frame.addYMin = std::min(frame.addYMin, vertices[i]._.y);
                frame.addYMax = std::max(frame.addYMax, vertices[i]._.y);
            }
        }
        frame.flatDraws++;
        frame.flatBlend[blend]++;
        if (frame.firstFlat < 0) frame.firstFlat = index;
        if (depthTest) frame.flatTested++;
        if (depthMask) frame.flatWritten++;
        return;
    }
    frame.depthDraws++;
    frame.depthBlend[blend]++;
    frame.lastDepth = index;
    if (frame.firstFlat >= 0) frame.depthAfterFlat++;
    if (depthTest) frame.depthTested++;
    if (depthMask) frame.depthWritten++;
    for (uint32_t i = 0; i < vertexcount; i++)
    {
        frame.zMin = std::min(frame.zMin, vertices[i]._.z);
        frame.zMax = std::max(frame.zMax, vertices[i]._.z);
        frame.rhwMin = std::min(frame.rhwMin, vertices[i].color.w);
        frame.rhwMax = std::max(frame.rhwMax, vertices[i].color.w);
    }
}

// Once a frame (ffuLookFrame, at the flip): the line every 120 FF8 battle frames, then the counts start again.
void ffuSurveyFrame()
{
    if (surveyOn != 1 || frame.draws == 0)
        return;
    // (a frame with more adding flat draws than the HUD's one panel: a spell, at most one line every 15 frames)
    if (frame.flatBlend[1] + frame.flatBlend[3] >= 2 && frames - lastSpellLine >= 15)
    {
        lastSpellLine = frames;
        ffnx_info("FFU survey, adding flat draws: %u (blends 1 %u, 3 %u), first at flat #%d of %u; their z %.5f..%.5f, y %.0f..%.0f; "
                  "flat order %s\n", frame.flatBlend[1] + frame.flatBlend[3], frame.flatBlend[1], frame.flatBlend[3], frame.addFirst,
                  frame.flatDraws, frame.addZMin, frame.addZMax, frame.addYMin, frame.addYMax, frame.flatOrder);
    }
    if (++frames % 120 == 0)
        ffnx_info("FFU survey: %u draws: %u with depth (blends 0-4: %u %u %u %u %u; tested %u, written %u; z %.5f..%.5f, rhw %.6f..%.6f), "
                  "%u flat (blends %u %u %u %u %u; tested %u, written %u), %u untransformed (tested %u, written %u; last #%d, %d after the first flat); "
                  "first flat #%d, last with depth #%d, %d with depth after the first flat\n",
                  frame.draws, frame.depthDraws, frame.depthBlend[0], frame.depthBlend[1], frame.depthBlend[2], frame.depthBlend[3],
                  frame.depthBlend[4], frame.depthTested, frame.depthWritten, frame.zMin, frame.zMax, frame.rhwMin, frame.rhwMax,
                  frame.flatDraws, frame.flatBlend[0], frame.flatBlend[1], frame.flatBlend[2], frame.flatBlend[3], frame.flatBlend[4],
                  frame.flatTested, frame.flatWritten, frame.worldDraws, frame.worldTested, frame.worldWritten, frame.lastWorld, frame.worldAfterFlat,
                  frame.firstFlat, frame.lastDepth, frame.depthAfterFlat);
    frame = Frame();
}
