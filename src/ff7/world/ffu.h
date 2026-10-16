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
        int32_t hideModel;         // version 2: FF7 world model id not drawn (the airship a guest replaces), -1 none
        // version 3: the bridge flies the camera (docs/space.md "Chase camera"): while cameraOverride is set,
        // FFNx's world camera takes it exactly, with no easing and with roll: cameraBasis holds the rows of the
        // camera's rotation in FFNx's world (right, screen down, forward: the rows FFNx's own pitch/yaw
        // quaternion gives), cameraZoom its distance back from the view centre (which the bridge sets), and
        // cameraPitch / cameraYaw the nearest FFNx angles, which the game's own state follows.
        int32_t cameraOverride;
        float cameraPitch, cameraYaw, cameraZoom;
        float cameraBasis[9];
        // version 4: a visitor walks FF7's world as someone else (docs/space.md "Who you are in another world"):
        // hidePlayerModel = 1 hides the on-foot player model (a stand-in or the live layer shows the visitor);
        // unitsPerR = FF7 units per planet radius the bridge places the flight camera with this frame (the live
        // layer's depth scale, src/ffu_layer.cpp), 0 = FF7_GLOBE_RADIUS.
        int32_t hidePlayerModel;
        float unitsPerR;
        // version 5: publishLayer = 1 while FF7 follows a flight of its own airship in another world (hidden): only then
        // is the live layer read back (src/ffu_layer.cpp); flying in FF7 itself there is no one to send it to.
        int32_t publishLayer;
    };

    // Exported by the bridge as `ffu_ff7_airships[AIRSHIP_SLOTS]`: airships FFNx draws from their real models
    // (mesh/airships/<id>.gltf, native axes), guests from the other games (docs/space.md "The airship's state").
    // model: row r: p[r] = model[4r..4r+2] . v + model[4r+3], from model space to FFNx's view space
    // (AIRSHIP_WITH_CAMERA: the pose rides with this frame's camera exactly) or to FF7's world space
    // (AIRSHIP_ON_PLANET).
    constexpr int AIRSHIP_SLOTS = 2;
    enum AirshipMode : int32_t
    {
        AIRSHIP_HIDDEN = 0,
        AIRSHIP_WITH_CAMERA = 1,   // flies with the camera: not bent onto the globe; the space lens applies
        AIRSHIP_ON_PLANET = 2,     // stands on FF7's planet: bent onto the globe like FF7's own models
    };
    struct Airship
    {
        char id[24];
        AirshipMode mode;
        float model[12];
    };
    // Exported by the bridge as `ffu_ff7_people[PEOPLE_SLOTS]` (bridge version 4): characters drawn the same way
    // from mesh/airships/<id>.gltf: a visitor's labelled stand-in, and Cloud left standing as an NPC.
    constexpr int PEOPLE_SLOTS = 2;

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
    const Control *cameraOverride();   // the bridge's camera, or nullptr when FFNx's own camera runs
    // The shaders' FF7 globe state for this draw: [0] = (on the world map, cloud drift in radians,
    // 1 while an airship that flies with the camera is drawn, 0).
    void globeUniform(float out[8]);
    // Replaces the game's world_draw_all_3d_model_74C6B0 call in the overworld draw: hides the model the
    // bridge asks to hide, then draws the airships after FF7's own models.
    void drawAllModels(int param);
}
