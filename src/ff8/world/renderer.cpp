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
        constexpr uint32_t SSIGPU_OFFSET_X = 0x1CA89E0;
        constexpr uint32_t SSIGPU_OFFSET_Y = 0x1CA89E4;
        constexpr uint32_t FRAME_DRAWN     = 0xB7CC24;    // 0 when FF8 skips drawing this frame
        constexpr uint32_t GLOBE_MAGIC     = 0x31474646;  // 'FFG1'
        constexpr uint32_t WORLD_STORY_FLAGS    = 0x2036BB6;   // u16: which story variant of each changed segment
        constexpr uint32_t WORLD_HEXFIELD_STAGE = 0x2036BD8;

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
            return replace_call_function(site, hook);
        }

        void hook_terrain()       { renderer.snapshot(); ((plain_fn)orig_terrain)(); }
        void hook_terrain_nofog() { renderer.snapshot(); ((plain_fn)orig_terrain_nofog)(); }
        void hook_draw_ot(int *ot)         { renderer.draw(); ((draw_ot_fn)orig_draw_ot)(ot); }
        void hook_draw_ot_paused(int *ot)  { renderer.draw(); ((draw_ot_fn)orig_draw_ot_paused)(ot); }
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

        // Each shape's place in the shared vertex/index buffers, so story variants can be skipped.
        uint32_t vertexOffset = 0, indexOffset = 0;
        for (const auto &shape : mesh.shapes)
        {
            shapeOffsets.push_back({ vertexOffset, indexOffset });
            vertexOffset += shape.vertices.size();
            indexOffset += shape.indices.size();
        }

        buildPolarCaps();
        loadOtherPlanets();
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
            HMODULE proxy = GetModuleHandleA("dinput.dll");
            auto state = proxy ? (GlobeState *)GetProcAddress(proxy, "ffu_ff8_globe") : nullptr;
            if (state && state->magic == GLOBE_MAGIC) dllGlobe = state;
        }
        cam.hasGlobe = dllGlobe != nullptr && dllGlobe->mode > 0.5f && dllGlobe->mode < 1.5f;
        if (dllGlobe)
        {
            cam.globe = *dllGlobe;
            dllGlobe->meshActive = replacesTerrain() ? 1 : 0;
        }
        cam.valid = true;
    }

    // FF8's projection with its rotation folded in (docs/ff8-3d-world-renderer.md 1.7/1.8): FF8's own
    // 1/8-pixel PS1 screen, mapped by the live ssigpu scale/offset into FFNx's TL space (wide viewport
    // included), then FFNx's orthographic matrix. Depth is replaced in the shader (FF8Depth).
    void Renderer::buildProjection(struct matrix &projection) const
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
        lens._31 = 2.0f * (offsetX + scaleX * cam.ofx - left) / width - 1.0f;
        lens._22 = -2.0f * scaleY * cam.H / height;
        lens._32 = 1.0f - 2.0f * (offsetY + scaleY * cam.ofy) / height;
        lens._33 = 1.0f;
        lens._34 = 1.0f;   // w = view z
        multiply_matrix(&rotation, &lens, &projection);
    }

    void Renderer::fillGlobeUniform(float out[24], Part part, const float* planetOffset) const
    {
        ::memset(out, 0, sizeof(float) * 24);
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
            out[23] = PLANET_RADIUS;   // FF8 units per planet radius, in space (docs/space.md)
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
        if (otherPlanets.empty()) return;

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
        ffnx_info("FF8 world mesh: %u other planets loaded\n", (uint32_t)otherPlanets.size());
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
            float globe[24];
            fillGlobeUniform(globe, Part::OtherPlanet, planet.offset);
            newRenderer.setFF8WorldMesh(true, globe);
            newRenderer.setCommonUniforms();
            newRenderer.setCullMode(RendererCullMode::DISABLED);
            sphere.bindField3dVertexBuffer(0, shape.vertices.size());
            sphere.bindField3dIndexBuffer(0, shape.indices.size());
            newRenderer.useTexture(planet.texture.idx, RendererTextureSlot::TEX_Y);
            newRenderer.bindTextures();
            newRenderer.draw(true, true, true);
        }
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
                if (isTranslucent != translucent || shape.indices.empty() || shape.pMaterial == nullptr
                    || shape.pMaterial->baseColorTexHandles.empty()) continue;
                auto texture = shape.pMaterial->baseColorTexHandles[shape.pMaterial->texIndex];
                if (!bgfx::isValid(texture)) continue;
                newRenderer.setCullMode(RendererCullMode::DISABLED);
                mesh.bindField3dVertexBuffer(shapeOffsets[i].vertex, shape.vertices.size());
                mesh.bindField3dIndexBuffer(shapeOffsets[i].index, shape.indices.size());
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

        animateTextures();
        float globe[24];
        fillGlobeUniform(globe, Part::World);
        newRenderer.setFF8WorldMesh(true, globe);
        newRenderer.isTexture(true);
        newRenderer.setCommonUniforms();
        drawShapes(false);

        {
            newRenderer.setBlendMode(RendererBlendMode::BLEND_AVG);
            newRenderer.doDepthWrite(false);
            newRenderer.setCommonUniforms();
            drawShapes(true);
            newRenderer.setBlendMode(RendererBlendMode::BLEND_NONE);
            newRenderer.doDepthWrite(true);
        }

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

        newRenderer.discardAllBindings();
        newRenderer.setFF8WorldMesh(false);
        newRenderer.isExternalTexture(false);
    }
}
