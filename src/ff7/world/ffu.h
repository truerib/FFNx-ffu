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

#pragma once

#include <cstdint>

// Final Fantasy Universe: FF7's planet (docs/globe.md in the Final Fantasy Universe project).
// The world-map globe itself is in the shaders (misc/FFNx.ffu7.sh), which the renderer swaps in for
// FFNx.* / FFNx.lighting.* on FF7's world map (Renderer::programForDraw). This side gives them their
// state (the ff7Globe uniform) and exchanges a few values with the project's FF7 bridge (its proxy
// dinput8.dll): with no bridge, FFNx's own camera limits stay as they are.
namespace ffu::ff7
{
    // Exported by the bridge (dinput8.dll) as `ffu_ff7_control`: limits it lifts for the climb into space.
    struct Control
    {
        uint32_t magic;            // 'F7CT'
        uint32_t version;          // 1
        float maxZoom;             // the world camera's zoom cap (FFNx: 35000)
        float pitchMin, pitchMax;  // the world camera's pitch clamp (FFNx: -175, -95)
        int32_t highwindCeiling;   // the Highwind climbs while lower (FFNx: 32767 - 2000 - 50 x multiplier)
    };

    // Exported by FFNx as `ffu_ff7_ffnx`: what the bridge reads to know FF7's camera exactly.
    struct Ffnx
    {
        uint32_t magic;            // 'F7FX'
        uint32_t version;          // 1
        void *camera;              // ff7::world::WorldCamera: target, localTarget, offset (vec3 each), zoom
        const float *viewMatrix, *invViewMatrix, *d3dProjection, *d3dViewport;   // bx layout, 16 floats
    };

    bool onWorldMap();             // FF7 (not FF8) is on its world map
    const Control *control();      // the bridge's values, or nullptr when it isn't there
    float maxZoom(float fallback);
    float pitchMin(float fallback);
    float pitchMax(float fallback);
    int highwindCeiling(int fallback);
    // The shaders' FF7 globe state for this draw: [0] = (on the world map, cloud drift in radians, 0, 0).
    void globeUniform(float out[8]);
}
