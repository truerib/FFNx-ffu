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
#include <vector>
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
#include "../../ffu_airships.h"

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
    const Control *cameraOverride() { auto c = control(); return c && c->version >= 3 && c->cameraOverride ? c : nullptr; }

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
        out[3] = cameraOverride() ? 1.0f : 0.0f;   // flying: the shared lens at every height
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

    static const Airship *bridgePeople()
    {
        static const Airship *found = nullptr;
        static DWORD lastLook = 0;
        if (found) return found;
        DWORD now = GetTickCount();
        if (lastLook && now - lastLook < 1000) return nullptr;
        lastLook = now;
        HMODULE bridge = bridgeModule();
        found = bridge ? (const Airship *)GetProcAddress(bridge, "ffu_ff7_people") : nullptr;
        return found;
    }

    static bool ownModel(const char *id) { return !strncmp(id, "ff7_", 4); }

    // FF7 units per character height (Control v6): the visitor's in FF7, or Cloud's own while he walks elsewhere.
    static float characterUnits()
    {
        const Control *c = control();
        return c && c->version >= 6 && c->characterUnits > 0.0f ? c->characterUnits : 0.0f;
    }

    // A character clip's id ("ff7_cloud_run", tools/airships) -> the character it shows ("ff7_cloud"): what the layer
    // says it shows, so the host matches it with its stand-in. An airship's id stays as it is.
    static void layerId(const char *id, char (&out)[sizeof(Airship::id)])
    {
        strncpy(out, id, sizeof(out) - 1);
        out[sizeof(out) - 1] = 0;
        for (const char *clip : { "_idle", "_walk", "_run" })
        {
            const size_t length = strlen(out), tail = strlen(clip);
            if (length > tail && !strcmp(out + length - tail, clip)) { out[length - tail] = 0; break; }
        }
    }

    // FF7 units per planet radius for the flight camera this frame (the bridge's, or the globe's radius).
    static float unitsPerR()
    {
        const Control *c = control();
        return c && c->version >= 4 && c->unitsPerR > 0.0f ? c->unitsPerR : 46936.7026f;
    }

    // mesh/airships/<id>.gltf in native axes, textures from <mod_path>/world, animated when it has frames
    // (src/ffu_airships.cpp). nullptr: not found.
    static ExternalMesh *airshipMesh(const char *id)
    {
        return ffu::airshipModel(id, sizeof(Airship::id), "FF7 world");
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

    // The ships drawn this frame, for the live layer's replay at the end of it (src/ffu_layer.cpp).
    struct LayerShip { Airship ship; ExternalMesh *model; };
    static std::vector<LayerShip> layerShips;
    static struct matrix layerView;

    static void drawLayerShips()
    {
        for (const auto &entry : layerShips) drawAirship(entry.ship, entry.model, &layerView);
        layerShips.clear();
    }

    // Every model slot the bridge filled: the airships, then the people (a visitor's stand-in, Cloud as an NPC, or
    // Cloud's own clip while he walks another world). An airship or a visitor from another game whose own game is
    // sending its live layer isn't drawn here: that layer is composited instead (src/ffu_layer.cpp, Rule 64).
    static void drawAirships()
    {
        const Airship *ships = bridgeAirships();
        if (!ships || !newRenderer.isFF7GlobeActive()) return;
        const Airship *people = bridgePeople();
        const Airship *slots[AIRSHIP_SLOTS + PEOPLE_SLOTS] = {};
        bool live[AIRSHIP_SLOTS + PEOPLE_SLOTS] = {};
        for (int i = 0; i < AIRSHIP_SLOTS; ++i) slots[i] = &ships[i];
        for (int i = 0; i < PEOPLE_SLOTS && people; ++i) slots[AIRSHIP_SLOTS + i] = &people[i];

        struct matrix view;
        ::memcpy(&view.m[0][0], newRenderer.getViewMatrix(), sizeof(view.m));
        bool stateSet = false;
        for (int i = 0; i < AIRSHIP_SLOTS + PEOPLE_SLOTS; ++i)
        {
            if (!slots[i]) continue;
            const Airship &ship = *slots[i];
            if (ship.mode != AIRSHIP_WITH_CAMERA && ship.mode != AIRSHIP_ON_PLANET) continue;
            // Only a guest flying with the camera is its own game's live layer: a parked one is still a copy (its game is
            // frozen), and swapping it for the last flying frame drew it in the wrong place for a moment. A visitor on foot
            // (people slot 0) is their game's layer while it is live (docs/crossover-design.md "Characters, live").
            const bool flyingGuest = i < AIRSHIP_SLOTS && ship.mode == AIRSHIP_WITH_CAMERA;
            const bool visitor = i == AIRSHIP_SLOTS && characterUnits() > 0.0f;
            const bool composited = (flyingGuest || visitor) && !ownModel(ship.id) && newRenderer.ffuLayerLive(ship.id);
            ExternalMesh *model = composited ? nullptr : airshipMesh(ship.id);
            if (!model && !composited) continue;
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
            if (composited) live[i] = true;
            else drawAirship(ship, model, &view);
        }
        if (!stateSet) return;
        for (int i = 0; i < AIRSHIP_SLOTS + PEOPLE_SLOTS; ++i)
            if (live[i]) newRenderer.ffuLayerComposite(slots[i]->id, Renderer::FFU_LAYER_DEPTH_FF7, i < AIRSHIP_SLOTS ? unitsPerR() : characterUnits());
        // The live layer (FFU_FF7_LAYER=1, src/ffu_layer.cpp): FF7's own airship again, alone, at the end of the
        // frame, into the layer target another game composites (docs/crossover-design.md). Guests aren't in it:
        // they are their own games' to send. While Cloud walks another world, it is his clip from that world's camera
        // (people slot 0, with the camera), in FF7 units per his height.
        const Control *c = control();
        if (newRenderer.ffuLayerWanted() && c && c->version >= 5 && c->publishLayer)
        {
            layerShips.clear();
            const Airship *person = people && people[0].mode == AIRSHIP_WITH_CAMERA && ownModel(people[0].id) && characterUnits() > 0.0f ? &people[0] : nullptr;
            if (person)
            {
                if (ExternalMesh *model = airshipMesh(person->id)) layerShips.push_back({ *person, model });
            }
            else
                for (int i = 0; i < AIRSHIP_SLOTS; ++i)
                {
                    const Airship &ship = ships[i];
                    if ((ship.mode != AIRSHIP_WITH_CAMERA && ship.mode != AIRSHIP_ON_PLANET) || !ownModel(ship.id)) continue;
                    if (ExternalMesh *model = airshipMesh(ship.id)) layerShips.push_back({ ship, model });
                }
            layerView = view;
            if (!layerShips.empty())
            {
                char id[sizeof(Airship::id)];
                layerId(layerShips[0].ship.id, id);
                newRenderer.ffuLayerCapture(&drawLayerShips, id, person ? characterUnits() : unitsPerR(), Renderer::FFU_LAYER_DEPTH_FF7);
            }
        }
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
        // A visitor on foot: Cloud's model (the single on-foot slot, ids 0-2) isn't drawn on the player.
        uint32_t *player = c && c->version >= 4 && c->hidePlayerModel ? modelDrawFlag(0) : nullptr;
        if (player == hidden) player = nullptr;
        const uint32_t savedPlayer = player ? *player : 0;
        if (player) *player = 0;
        // The game's models bend with the planet: say so here instead of relying on the rate the draw before
        // happened to leave (drawAirships sets 0 after itself; docs/world-map-audit.md #13).
        newRenderer.setSphericalWorldRate(1.0f);
        newRenderer.ffuLayerPrepare();   // once: the live layer's target and reads, before any flight needs them
        ((void (*)(int))::ff7_externals.world_draw_all_3d_model_74C6B0)(param);
        if (hidden) *hidden = saved;
        if (player) *player = savedPlayer;
        drawAirships();
    }
}
