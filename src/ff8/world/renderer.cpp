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

#include "renderer.h"
#include "../../ffu_airships.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../../cfg.h"
#include "../../globals.h"
#include "../../log.h"
#include "../../patch.h"
#include "../../renderer.h"
#include "../../ff8.h"
#include "../../gl.h"

// Addresses are FF8 1.2 US (FF8.exe as run by FF8 Remastered through FFNx); the hooks are only
// installed for that build, and every call site is checked before it is replaced.
namespace ffu::ff8world
{
    Renderer renderer;

    namespace
    {
        constexpr uint32_t CAMERA_MATRIX   = 0x2040140;   // PS1 MATRIX
        constexpr uint32_t GTE_H           = 0x1CA92E4;
        constexpr uint32_t GTE_OFX         = 0x1CA92DE;   // integer half of the 16.16 OFX
        constexpr uint32_t GTE_OFY         = 0x1CA92E2;
        constexpr uint32_t CAMERA_REL      = 0x2045C98;   // int x, y, z
        constexpr uint32_t PLAYER_X        = 0x203EE80;
        constexpr uint32_t PLAYER_Z        = 0x203EE84;
        constexpr uint32_t CURVE_START     = 0xC75CFC;
        constexpr uint32_t SSIGPU_SCALE_X  = 0xB7CC14;    // vw / draw area width / 8, set by PutDrawEnv
        constexpr uint32_t SSIGPU_SCALE_Y  = 0xB7CC18;
        constexpr uint32_t FF8_ZONE_BACK   = 0x204DA18;   // u8 R, G, B: the zone's back colour this frame (GTE BK >> 4)
        constexpr uint32_t FF8_ZONE_MATRIX = 0x204DA20;   // s16[3][3]: its colour matrix (GTE LCM), rows R, G, B
        constexpr uint32_t SSIGPU_OFFSET_X = 0x1CA89E0;
        constexpr uint32_t SSIGPU_OFFSET_Y = 0x1CA89E4;
        constexpr uint32_t FRAME_DRAWN     = 0xB7CC24;    // 0 when FF8 skips drawing this frame
        constexpr uint32_t GLOBE_MAGIC     = 0x31474646;  // 'FFG1'
        constexpr uint32_t WORLD_STORY_FLAGS    = 0x2036BB6;   // u16: which story variant of each changed segment
        constexpr uint32_t WORLD_HEXFIELD_STAGE = 0x2036BD8;

        constexpr float MAP_WIDTH = 262144.0f;
        constexpr float MAP_HEIGHT = 196608.0f;
        constexpr float PLANET_RADIUS = 41721.5f;

        typedef void (*emit_fn)(uint8_t *block, int ot, int out);
        typedef void (*plain_fn)();
        typedef void (*draw_ot_fn)(int *ot);
        typedef void (*exit_fn)(int);
        typedef void (*link_fn)(uint32_t *entry, void *prim, int z0, int z1, int z2, int z3);

        uint32_t orig_terrain, orig_terrain_nofog, orig_draw_ot, orig_draw_ot_paused, orig_enter, orig_exit;
        uint32_t orig_emit[12];
        uint32_t orig_sky_link[4];

        // A `call rel32` at `site` whose target is `expected` (0 = any): replaced by `hook`, previous target returned.
        uint32_t hook_call(uint32_t site, uint32_t expected, void *hook)
        {
            const uint8_t *at = (const uint8_t *)site;
            if (at[0] != 0xE8)
            {
                ffnx_error("FF8 world mesh: no call at 0x%X, hook skipped\n", site);
                return 0;
            }
            const uint32_t target = site + 5 + *(const int32_t *)(at + 1);
            if (expected && target != expected)
            {
                ffnx_error("FF8 world mesh: call at 0x%X goes to 0x%X, not 0x%X, hook skipped\n", site, target, expected);
                return 0;
            }
            // replace_call_function returns an index for undoing the patch, not the old target.
            replace_call_function(site, hook);
            return target;
        }

        void hook_terrain()       { renderer.snapshot(); ((plain_fn)orig_terrain)(); }
        void hook_terrain_nofog() { renderer.snapshot(); ((plain_fn)orig_terrain_nofog)(); }
        void hook_draw_ot(int *ot)         { renderer.draw(); ((draw_ot_fn)orig_draw_ot)(ot); renderer.afterDrawOT(); }
        void hook_draw_ot_paused(int *ot)  { renderer.draw(); ((draw_ot_fn)orig_draw_ot_paused)(ot); renderer.afterDrawOT(); }
        void hook_enter()          { ((plain_fn)orig_enter)(); renderer.load(); }
        void hook_exit(int a1)     { renderer.onExit(); ((exit_fn)orig_exit)(a1); }

        // FF8's own terrain triangles (and their distance haze) are switched off while the mesh draws the
        // world. The rest of the terrain function still runs: it also keeps collision, entity block lists
        // and the Ragnarok's ceiling up to date.
        template<int I> void hook_emit(uint8_t *block, int ot, int out)
        {
            if (renderer.replacesTerrain()) return;
            ((emit_fn)orig_emit[I])(block, ot, out);
        }

        // The sky, its stars and clouds sit in FF8's last depth slot with depth 0xFFF (z = 0.0639), which
        // would cover any terrain farther than view z 0x7FF8 now that the world goes past it: their depth
        // goes to 0xFFFF (z = 0.99998); the slot (draw order) is unchanged.
        template<int I> void hook_sky_link(uint32_t *entry, void *prim, int z0, int z1, int z2, int z3)
        {
            auto skyDepth = [](int z) { return z == 0xFFF && renderer.replacesTerrain() ? 0xFFFF : z; };
            ((link_fn)orig_sky_link[I])(entry, prim, skyDepth(z0), skyDepth(z1), skyDepth(z2), skyDepth(z3));
        }

        struct EmitSite { uint32_t site, target; };
        const EmitSite emit_sites[12] = {
            { 0x53BE0D, 0 }, { 0x53C4BD, 0 },               // block_1 (already wrapped by FFNx's UV fix)
            { 0x53BE22, 0 }, { 0x53C4D2, 0 },               // block_2 (idem)
            { 0x53BE46, 0x5511F0 }, { 0x53C500, 0x5511F0 }, // big blocks
            { 0x53C9C4, 0x553E20 }, { 0x53D06B, 0x553E20 }, // fog-off copies
            { 0x53C9D9, 0x5541F0 }, { 0x53D080, 0x5541F0 },
            { 0x53C9FD, 0x5511F0 }, { 0x53D0AE, 0x5511F0 },
        };
        const uint32_t sky_link_sites[4] = { 0x554FF1, 0x55522D, 0x55537E, 0x555447 };

        template<int... I> void hook_emitters(std::integer_sequence<int, I...>)
        {
            ((orig_emit[I] = hook_call(emit_sites[I].site, emit_sites[I].target, (void *)hook_emit<I>)), ...);
        }
        template<int... I> void hook_sky_links(std::integer_sequence<int, I...>)
        {
            ((orig_sky_link[I] = hook_call(sky_link_sites[I], 0x45C860, (void *)hook_sky_link<I>)), ...);
        }

        int floor_mod(int value, int by) { int m = value % by; return m < 0 ? m + by : m; }
    }

    void Renderer::init()
    {
        if (!FF8_US_VERSION)
        {
            ffnx_info("FF8 world mesh: only FF8 1.2 US addresses are known; disabled\n");
            return;
        }

        orig_terrain        = hook_call(0x53FBE4, 0x53B9E0, (void *)hook_terrain);
        orig_terrain_nofog  = hook_call(0x53FBEB, 0x53C5A0, (void *)hook_terrain_nofog);
        orig_draw_ot        = hook_call(0x55935B, 0x45D600, (void *)hook_draw_ot);
        orig_draw_ot_paused = hook_call(0x53F0AF, 0x45D600, (void *)hook_draw_ot_paused);
        hook_emitters(std::make_integer_sequence<int, 12>{});
        hook_sky_links(std::make_integer_sequence<int, 4>{});

        orig_enter = *(uint32_t *)(ff8_externals.main_loop + 0x2C0);
        orig_exit  = *(uint32_t *)(ff8_externals.main_loop + 0x2C8);
        patch_code_dword(ff8_externals.main_loop + 0x2C0, (DWORD)hook_enter);
        patch_code_dword(ff8_externals.main_loop + 0x2C8, (DWORD)hook_exit);

        ffnx_info("FF8 world mesh: hooks installed (terrain 0x%X, draw 0x%X, enter 0x%X, exit 0x%X)\n",
                  orig_terrain, orig_draw_ot, orig_enter, orig_exit);
    }

    void Renderer::load()
    {
        if (loadAttempted) return;   // kept resident: the world module exits and re-enters on every battle
        loadAttempted = true;

        const std::filesystem::path folder = std::filesystem::path(basedir) / external_mesh_path / "ff8world";
        std::error_code error;
        std::vector<std::filesystem::path> files;
        for (const auto &entry : std::filesystem::directory_iterator(folder, error))
            if (entry.path().extension() == ".gltf") files.push_back(entry.path());
        if (error || files.empty())
        {
            ffnx_error("FF8 world mesh: no .gltf files in %s; FF8 keeps drawing its own terrain\n", folder.string().c_str());
            return;
        }
        std::sort(files.begin(), files.end());

        // Base-colour DDS files live in <mod_path>/world/ (ExternalMesh's first lookup); tex_path is its
        // fallback. Forward slashes: the loader splits paths on '/' to find each <model>_config.toml.
        const std::string texPath = (std::filesystem::path(basedir) / mod_path / "world").generic_string() + "/";
        mesh.deferBufferUpdates = true;
        for (const auto &file : files)
        {
            std::string path = file.generic_string(), tex = texPath;
            const uint32_t before = mesh.shapes.size();
            if (!mesh.importExternalMeshGltfFile(path.data(), tex.data(), true))   // true: keep FF8's axes as written
            {
                ffnx_error("FF8 world mesh: could not load %s\n", path.c_str());
                continue;
            }
            // Story variants (tools/ff8world): ffu_ff8wm_story<bit>_<state>[_hexfield].gltf
            MeshPart part = { before, uint32_t(mesh.shapes.size()) - before };
            const std::string stem = file.stem().string();
            int bit = -1, state = 0;
            if (sscanf(stem.c_str(), "ffu_ff8wm_story%d_%d", &bit, &state) == 2)
            {
                part.storyBit = bit;
                part.storyState = state;
                part.hexField = stem.find("_hexfield") != std::string::npos;
            }
            parts.push_back(part);
        }

        if (mesh.shapes.empty())
        {
            ffnx_error("FF8 world mesh: no shapes loaded; FF8 keeps drawing its own terrain\n");
            return;
        }
        // One upload of everything, then the CPU copies go (FF8 is a 32-bit process); each shape keeps its
        // place and size in the shared buffers, and its bounds for culling.
        mesh.updateExternalMeshBuffers();
        mesh.clearExternalMesh3dBuffers();
        uint32_t vertexOffset = 0, indexOffset = 0;
        for (auto &shape : mesh.shapes)
        {
            const uint32_t vertices = shape.vertices.size(), indices = shape.indices.size();
            shapeOffsets.push_back({ vertexOffset, indexOffset, vertices, indices });
            vertexOffset += vertices;
            indexOffset += indices;
            std::vector<nvertex>().swap(shape.vertices);
            std::vector<vector3<float>>().swap(shape.normals);
            std::vector<vector4<float>>().swap(shape.joints);
            std::vector<vector4<float>>().swap(shape.weights);
            std::vector<uint32_t>().swap(shape.indices);
        }

        buildPolarCaps();
        buildUnitSphere();
        loadOtherPlanets();
        loadClouds();
        loaded = !mesh.shapes.empty();
        ffnx_info("FF8 world mesh: %u shapes, %u vertices from %u files\n", (uint32_t)mesh.shapes.size(), vertexOffset, (uint32_t)files.size());
    }

    void Renderer::onExit()
    {
        cam.valid = false;
    }

    // Two ocean caps beyond the map's north and south edges (true Mercator stops at +-79.2 degrees).
    // Vertices are unit vectors on the planet (y = north); the shader's cap mode places them.
    void Renderer::buildPolarCaps()
    {
        constexpr int RINGS = 12, SECTORS = 192;
        const float edge = 2.0f * std::atan(std::exp(0.5f * MAP_HEIGHT / PLANET_RADIUS)) - 1.5707963f;
        // FF8's open sea along the map's north and south edges (median of those rows of the real terrain,
        // tools/ff8world planet): RGB (33, 57, 74).
        const uint32_t ocean = 0xFF21394A;   // A R G B in the vertex layout's uint32

        for (int hemisphere = 0; hemisphere < 2; ++hemisphere)
        {
            const float sign = hemisphere == 0 ? 1.0f : -1.0f;
            Shape shape;
            for (int ring = 0; ring <= RINGS; ++ring)
            {
                const float lat = sign * (edge + (1.5707963f - edge) * float(ring) / RINGS);
                for (int sector = 0; sector <= SECTORS; ++sector)
                {
                    const float lon = 6.2831853f * float(sector) / SECTORS;
                    struct nvertex vertex = {};
                    vertex._.x = std::cos(lat) * std::sin(lon);
                    vertex._.y = std::sin(lat);
                    vertex._.z = std::cos(lat) * std::cos(lon);
                    vertex.color.color = ocean;
                    vertex.color.w = 1.0f;
                    shape.vertices.push_back(vertex);
                    shape.normals.push_back({ vertex._.x, vertex._.y, vertex._.z });
                    shape.joints.push_back({});
                    shape.weights.push_back({});
                }
            }
            for (int ring = 0; ring < RINGS; ++ring)
                for (int sector = 0; sector < SECTORS; ++sector)
                {
                    const uint32_t a = ring * (SECTORS + 1) + sector, b = a + SECTORS + 1;
                    for (uint32_t index : { a, b, a + 1, a + 1, b, b + 1 }) shape.indices.push_back(index);
                }
            shape.isDoubleSided = true;
            caps.fillExternalMeshVertexBuffer(shape.vertices.data(), shape.normals.data(), shape.joints.data(), shape.weights.data(), shape.vertices.size());
            caps.fillExternalMeshIndexBuffer(shape.indices.data(), shape.indices.size());
            caps.shapes.push_back(std::move(shape));
        }
        caps.updateExternalMeshBuffers();
    }

    void Renderer::snapshot()
    {
        ::memcpy(cam.m, (const void *)CAMERA_MATRIX, sizeof(cam.m));
        cam.H = float(*(const uint32_t *)GTE_H & 0xFFFF);
        cam.ofx = float(*(const int16_t *)GTE_OFX);
        cam.ofy = float(*(const int16_t *)GTE_OFY);
        ::memcpy(cam.c, (const void *)CAMERA_REL, sizeof(cam.c));
        cam.refBlockX = floor_mod(*(const int32_t *)PLAYER_X + 0x60000, 0x40000) >> 11;
        cam.refBlockZ = floor_mod(*(const int32_t *)PLAYER_Z + 0x48000, 0x30000) >> 11;
        cam.curveStart = *(const int32_t *)CURVE_START;

        if (!dllGlobe)
        {
            // The proxy in the game folder, not the system dinput.dll it loads itself.
            char path[MAX_PATH] = {};
            GetModuleFileNameA(NULL, path, sizeof(path));
            char *slash = strrchr(path, '\\');
            if (slash) strcpy(slash + 1, "dinput.dll");
            HMODULE proxy = GetModuleHandleA(path);
            auto state = proxy ? (GlobeState *)GetProcAddress(proxy, "ffu_ff8_globe") : nullptr;
            if (state && state->magic == GLOBE_MAGIC) { dllGlobe = state; dllModule = proxy; }
        }
        cam.hasGlobe = dllGlobe != nullptr && dllGlobe->mode > 0.5f && dllGlobe->mode < 1.5f;
        if (!dllAirships && dllGlobe)
            dllAirships = (AirshipPose *)GetProcAddress(dllModule, "ffu_ff8_airships");
        if (dllAirships) ::memcpy(cam.airships, dllAirships, sizeof(cam.airships));
        if (dllGlobe)
        {
            cam.globe = *dllGlobe;
            dllGlobe->meshActive = replacesTerrain() ? 1 : 0;
            // For the DLL's live characters: FF8's lens as it draws its picture and as its live layer does.
            float lens[4], layerLens[4], aspect = 1.6f;
            lensTerms(false, lens, aspect);
            lensTerms(true, layerLens, aspect);
            for (int i = 0; i < 4; ++i) { dllGlobe->lens[i] = lens[i]; dllGlobe->layerLens[i] = layerLens[i]; }
            dllGlobe->aspect = aspect;
        }
        cam.valid = true;
    }

    // FF8's projection with its rotation folded in (docs/ff8-3d-world-renderer.md 1.7/1.8): FF8's own
    // 1/8-pixel PS1 screen, mapped by the live ssigpu scale/offset into FFNx's TL space (wide viewport
    // included), then FFNx's orthographic matrix. Depth is replaced in the shader (FF8Depth).
    void Renderer::buildProjection(struct matrix &projection, bool squarePixels) const
    {
        const float scaleX = *(const float *)SSIGPU_SCALE_X * 8.0f, scaleY = *(const float *)SSIGPU_SCALE_Y * 8.0f;
        const float offsetX = *(const float *)SSIGPU_OFFSET_X, offsetY = *(const float *)SSIGPU_OFFSET_Y;
        // FFNx's TL orthographic matrix spans its wide viewport in widescreen, which FFNx also writes
        // into FF8's ssigpu viewport (ff8_engine_set_wide_viewport).
        const float left = widescreen_enabled ? float(*ff8_externals.ssigpu_viewport_x_dword_1CA89D8) : 0.0f;
        const float width = widescreen_enabled ? float(*ff8_externals.ssigpu_viewport_width_dword_B7CBF8) : float(game_width);
        const float height = float(game_height);

        struct matrix rotation = {}, lens = {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                rotation.m[i][j] = float(cam.m[j][i]) / 4096.0f;   // row vectors: v = p * R^T
        rotation._44 = 1.0f;

        lens._11 = 2.0f * scaleX * cam.H / width;
        // FF8's 320x224 picture is stretched to the 16:10 frame (scaleX / scaleY ~ 1.08 wider). In space every
        // game must show the same picture (docs/space.md "Lens"), so the fork's own drawing (world, planets,
        // glow, airships) gets square pixels there, blended in with FF8's space share; on the ground FF8 keeps
        // its own look (its models are drawn by FF8 with the stretch).
        // The live layer is always square: it is laid into another game's picture (src/ffu_layer.cpp).
        const float squareShare = squarePixels ? 1.0f : cam.globe.space;
        if (cam.hasGlobe && scaleX > 0.0f) lens._11 *= 1.0f + squareShare * (scaleY / scaleX - 1.0f);
        lens._31 = 2.0f * (offsetX + scaleX * cam.ofx - left) / width - 1.0f;
        lens._22 = -2.0f * scaleY * cam.H / height;
        lens._32 = 1.0f - 2.0f * (offsetY + scaleY * cam.ofy) / height;
        lens._33 = 1.0f;
        lens._34 = 1.0f;   // w = view z
        multiply_matrix(&rotation, &lens, &projection);
    }

    // buildProjection's lens for a point (X right, Y up, Z ahead) of the camera's frame, in the terms of
    // docs/crossover-design.md "Characters, live": u = fu X/Z + cu, v = fv Y/Z + cv, v = ±1 at the picture's top and bottom
    // and u in the same units (± the aspect at its sides). FF8's own picture is stretched sideways near the ground (fu > fv).
    void Renderer::lensTerms(bool squarePixels, float out[4], float &aspect) const
    {
        const float scaleX = *(const float *)SSIGPU_SCALE_X * 8.0f, scaleY = *(const float *)SSIGPU_SCALE_Y * 8.0f;
        const float offsetX = *(const float *)SSIGPU_OFFSET_X, offsetY = *(const float *)SSIGPU_OFFSET_Y;
        const float left = widescreen_enabled ? float(*ff8_externals.ssigpu_viewport_x_dword_1CA89D8) : 0.0f;
        const float width = widescreen_enabled ? float(*ff8_externals.ssigpu_viewport_width_dword_B7CBF8) : float(game_width);
        const float height = float(game_height);
        aspect = height > 0.0f ? width / height : 1.6f;
        float x = 2.0f * scaleX * cam.H / width;
        const float squareShare = squarePixels ? 1.0f : cam.globe.space;
        if (cam.hasGlobe && scaleX > 0.0f) x *= 1.0f + squareShare * (scaleY / scaleX - 1.0f);
        // ndc x = x X/Z + cx; ndc y = -(-2 scaleY H / height) Y/Z + cy, the view's y being down (buildProjection).
        out[0] = x * aspect;
        out[1] = 2.0f * scaleY * cam.H / height;
        out[2] = (2.0f * (offsetX + scaleX * cam.ofx - left) / width - 1.0f) * aspect;
        out[3] = 1.0f - 2.0f * (offsetY + scaleY * cam.ofy) / height;
    }

    void Renderer::fillGlobeUniform(float out[36], Part part, const float* planetOffset) const
    {
        ::memset(out, 0, sizeof(float) * 36);
        // [6..8] FF8's zone light this world frame (0x555480 blends it in: docs/research/ff8-world-lighting-2026-10-19.md):
        // the colour matrix's rows R, G, B (s16, 4096 = 1.0, 0x204DA20) and the back colour (u8 R, G, B, 0x204DA18).
        // The shader lights the terrain with them as FF8's NCCT does (FF8ZoneLight); zero until FF8 has set them.
        if (part == Part::World)
        {
            const int16_t *matrix = (const int16_t *)FF8_ZONE_MATRIX;
            const uint8_t *back = (const uint8_t *)FF8_ZONE_BACK;
            for (int c = 0; c < 3; ++c)
            {
                for (int j = 0; j < 3; ++j) out[24 + 4 * c + j] = float(matrix[3 * c + j]) / 4096.0f;
                out[24 + 4 * c + 3] = float(back[c]);
            }
        }
        // Camera position in the mesh's space (FF8's G space), for the flat and FF8-bend modes.
        const float cameraX = float(cam.refBlockX) * 2048.0f - float(cam.c[0]);
        const float cameraY = -float(cam.c[1]);
        const float cameraZ = -float(cam.refBlockZ) * 2048.0f - float(cam.c[2]);
        if (cam.hasGlobe)
        {
            ::memcpy(out, &cam.globe.anchorX, sizeof(float) * 20);
            out[6] = 1.0f;
        }
        else
        {
            // No globe from the proxy: FF8's own horizon bend, exactly as FF8 draws its terrain.
            out[2] = float(cam.c[1]);
            out[4] = 1.0f;
            out[5] = PLANET_RADIUS;
            out[6] = 2.0f;
            for (int j = 0; j < 3; ++j) out[8 + j] = float(cam.m[2][j]) / 4096.0f;
            out[11] = float(cam.curveStart);
            out[12] = cameraX; out[13] = cameraY; out[14] = cameraZ;
        }
        out[7] = part == Part::PolarCap ? 1.0f : 0.0f;
        if (part == Part::OtherPlanet && planetOffset)
        {
            out[6] = 4.0f;
            out[20] = planetOffset[0]; out[21] = planetOffset[1]; out[22] = planetOffset[2];
            out[23] = cam.globe.radius;   // FF8 units per planet radius, the same as FF8's own globe this frame
        }
    }

    // The other planets of docs/space.md, as seen from FF8's: their real worlds (tools/ff8world planet,
    // tools/ff7planet, FF9's) on unit spheres at their solar centres, relative to FF8's centre.
    void Renderer::loadOtherPlanets()
    {
        struct Known { const char* id; float x, y, z; };
        const Known known[] = { { "ff7", 0.0f, 0.0f, 20.0f }, { "ff9", -17.3205f, 0.0f, -10.0f } };
        const float self[3] = { 17.3205f, 0.0f, -10.0f };
        for (const auto &planet : known)
        {
            char path[MAX_PATH];
            _snprintf(path, sizeof(path), "%s/%s/planets/%s_planet.dds", basedir, external_mesh_path.c_str(), planet.id);
            uint32_t width = 0, height = 0, mips = 0;
            auto texture = newRenderer.createTextureHandle(path, &width, &height, &mips);
            if (!texture.idx) { ffnx_info("FF8 world mesh: no %s, that planet is not drawn\n", path); continue; }
            otherPlanets.push_back({ planet.id, { planet.x - self[0], planet.y - self[1], planet.z - self[2] }, texture });
        }
        buildUnitSphere();
        ffnx_info("FF8 world mesh: %u other planets loaded\n", (uint32_t)otherPlanets.size());
    }

    // docs/space.md "Clouds": the map turns east one turn per 6 hours of real time, the same in every game.
    float Renderer::cloudDrift() const
    {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        const uint64_t ticks = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;   // 100 ns since 1601
        const uint64_t unix_seconds = ticks / 10000000ull - 11644473600ull;
        return float(unix_seconds % 21600ull) / 21600.0f;
    }

    // Faded in with the camera's height above the ground: smoothstep(0.25, 0.6, h), h in planet radii.
    float Renderer::cloudFade() const
    {
        if (!cam.hasGlobe || cam.globe.radius <= 0.0f) return 0.0f;
        const float h = (float(cam.c[1]) + cam.globe.altitude) / cam.globe.radius;
        float t = (h - 0.25f) / (0.6f - 0.25f);
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        return t * t * (3.0f - 2.0f * t);
    }

    void Renderer::loadClouds()
    {
        auto load = [](const char *id) {
            char path[MAX_PATH];
            _snprintf(path, sizeof(path), "%s/%s/planets/%s_clouds.dds", basedir, external_mesh_path.c_str(), id);
            uint32_t width = 0, height = 0, mips = 0;
            auto texture = newRenderer.createTextureHandle(path, &width, &height, &mips);
            if (!texture.idx) { ffnx_info("FF8 world mesh: no %s, no clouds there\n", path); return bgfx::TextureHandle(BGFX_INVALID_HANDLE); }
            return texture;
        };
        ownClouds = load("ff8");
        for (auto &planet : otherPlanets) planet.clouds = load(planet.id);
    }

    // Our cloud shell, 0.02 R above sea level, alpha-blended over everything drawn so far.
    void Renderer::drawCloudShell()
    {
        const float fade = cloudFade();
        if (fade <= 0.0f || !bgfx::isValid(ownClouds) || sphere.shapes.empty()) return;
        const Shape &shape = sphere.shapes[0];
        struct matrix identity, projection;
        identity_matrix(&identity);
        buildProjection(projection);
        newRenderer.setInterpolationQualifier(SMOOTH);
        newRenderer.setPrimitiveType();
        newRenderer.isTLVertex(false);
        newRenderer.isTexture(true);
        newRenderer.isExternalTexture(true);
        newRenderer.doTextureFiltering(true);
        newRenderer.setBlendMode(RendererBlendMode::BLEND_AVG);
        newRenderer.doDepthTest(true);
        newRenderer.doDepthWrite(false);
        newRenderer.setD3DViweport(&identity);
        newRenderer.setD3DProjectionRaw(&projection);
        newRenderer.setWorldViewMatrix(&identity, false);
        float globe[36];
        fillGlobeUniform(globe, Part::World);
        globe[6] = 7.0f;
        globe[20] = fade;
        globe[23] = 0.02f * cam.globe.radius;
        newRenderer.setFF8WorldMesh(true, globe, 3.0f, cloudDrift());
        newRenderer.setCommonUniforms();
        newRenderer.setCullMode(RendererCullMode::DISABLED);
        sphere.bindField3dVertexBuffer(0, shape.vertices.size());
        sphere.bindField3dIndexBuffer(0, shape.indices.size());
        newRenderer.useTexture(ownClouds.idx, RendererTextureSlot::TEX_Y);
        newRenderer.bindTextures();
        newRenderer.draw(true, true, true);
    }

    // A unit sphere with equirectangular UVs: the other planets and the atmosphere shell.
    void Renderer::buildUnitSphere()
    {
        if (!sphere.shapes.empty()) return;
        constexpr int RINGS = 64, SECTORS = 128;
        Shape shape;
        for (int ring = 0; ring <= RINGS; ++ring)
        {
            const float lat = 1.5707963f - 3.1415927f * float(ring) / RINGS;
            for (int sector = 0; sector <= SECTORS; ++sector)
            {
                const float lon = 6.2831853f * float(sector) / SECTORS;   // 0 at +Z, east towards +X
                struct nvertex vertex = {};
                vertex._.x = std::cos(lat) * std::sin(lon);
                vertex._.y = std::sin(lat);
                vertex._.z = std::cos(lat) * std::cos(lon);
                vertex.color.color = 0xFFFFFFFF;
                vertex.color.w = 1.0f;
                vertex.u = float(sector) / SECTORS;
                vertex.v = float(ring) / RINGS;
                shape.vertices.push_back(vertex);
                shape.normals.push_back({ vertex._.x, vertex._.y, vertex._.z });
                shape.joints.push_back({});
                shape.weights.push_back({});
            }
        }
        for (int ring = 0; ring < RINGS; ++ring)
            for (int sector = 0; sector < SECTORS; ++sector)
            {
                const uint32_t a = ring * (SECTORS + 1) + sector, b = a + SECTORS + 1;
                for (uint32_t index : { a, b, a + 1, a + 1, b, b + 1 }) shape.indices.push_back(index);
            }
        sphere.fillExternalMeshVertexBuffer(shape.vertices.data(), shape.normals.data(), shape.joints.data(), shape.weights.data(), shape.vertices.size());
        sphere.fillExternalMeshIndexBuffer(shape.indices.data(), shape.indices.size());
        sphere.shapes.push_back(std::move(shape));
        sphere.updateExternalMeshBuffers();
    }

    // docs/space.md "## Atmosphere": an additive shell 0.6 R above the ground, drawn after FF8's own
    // primitives at just-in-front-of-the-sky depth, so it only shows where the planet doesn't. Its colour
    // is computed per pixel in FFNx.frag (FF8Atmosphere) and faded in as FF8's own 2D sky fades out.
    void Renderer::afterDrawOT()
    {
        if (!replacesTerrain() || !cam.valid) return;
        if (*(const uint32_t *)FRAME_DRAWN == 0) return;
        struct driver_state saved;
        gl_save_state(&saved);
        newRenderer.doAlphaTest(false);
        newRenderer.isFBTexture(false);
        newRenderer.doModulateAlpha(true);
        drawTranslucentTerrain();
        if (cam.hasGlobe) drawCloudShell();
        if (cam.hasGlobe && !sphere.shapes.empty() && cam.globe.space > 0.0f) drawAtmosphere();
        drawOtherPlanetGlows();
        newRenderer.discardAllBindings();
        newRenderer.setFF8WorldMesh(false);
        gl_load_state(&saved);
    }

    // Semi-transparent terrain after FF8's opaque models, so models behind water or ground haze still show.
    void Renderer::drawTranslucentTerrain()
    {
        struct matrix identity, projection;
        identity_matrix(&identity);
        buildProjection(projection);
        newRenderer.setInterpolationQualifier(SMOOTH);
        newRenderer.setPrimitiveType();
        newRenderer.isTLVertex(false);
        newRenderer.isExternalTexture(true);
        newRenderer.isTexture(true);
        newRenderer.doTextureFiltering(true);
        newRenderer.setBlendMode(RendererBlendMode::BLEND_AVG);
        newRenderer.doDepthTest(true);
        newRenderer.doDepthWrite(false);
        newRenderer.setD3DViweport(&identity);
        newRenderer.setD3DProjectionRaw(&projection);
        newRenderer.setWorldViewMatrix(&identity, false);
        float globe[36];
        fillGlobeUniform(globe, Part::World);
        const bool shadows = bgfx::isValid(ownClouds) && cloudFade() > 0.0f;   // as on the opaque terrain
        globe[20] = shadows ? cloudFade() : 0.0f;
        newRenderer.setFF8WorldMesh(true, globe, 1.0f, cloudDrift());
        newRenderer.setCommonUniforms();
        if (shadows) newRenderer.useTexture(ownClouds.idx, RendererTextureSlot::TEX_U);
        drawShapes(true);
        if (shadows) newRenderer.useTexture(0, RendererTextureSlot::TEX_U);
        newRenderer.isExternalTexture(false);
    }

    void Renderer::drawAtmosphere()
    {
        const Shape &shape = sphere.shapes[0];
        struct matrix identity, projection;
        identity_matrix(&identity);
        buildProjection(projection);
        newRenderer.setInterpolationQualifier(SMOOTH);
        newRenderer.setPrimitiveType();
        newRenderer.isTLVertex(false);
        newRenderer.isTexture(false);
        newRenderer.isExternalTexture(false);
        newRenderer.setBlendMode(RendererBlendMode::BLEND_ADD);
        newRenderer.doDepthTest(true);
        newRenderer.doDepthWrite(false);
        newRenderer.doScissorTest(false);
        newRenderer.setSphericalWorldRate(0.0f);
        newRenderer.setFogEnabled(false);
        newRenderer.setD3DViweport(&identity);
        newRenderer.setD3DProjectionRaw(&projection);
        newRenderer.setWorldViewMatrix(&identity, false);
        float globe[36];
        fillGlobeUniform(globe, Part::World);
        globe[6] = 6.0f;
        // The shell always encloses the camera, so every sky pixel is covered exactly once.
        globe[20] = cam.globe.space;                                                 // fade
        globe[23] = float(cam.c[1]) + cam.globe.altitude + 0.25f * cam.globe.radius;   // above the ground
        newRenderer.setFF8WorldMesh(true, globe, 2.0f);   // atmosphere
        newRenderer.setCommonUniforms();
        newRenderer.setCullMode(RendererCullMode::DISABLED);   // FFNx's FRONT mode is broken (missing break)
        sphere.bindField3dVertexBuffer(0, shape.vertices.size());
        sphere.bindField3dIndexBuffer(0, shape.indices.size());
        newRenderer.draw(true, true, true);
    }

    void Renderer::drawOtherPlanets()
    {
        // Only once the sky is black: the planets are lit worlds, and FF8's 2D sky can't show them through
        // its atmosphere colours.
        if (otherPlanets.empty() || sphere.shapes.empty() || !cam.hasGlobe || cam.globe.space < 0.98f) return;
        const Shape &shape = sphere.shapes[0];
        newRenderer.isTexture(true);
        newRenderer.doTextureFiltering(true);
        for (const auto &planet : otherPlanets)
        {
            float globe[36];
            fillGlobeUniform(globe, Part::OtherPlanet, planet.offset);
            newRenderer.setFF8WorldMesh(true, globe);
            newRenderer.setCommonUniforms();
            newRenderer.setCullMode(RendererCullMode::DISABLED);
            sphere.bindField3dVertexBuffer(0, shape.vertices.size());
            sphere.bindField3dIndexBuffer(0, shape.indices.size());
            newRenderer.useTexture(planet.texture.idx, RendererTextureSlot::TEX_Y);
            newRenderer.bindTextures();
            newRenderer.draw(true, true, true);
            if (bgfx::isValid(planet.clouds))
            {
                globe[6] = 8.0f;
                newRenderer.setFF8WorldMesh(true, globe, 3.0f, cloudDrift());
                newRenderer.setBlendMode(RendererBlendMode::BLEND_AVG);
                newRenderer.doDepthWrite(false);
                newRenderer.setCommonUniforms();
                sphere.bindField3dVertexBuffer(0, shape.vertices.size());
                sphere.bindField3dIndexBuffer(0, shape.indices.size());
                newRenderer.useTexture(planet.clouds.idx, RendererTextureSlot::TEX_Y);
                newRenderer.bindTextures();
                newRenderer.draw(true, true, true);
                newRenderer.setBlendMode(RendererBlendMode::BLEND_NONE);
                newRenderer.doDepthWrite(true);
            }
        }
    }

    // The other planets' atmosphere glow, as their own games draw it, so nothing appears at a hand-over. After
    // FF8's own (black) space sky, which would cover it otherwise: the far half of a 1.3 R shell, additive, depth
    // tested, so the planet in front hides it over the disc and only the limb glows.
    void Renderer::drawOtherPlanetGlows()
    {
        if (otherPlanets.empty() || sphere.shapes.empty() || !cam.hasGlobe || cam.globe.space < 0.98f) return;
        const Shape &shape = sphere.shapes[0];
        struct matrix identity, projection;
        identity_matrix(&identity);
        buildProjection(projection);
        newRenderer.setInterpolationQualifier(SMOOTH);
        newRenderer.setPrimitiveType();
        newRenderer.isTLVertex(false);
        newRenderer.isTexture(false);
        newRenderer.isExternalTexture(false);
        newRenderer.setBlendMode(RendererBlendMode::BLEND_ADD);
        newRenderer.doDepthTest(true);
        newRenderer.doDepthWrite(false);
        newRenderer.doScissorTest(false);
        newRenderer.setSphericalWorldRate(0.0f);
        newRenderer.setFogEnabled(false);
        newRenderer.setD3DViweport(&identity);
        newRenderer.setD3DProjectionRaw(&projection);
        newRenderer.setWorldViewMatrix(&identity, false);
        for (const auto &planet : otherPlanets)
        {
            float globe[36];
            fillGlobeUniform(globe, Part::OtherPlanet, planet.offset);
            globe[6] = 9.0f;
            newRenderer.setFF8WorldMesh(true, globe, 4.0f);
            newRenderer.setCommonUniforms();
            newRenderer.setCullMode(RendererCullMode::DISABLED);
            sphere.bindField3dVertexBuffer(0, shape.vertices.size());
            sphere.bindField3dIndexBuffer(0, shape.indices.size());
            newRenderer.draw(true, true, true);
        }
    }

    // mesh/airships/<id>.gltf, animated when it has frames (src/ffu_airships.cpp).
    ExternalMesh* Renderer::airshipMesh(const char* id)
    {
        return ffu::airshipModel(id, sizeof(AirshipPose::id), "FF8 world mesh");
    }

    // Airships drawn from their real models (AirshipPose). They climb with the camera, so the shader only
    // applies the virtual scale (the camera's climb leaves them where they are relative to it).
    void Renderer::drawAirship(const AirshipPose &pose, ExternalMesh *model)
    {
        struct matrix place = {};
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column) place.m[column][row] = pose.model[row * 4 + column];   // row vectors
            place.m[3][row] = pose.model[row * 4 + 3];
        }
        place._44 = 1.0f;
        float globe[36];
        fillGlobeUniform(globe, Part::World);
        globe[6] = 5.0f;
        newRenderer.setWorldViewMatrix(&place, false);
        newRenderer.setFF8WorldMesh(true, globe);
        newRenderer.isTexture(true);
        newRenderer.setCommonUniforms();
        uint32_t vertexOffset = 0, indexOffset = 0;
        for (const auto &shape : model->shapes)
        {
            if (shape.pMaterial && !shape.pMaterial->baseColorTexHandles.empty()
                && bgfx::isValid(shape.pMaterial->baseColorTexHandles[shape.pMaterial->texIndex]))
            {
                // No culling: the airship glTFs keep FF8's winding reversed, and FFNx's FRONT mode is broken
                // (a missing break sets both cull bits).
                newRenderer.setCullMode(RendererCullMode::DISABLED);
                model->bindField3dVertexBuffer(vertexOffset, shape.vertices.size());
                model->bindField3dIndexBuffer(indexOffset, shape.indices.size());
                newRenderer.useTexture(shape.pMaterial->baseColorTexHandles[shape.pMaterial->texIndex].idx, RendererTextureSlot::TEX_Y);
                newRenderer.bindTextures();
                newRenderer.draw(true, true, true);
            }
            vertexOffset += shape.vertices.size();
            indexOffset += shape.indices.size();
        }
    }

    // FF8's own airships again, alone, square, at the end of the frame (src/ffu_layer.cpp).
    void Renderer::drawLayerShips()
    {
        struct matrix projection;
        renderer.buildProjection(projection, true);
        newRenderer.setD3DProjectionRaw(&projection);
        for (const auto &pose : renderer.layerPoses)
            if (ExternalMesh *model = renderer.airshipMesh(pose.id)) renderer.drawAirship(pose, model);
        renderer.layerPoses.clear();
        struct matrix identity;
        identity_matrix(&identity);
        newRenderer.setWorldViewMatrix(&identity, false);
    }

    // Every pose the proxy filled: FF8's own airship, a guest, Squall as an NPC, a visitor's stand-in, or Squall's own clip
    // for another world's camera. A guest airship or visitor whose own game is sending its live layer isn't drawn here:
    // that layer is composited instead (src/ffu_layer.cpp, Rule 64). FF8's own airships, or Squall's clip, also go into
    // FF8's layer when FFU_FF8_LAYER=1 (docs/crossover-design.md "Characters, live").
    void Renderer::drawAirships()
    {
        if (!cam.hasGlobe) return;
        // FF8 units per planet radius as drawn: the globe's radius at the camera, times the climb's virtual scale.
        const float unitsPerR = cam.globe.radius * cam.globe.scale;
        std::vector<const char *> live, liveCharacters;
        layerPoses.clear();
        const AirshipPose *clip = nullptr;   // Squall's own clip, drawn for another world's camera
        for (const auto &pose : cam.airships)
        {
            if (!pose.visible || !pose.id[0]) continue;
            const bool own = !strncmp(pose.id, "ff8_", 4);
            // Only a guest flying with the camera (the proxy's visible = 2) is its own game's live layer; a parked one
            // is still a copy (its game is frozen by then). A visitor on foot (visible = 3) is their game's layer while
            // it is live.
            if (!own && pose.visible == 2 && newRenderer.ffuLayerLive(pose.id)) { live.push_back(pose.id); continue; }
            if (!own && pose.visible == 3 && cam.globe.characterUnits > 0.0f && newRenderer.ffuLayerLive(pose.id)) { liveCharacters.push_back(pose.id); continue; }
            ExternalMesh *model = airshipMesh(pose.id);
            if (!model) continue;
            drawAirship(pose, model);
            const size_t length = strlen(pose.id);
            const bool isClip = own && ((length > 5 && !strcmp(pose.id + length - 5, "_idle")) || (length > 5 && !strcmp(pose.id + length - 5, "_walk"))
                                        || (length > 4 && !strcmp(pose.id + length - 4, "_run")));
            if (isClip) clip = &pose;
            // Characters (Squall as an NPC) aren't airships: the layer carries the flown ship, or Squall's clip.
            else if (own && strcmp(pose.id, "ff8_squall") && newRenderer.ffuLayerWanted()) layerPoses.push_back(pose);
        }
        if (clip && newRenderer.ffuLayerWanted() && cam.globe.characterUnits > 0.0f)
        {
            layerPoses.clear();
            layerPoses.push_back(*clip);
        }
        // FF8 stretches its own picture sideways near the ground (buildProjection); the layers are square.
        const float scaleX = *(const float *)SSIGPU_SCALE_X, scaleY = *(const float *)SSIGPU_SCALE_Y;
        const float ratio = scaleX > 0.0f ? scaleY / scaleX : 1.0f;
        const float xScale = (1.0f + cam.globe.space * (ratio - 1.0f)) / ratio;
        for (const char *id : live) newRenderer.ffuLayerComposite(id, ::Renderer::FFU_LAYER_DEPTH_FF8, unitsPerR, xScale);
        // A visitor's layer was drawn with FF8's own lens, stretch included (the DLL sends it): placed by aspect only.
        for (const char *id : liveCharacters) newRenderer.ffuLayerComposite(id, ::Renderer::FFU_LAYER_DEPTH_FF8, cam.globe.characterUnits, 1.0f);
        if (!layerPoses.empty() && cam.globe.publishLayer > 0.5f)
        {
            // A clip's layer says which character it shows ("ff8_squall_run" -> "ff8_squall"), in FF8 units per his height.
            char id[sizeof(AirshipPose::id)];
            strncpy(id, layerPoses[0].id, sizeof(id) - 1);
            id[sizeof(id) - 1] = 0;
            if (clip) *strrchr(id, '_') = 0;
            newRenderer.ffuLayerCapture(&Renderer::drawLayerShips, id, clip ? cam.globe.characterUnits : unitsPerR, ::Renderer::FFU_LAYER_DEPTH_FF8);
        }
        struct matrix identity;
        identity_matrix(&identity);
        newRenderer.setWorldViewMatrix(&identity, false);
    }

    // Story variants: the 16-bit world flags at 0x2036BB6 pick each variant file; the hexagon field around
    // Esthar is only shown while ([0x2036BD8] & 0x1F) is 3 or 4 (FF8 wipes its texture otherwise).
    bool Renderer::partVisible(const MeshPart &part) const
    {
        if (part.storyBit < 0) return true;
        const uint16_t flags = *(const uint16_t *)WORLD_STORY_FLAGS;
        if (((flags >> part.storyBit) & 1) != part.storyState) return false;
        if (part.hexField)
        {
            const uint32_t stage = *(const uint32_t *)WORLD_HEXFIELD_STAGE & 0x1F;
            return stage == 3 || stage == 4;
        }
        return true;
    }

    // Whether a shape can be seen this frame. Globe: its centre must lie on the camera's side of the
    // planet, within the horizon (cos = R / (R + h)) widened by the shape's angular size and terrain height.
    // FF8's own bend: within FF8's draw distance of the camera on the wrapping map.
    bool Renderer::shapeVisible(const Shape &shape) const
    {
        const float cx = 0.5f * (shape.min.x + shape.max.x), cz = 0.5f * (shape.min.z + shape.max.z);
        const float size = std::max(shape.max.x - shape.min.x, shape.max.z - shape.min.z);
        if (!cam.hasGlobe)
        {
            const float cameraX = float(cam.refBlockX) * 2048.0f - float(cam.c[0]);
            const float cameraZ = -float(cam.refBlockZ) * 2048.0f - float(cam.c[2]);
            float dx = std::fmod(std::fabs(cx - cameraX), MAP_WIDTH), dz = std::fmod(std::fabs(cz - cameraZ), MAP_HEIGHT);
            dx = std::min(dx, MAP_WIDTH - dx);
            dz = std::min(dz, MAP_HEIGHT - dz);
            return dx * dx + dz * dz < (60000.0f + size) * (60000.0f + size);
        }
        float mx = std::fmod(cx, MAP_WIDTH), mz = std::fmod(-cz, MAP_HEIGHT);
        if (mx < 0.0f) mx += MAP_WIDTH;
        if (mz < 0.0f) mz += MAP_HEIGHT;
        const float lon = mx / PLANET_RADIUS;
        const float lat = 2.0f * std::atan(std::exp((0.5f * MAP_HEIGHT - mz) / PLANET_RADIUS)) - 1.5707963f;
        const float unit[3] = { std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon) };
        const float *up = cam.globe.up;
        const float facing = unit[0] * up[0] + unit[1] * up[1] + unit[2] * up[2];
        const float height = float(cam.c[1]) + cam.globe.altitude;
        const float horizon = std::acos(std::min(1.0f, cam.globe.radius / (cam.globe.radius + std::max(height, 1.0f))));
        // The camera trails the anchor by up to ~15,000 units; a shape spans up to its size; mountains add a little.
        const float margin = (size * 0.75f + 15000.0f) / cam.globe.radius + 0.05f;
        return facing > std::cos(std::min(horizon + margin, 3.1415f));
    }

    // Semi-transparent terrain (FF8 code 0x36) is in materials named ffu_<kind>_semi__* (tools/ff8world).
    void Renderer::drawShapes(bool translucent)
    {
        for (const auto &part : parts)
        {
            if (!partVisible(part)) continue;
            for (uint32_t i = part.firstShape; i < part.firstShape + part.shapeCount; ++i)
            {
                const Shape &shape = mesh.shapes[i];
                const bool isTranslucent = shape.materialName.find("_semi__") != std::string::npos;
                if (isTranslucent != translucent || shapeOffsets[i].indexCount == 0 || shape.pMaterial == nullptr
                    || shape.pMaterial->baseColorTexHandles.empty() || !shapeVisible(shape)) continue;
                auto texture = shape.pMaterial->baseColorTexHandles[shape.pMaterial->texIndex];
                if (!bgfx::isValid(texture)) continue;
                newRenderer.setCullMode(RendererCullMode::DISABLED);
                mesh.bindField3dVertexBuffer(shapeOffsets[i].vertex, shapeOffsets[i].vertexCount);
                mesh.bindField3dIndexBuffer(shapeOffsets[i].index, shapeOffsets[i].indexCount);
                newRenderer.useTexture(texture.idx, RendererTextureSlot::TEX_Y);
                newRenderer.bindTextures();
                newRenderer.draw(true, true, true);
            }
        }
    }

    // FF8's animated water and beaches, converted by tools/ff8world to FFNx's equal-length frames.
    void Renderer::animateTextures()
    {
        for (auto &[name, material] : mesh.materials)
        {
            const int frames = material.baseColorTexHandles.size();
            const int interval = material.frameInterval * common_frame_multiplier;
            if (frames <= 1 || interval <= 0) continue;
            if (frame_counter % interval == 0) material.texIndex = (material.texIndex + 1) % frames;
        }
    }

    void Renderer::draw()
    {
        if (!replacesTerrain() || !cam.valid) return;
        if (*(const uint32_t *)FRAME_DRAWN == 0) return;
        // FFNx only re-applies a render state when FF8's next batch asks for it, so ours must not leak.
        struct driver_state saved;
        gl_save_state(&saved);
        newRenderer.doAlphaTest(false);
        newRenderer.isFBTexture(false);
        newRenderer.doModulateAlpha(true);

        newRenderer.setInterpolationQualifier(SMOOTH);
        newRenderer.setPrimitiveType();
        newRenderer.isTLVertex(false);
        newRenderer.setBlendMode(RendererBlendMode::BLEND_NONE);
        newRenderer.doTextureFiltering(true);
        newRenderer.doMirrorTextureWrap(false);
        newRenderer.isExternalTexture(true);
        newRenderer.doDepthTest(true);
        newRenderer.doDepthWrite(true);
        newRenderer.doScissorTest(false);
        newRenderer.setSphericalWorldRate(0.0f);
        newRenderer.setFogEnabled(false);

        struct matrix identity, projection;
        identity_matrix(&identity);
        buildProjection(projection);
        newRenderer.setD3DViweport(&identity);
        newRenderer.setD3DProjectionRaw(&projection);
        newRenderer.setWorldViewMatrix(&identity, false);

        newRenderer.ffuLayerPrepare();   // once: the live layer's target and reads, before any flight needs them
        animateTextures();
        float globe[36];
        fillGlobeUniform(globe, Part::World);
        const bool shadows = bgfx::isValid(ownClouds) && cloudFade() > 0.0f;
        globe[20] = shadows ? cloudFade() : 0.0f;   // cloud shadows on the terrain (FFNx.frag)
        newRenderer.setFF8WorldMesh(true, globe, 1.0f, cloudDrift());
        newRenderer.isTexture(true);
        newRenderer.setCommonUniforms();
        if (shadows) newRenderer.useTexture(ownClouds.idx, RendererTextureSlot::TEX_U);
        drawShapes(false);
        if (shadows) newRenderer.useTexture(0, RendererTextureSlot::TEX_U);


        if (cam.hasGlobe && !caps.shapes.empty())
        {
            fillGlobeUniform(globe, Part::PolarCap);
            newRenderer.setFF8WorldMesh(true, globe);
            newRenderer.isTexture(false);
            newRenderer.setCommonUniforms();
            uint32_t vertexOffset = 0, indexOffset = 0;
            for (auto &shape : caps.shapes)
            {
                newRenderer.setCullMode(RendererCullMode::DISABLED);
                caps.bindField3dVertexBuffer(vertexOffset, shape.vertices.size());
                caps.bindField3dIndexBuffer(indexOffset, shape.indices.size());
                newRenderer.draw(true, true, true);
                vertexOffset += shape.vertices.size();
                indexOffset += shape.indices.size();
            }
        }

        drawOtherPlanets();
        drawAirships();

        newRenderer.discardAllBindings();
        newRenderer.setFF8WorldMesh(false);
        newRenderer.isExternalTexture(false);
        gl_load_state(&saved);
    }
}
