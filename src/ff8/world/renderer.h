/****************************************************************************/
//    Copyright (C) 2026 Final Fantasy Universe contributors                //
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
#include <vector>

#include "../../external_mesh.h"

// FF8's whole world map drawn as a real 3D mesh, the way FF7's world is drawn from Cosmos Gaia's
// external mesh. FF8 projects its world in software (an emulated PS1 GTE) and only ever hands FFNx
// screen-space vertices for ~11 x 11 blocks; with `ff8_world_external_mesh` the fork switches FF8's
// own terrain triangles off and draws every block of the world from <external_mesh_path>/ff8world/*.gltf
// with FF8's exact camera, lens and depth, so FF8's own models (Squall, vehicles, Garden) and effects
// still sort against it. Design and the maths: docs/ff8-3d-world-renderer.md (Final Fantasy Universe).
namespace ffu::ff8world
{
    // Exported by the Final Fantasy Universe proxy dinput.dll as `ffu_ff8_globe` (globe_ff8.c). The
    // first 20 floats are exactly the shader's ff8Globe[5] layout for the globe mode.
    struct GlobeState
    {
        uint32_t magic;          // 'FFG1'
        uint32_t frame;
        float anchorX, anchorZ, cameraY, altitude;
        float scale, radius, mode, reserved;
        float east[4], up[4], north[4];
        volatile int32_t meshActive;   // written by us: the DLL then skips its own far-side fallback
    };

    struct CameraSnapshot
    {
        int16_t m[3][3] = {};   // 0x2040140: GTE rotation, rows, 4096 = 1.0
        float H = 1024.0f;      // GTE H (0x1CA92E4), includes the DLL's widened lens
        float ofx = 160.0f, ofy = 112.0f;
        int32_t c[3] = {};      // 0x2045C98: -camera relative to the player's block
        int32_t refBlockX = 0, refBlockZ = 0;
        int32_t curveStart = 0x1460;
        GlobeState globe = {};
        bool hasGlobe = false;
        bool valid = false;
    };

    struct MeshPart
    {
        uint32_t firstShape = 0, shapeCount = 0;
        int storyBit = -1, storyState = 0;   // -1: always drawn
        bool hexField = false;
    };

    struct ShapeOffset { uint32_t vertex, index; };

    class Renderer
    {
    public:
        void init();
        void load();
        void onExit();
        void snapshot();
        void draw();
        bool replacesTerrain() const { return loaded && !mesh.shapes.empty(); }

    private:
        void buildProjection(struct matrix& projection) const;
        void fillGlobeUniform(float out[20], bool polarCap) const;
        void buildPolarCaps();
        void drawShapes(bool translucent);
        bool partVisible(const MeshPart& part) const;
        void animateTextures();

        ExternalMesh mesh;
        ExternalMesh caps;
        std::vector<MeshPart> parts;
        std::vector<ShapeOffset> shapeOffsets;
        CameraSnapshot cam;
        GlobeState* dllGlobe = nullptr;
        bool loaded = false;
        bool loadAttempted = false;
    };

    extern Renderer renderer;
}
