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

// Final Fantasy Universe: a survey of FF8's battle draws (docs/battle-look.md "FF8 lighting"), for a screen-space
// lighting pass over FF8's own depth before its first flat 2D draw. Off unless FFU_FF8_SURVEY=1. Every draw FFNx makes
// in an FF8 battle frame is counted: 3D (screen-space vertices with depth: some rhw other than 1) or flat (every rhw 1:
// HUD, menus, flat sprites) or untransformed, in the order they come, with their blend modes, depth tests and writes and
// the 3D draws' depth range; one line every 120 frames (FFNx.log, "FFU survey").

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
        int firstFlat = -1, lastDepth = -1, depthAfterFlat = 0;
        uint32_t depthBlend[8] = {}, flatBlend[8] = {};
        uint32_t depthTested = 0, depthWritten = 0, flatTested = 0, flatWritten = 0;
        float zMin = 1e9f, zMax = -1e9f, rhwMin = 1e9f, rhwMax = -1e9f;
    } frame;
    uint32_t frames = 0;
}

void ffuSurveyDraw(uint32_t vertextype, const struct nvertex *vertices, uint32_t vertexcount, uint32_t blendMode,
                   uint32_t depthTest, uint32_t depthMask)
{
    if (surveyOn < 0)
    {
        const char *value = getenv("FFU_FF8_SURVEY");
        surveyOn = value && !strcmp(value, "1") ? 1 : 0;
    }
    if (!surveyOn || !ff8 || getmode_cached()->driver_mode != MODE_BATTLE)
        return;
    const int index = (int)frame.draws++;
    const uint32_t blend = blendMode < 8 ? blendMode : 7;
    if (vertextype != TLVERTEX)
    {
        frame.worldDraws++;
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
    if (++frames % 120 == 0)
        ffnx_info("FFU survey: %u draws: %u with depth (blends 0-4: %u %u %u %u %u; tested %u, written %u; z %.5f..%.5f, rhw %.6f..%.6f), "
                  "%u flat (blends %u %u %u %u %u; tested %u, written %u), %u untransformed; first flat #%d, last with depth #%d, "
                  "%d with depth after the first flat\n",
                  frame.draws, frame.depthDraws, frame.depthBlend[0], frame.depthBlend[1], frame.depthBlend[2], frame.depthBlend[3],
                  frame.depthBlend[4], frame.depthTested, frame.depthWritten, frame.zMin, frame.zMax, frame.rhwMin, frame.rhwMax,
                  frame.flatDraws, frame.flatBlend[0], frame.flatBlend[1], frame.flatBlend[2], frame.flatBlend[3], frame.flatBlend[4],
                  frame.flatTested, frame.flatWritten, frame.worldDraws, frame.firstFlat, frame.lastDepth, frame.depthAfterFlat);
    frame = Frame();
}
