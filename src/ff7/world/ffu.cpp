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

#include <windows.h>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <cstring>

#include "../../globals.h"
#include "../../renderer.h"
#include "../../external_mesh.h"
#include "../../gl.h"
#include "../../cfg.h"
#include "../../log.h"
#include "../../matrix.h"
#include "camera.h"
#include "ffu.h"

extern "C" __declspec(dllexport) ffu::ff7::Ffnx ffu_ff7_ffnx = {};

namespace ffu::ff7
{
    static bool drawingWithCamera = false;   // ff7Globe[0].z while an airship that flies with the camera is drawn

    static HMODULE bridgeModule()
    {
        return GetModuleHandleA("dinput8.dll");
    }

    const Control *control()
    {
        static const Control *found = nullptr;
        static DWORD lastLook = 0;
        if (found) return found;
        DWORD now = GetTickCount();
        if (lastLook && now - lastLook < 1000) return nullptr;   // look again once a second
        lastLook = now;
        HMODULE bridge = bridgeModule();
        auto candidate = bridge ? (const Control *)GetProcAddress(bridge, "ffu_ff7_control") : nullptr;
        if (candidate && candidate->magic == 0x54433746 && candidate->version >= 1) found = candidate;   // 'F7CT'
        return found;
    }

    float maxZoom(float fallback) { auto c = control(); return c && c->maxZoom > 0.0f ? c->maxZoom : fallback; }
    float pitchMin(float fallback) { auto c = control(); return c ? c->pitchMin : fallback; }
    float pitchMax(float fallback) { auto c = control(); return c ? c->pitchMax : fallback; }
    int highwindCeiling(int fallback) { auto c = control(); return c && c->highwindCeiling > 0 ? c->highwindCeiling : fallback; }

    static void publish()
    {
        if (ffu_ff7_ffnx.magic) return;
        ffu_ff7_ffnx.version = 1;
        ffu_ff7_ffnx.camera = &::ff7::world::camera;
        ffu_ff7_ffnx.viewMatrix = newRenderer.getViewMatrix();
        ffu_ff7_ffnx.invViewMatrix = newRenderer.getInvViewMatrix();
        ffu_ff7_ffnx.d3dProjection = newRenderer.getD3DProjectionMatrix();
        ffu_ff7_ffnx.d3dViewport = newRenderer.getD3DViewportMatrix();
        ffu_ff7_ffnx.magic = 0x58463746;   // 'F7FX'
    }

    bool onWorldMap()
    {
        return !ff8 && getmode_cached()->driver_mode == MODE_WORLDMAP;
    }

    void globeUniform(float out[8])
    {
        for (int i = 0; i < 8; i++) out[i] = 0.0f;
        if (ff8) return;
        publish();
        if (!onWorldMap()) return;
        // docs/space.md "Clouds": one turn east per 6 h of real time, from the system clock.
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        const uint64_t ms = ((((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime) / 10000ULL) - 11644473600000ULL;
        out[0] = 1.0f;
        out[1] = (float)(ms % 21600000ULL) * (6.2831853f / 21600000.0f);
        out[2] = drawingWithCamera ? 1.0f : 0.0f;
    }

    // ---- Airships (docs/space.md "The airship's state") ---------------------------------------------

    static const Airship *bridgeAirships()
    {
        static const Airship *found = nullptr;
        static DWORD lastLook = 0;
        if (found) return found;
        DWORD now = GetTickCount();
        if (lastLook && now - lastLook < 1000) return nullptr;
        lastLook = now;
        HMODULE bridge = bridgeModule();
        found = bridge ? (const Airship *)GetProcAddress(bridge, "ffu_ff7_airships") : nullptr;
        return found;
    }

    // mesh/airships/<id>.gltf in native axes, textures from <mod_path>/world (as for FF8). nullptr: not found.
    static ExternalMesh *airshipMesh(const char *id)
    {
        static std::map<std::string, std::unique_ptr<ExternalMesh>> meshes;
        const std::string key(id, strnlen(id, sizeof(Airship::id)));
        auto found = meshes.find(key);
        if (found != meshes.end()) return found->second.get();
        auto &slot = meshes[key];
        const std::filesystem::path file = std::filesystem::path(basedir) / external_mesh_path / "airships" / (key + ".gltf");
        auto mesh = std::make_unique<ExternalMesh>();
        std::string path = file.generic_string();
        std::string textures = (std::filesystem::path(basedir) / mod_path / "world").generic_string() + "/";
        if (std::filesystem::exists(file) && mesh->importExternalMeshGltfFile(path.data(), textures.data(), true) && !mesh->shapes.empty())
        {
            ffnx_info("FF7 world: airship %s loaded (%u shapes)\n", key.c_str(), (uint32_t)mesh->shapes.size());
            slot = std::move(mesh);
        }
        else ffnx_error("FF7 world: no airship model %s\n", path.c_str());
        return slot.get();
    }

    static void drawAirship(const Airship &ship, ExternalMesh *model, struct matrix *view)
    {
        struct matrix place = {}, worldView;
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column) place.m[column][row] = ship.model[row * 4 + column];   // row vectors
            place.m[3][row] = ship.model[row * 4 + 3];
        }
        place._44 = 1.0f;
        if (ship.mode == AIRSHIP_ON_PLANET) multiply_matrix(&place, view, &worldView);
        else worldView = place;

        drawingWithCamera = ship.mode == AIRSHIP_WITH_CAMERA;
        newRenderer.setSphericalWorldRate(ship.mode == AIRSHIP_ON_PLANET ? 1.0f : 0.0f);
        newRenderer.setWorldViewMatrix(&worldView);
        newRenderer.setCommonUniforms();

        uint32_t vertexOffset = 0, indexOffset = 0;
        for (const auto &shape : model->shapes)
        {
            if (shape.pMaterial && !shape.pMaterial->baseColorTexHandles.empty()
                && bgfx::isValid(shape.pMaterial->baseColorTexHandles[shape.pMaterial->texIndex]))
            {
                // No culling: the glTFs keep each game's own winding, and FFNx's FRONT mode is broken.
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
        drawingWithCamera = false;
    }

    static void drawAirships()
    {
        const Airship *ships = bridgeAirships();
        if (!ships || !newRenderer.isFF7GlobeActive()) return;

        struct matrix view;
        ::memcpy(&view.m[0][0], newRenderer.getViewMatrix(), sizeof(view.m));
        bool stateSet = false;
        for (int i = 0; i < AIRSHIP_SLOTS; ++i)
        {
            const Airship &ship = ships[i];
            if (ship.mode != AIRSHIP_WITH_CAMERA && ship.mode != AIRSHIP_ON_PLANET) continue;
            ExternalMesh *model = airshipMesh(ship.id);
            if (!model) continue;
            if (!stateSet)
            {
                // The world's own projection, as FFNx's cloud and meteor meshes set it.
                auto game_object = (struct ff7_game_obj *)common_externals.get_game_object();
                auto polygon_set = game_object ? (ff7_polygon_set *)game_object->polygon_set_2EC : nullptr;
                if (!polygon_set || !polygon_set->matrix_set || !polygon_set->matrix_set->matrix_projection) return;
                newRenderer.setD3DProjection(polygon_set->matrix_set->matrix_projection);
                newRenderer.setD3DViweport(&d3dviewport_matrix);
                newRenderer.setInterpolationQualifier(SMOOTH);
                newRenderer.setPrimitiveType();
                newRenderer.isTLVertex(false);
                newRenderer.setBlendMode(RendererBlendMode::BLEND_NONE);
                newRenderer.doTextureFiltering(true);
                newRenderer.isExternalTexture(true);
                newRenderer.isTexture(true);
                newRenderer.doDepthTest(true);
                newRenderer.doDepthWrite(true);
                stateSet = true;
            }
            drawAirship(ship, model, &view);
        }
        if (!stateSet) return;
        newRenderer.discardAllBindings();
        newRenderer.setSphericalWorldRate(0.0f);
        struct matrix identity;
        identity_matrix(&identity);
        newRenderer.setWorldViewMatrix(&identity);
    }

    // The model slot FF7 draws a world model id from (the game's own lookup, FUN_00768a37): ids 0-2 (on
    // foot) use the single slot at 0xE2C430, ids 3-31 the table at 0xE2C808 through the id -> slot bytes at
    // 0xE3B0F8. A slot is drawn while its dword at +0x24 is non-zero (world_draw_all_3d_model_74C6B0).
    static uint32_t *modelDrawFlag(int id)
    {
        if (id < 0 || id >= 0x20) return nullptr;
        if (id < 3) return (uint32_t *)(0xE2C430 + 0x24);
        const int slot = (int)*(int8_t *)(0xE3B0F8 + id);
        if (slot < 1) return nullptr;
        return (uint32_t *)(0xE2C808 + (slot - 1) * 0xB8 + 0x24);
    }

    void drawAllModels(int param)
    {
        const Control *c = control();
        uint32_t *hidden = c && c->version >= 2 && c->hideModel >= 0 ? modelDrawFlag(c->hideModel) : nullptr;
        const uint32_t saved = hidden ? *hidden : 0;
        if (hidden) *hidden = 0;
        ((void (*)(int))::ff7_externals.world_draw_all_3d_model_74C6B0)(param);
        if (hidden) *hidden = saved;
        drawAirships();
    }
}
