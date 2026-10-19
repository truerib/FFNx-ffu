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
#include <map>
#include <memory>
#include <string>
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
        float scale, radius, mode, publishLayer;   // publishLayer: 1 while FF8 follows a flight of its own airship elsewhere
        float east[4], up[4], north[4];
        float space;                   // 0 in the atmosphere .. 1 in space (the sky is black)
        volatile int32_t meshActive;   // written by us: the DLL then skips its own far-side fallback
        // Live characters (docs/crossover-design.md "Characters, live"):
        float characterUnits;          // FF8 units per character height: a visitor's (slot visible = 3) or Squall's own
                                       //   while his clip is drawn for another world's camera (the layer's units)
        volatile float lens[4];        // written by us: FF8's lens as it draws its own picture, in that page's terms
                                       //   (u = fu X/Z + cu, v = fv Y/Z + cv; u right, v up, ±1 top to bottom)
        volatile float layerLens[4];   // written by us: the live layer's lens (square pixels)
        volatile float aspect;         // written by us: the picture's width / height
    };

    // Exported by the proxy dinput.dll as `ffu_ff8_airships[FF8_AIRSHIP_SLOTS]`: airships the fork draws
    // from their real models (mesh/airships/<id>.gltf): FF8's own Ragnarok in space, and guests from the
    // other games. `model` maps a model-space point v to the camera-relative frame (FF8 units, camera at
    // the origin, +y down): p = model[0..2] . v + model[3], row by row (3 rows of 4).
    constexpr int FF8_AIRSHIP_SLOTS = 4;
    struct AirshipPose
    {
        char id[24];             // file stem under mesh/airships, e.g. "ff8_ragnarok"
        int32_t visible;
        float model[12];
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
        AirshipPose airships[FF8_AIRSHIP_SLOTS] = {};
        bool hasGlobe = false;
        bool valid = false;
    };

    struct MeshPart
    {
        uint32_t firstShape = 0, shapeCount = 0;
        int storyBit = -1, storyState = 0;   // -1: always drawn
        bool hexField = false;
    };

    struct ShapeOffset { uint32_t vertex, index, vertexCount, indexCount; };

    class Renderer
    {
    public:
        void init();
        void load();
        void onExit();
        void snapshot();
        void draw();
        void afterDrawOT();
        void drawTranslucentTerrain();
        void drawAtmosphere();
        bool replacesTerrain() const { return loaded && !mesh.shapes.empty(); }

    private:
        void buildProjection(struct matrix& projection, bool squarePixels = false) const;
        void lensTerms(bool squarePixels, float out[4], float &aspect) const;   // buildProjection's lens, in the protocol's terms
        enum class Part { World, PolarCap, OtherPlanet };
        void fillGlobeUniform(float out[36], Part part, const float* planetOffset = nullptr) const;
        void buildPolarCaps();
        void loadOtherPlanets();
        void buildUnitSphere();
        void drawOtherPlanetGlows();
        void loadClouds();
        void drawCloudShell();
        float cloudDrift() const;
        float cloudFade() const;
        void drawOtherPlanets();
        void drawAirships();
        void drawAirship(const AirshipPose& pose, ExternalMesh* model);
        static void drawLayerShips();          // the live layer's replay (src/ffu_layer.cpp)
        std::vector<AirshipPose> layerPoses;   // FF8's own airships drawn this frame, for that replay
        ExternalMesh* airshipMesh(const char* id);
        void drawShapes(bool translucent);
        bool partVisible(const MeshPart& part) const;
        bool shapeVisible(const Shape& shape) const;
        void animateTextures();

        ExternalMesh mesh;
        ExternalMesh caps;
        ExternalMesh sphere;
        bgfx::TextureHandle ownClouds = BGFX_INVALID_HANDLE;    // unit sphere with equirectangular UVs, for the other planets
        struct OtherPlanet { const char* id; float offset[3]; bgfx::TextureHandle texture = BGFX_INVALID_HANDLE; bgfx::TextureHandle clouds = BGFX_INVALID_HANDLE; };
        std::vector<OtherPlanet> otherPlanets;
        std::vector<MeshPart> parts;
        std::vector<ShapeOffset> shapeOffsets;
        CameraSnapshot cam;
        GlobeState* dllGlobe = nullptr;
        HMODULE dllModule = nullptr;
        AirshipPose* dllAirships = nullptr;
        bool loaded = false;
        bool loadAttempted = false;
    };

    extern Renderer renderer;
}
