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

// Final Fantasy Universe: FF8's battle lighting (docs/battle-look.md "FF8 lighting"), the battle look's shared light for
// FF8, whose battles reach FFNx only as screen-space triangles (FF8's own software GTE projects everything), so FFNx's
// own lighting (FF7's, 3D) can't light them.
//
// The survey (ffu_survey.cpp, FFU_FF8_SURVEY=1) found that in every FF8 battle frame all of FF8's 3D comes first, with
// depth tested and written, and its flat 2D (the HUD: every rhw 1) only after. So at the frame's first flat draw (or at
// the flip, in a frame with none) this pass draws the picture so far lit (FFNx.ffuff8look.frag), from FFNx's colour and
// FF8's own depth, into a target of its own, copies that back over FFNx's colour and gives FFNx a new view to carry on
// with. Its views are FFNx's own count (the pass falls between FFNx's draws); the one with a frame buffer set draws into
// the pass's own target, so a later frame's FFNx view on that id (FF8's battles never reset theirs) touches only that.
// FF8's depth is linear in view depth, in one of two ways (sub_461220, 0x461210): its characters and effects at
// 1.5235901e-5 a unit + 0.0015, its stage at its globals' slope and offset (0xB7CEE0; 0x1CA9E9C x 0x1CA882C), which push
// the stage back as the PlayStation's ordering table kept it behind them. Each frame the pass learns from FF8's draws
// which depths are the stage's (ffuFF8LookDraw), so the floor is lit, shadowed and hazed at its true depth; the main
// light stands in FF8's battle world, turned into the view by FF8's battle camera matrix each frame.
// Off with the look's lighting switched off, in HDR, with MSAA, and while a battle frame goes to the live layer.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "renderer.h"
#include "cfg.h"
#include "common.h"
#include "gl.h"
#include "globals.h"
#include "log.h"
#include "utils.h"

bool ffuLookLighting();   // ffu_look.cpp
bool ffuLookShown();
bool ffuLookAtmosphere();
float ffuLookLevel(int which);
float ffuLookSplit();

namespace
{
    // FF8's lens (as the fork's world renderer reads it: src/ff8/world/renderer.cpp)
    constexpr uint32_t GTE_H = 0x1CA92E4, GTE_OFX = 0x1CA92DE, GTE_OFY = 0x1CA92E2;
    constexpr uint32_t SSIGPU_SCALE_X = 0xB7CC14, SSIGPU_SCALE_Y = 0xB7CC18, SSIGPU_OFFSET_X = 0x1CA89E0, SSIGPU_OFFSET_Y = 0x1CA89E4;
    // FF8's depth (sub_461220): its own, and the stage's from its globals at the draw (Richard 2026-10-22: "I can't see any
    // other changes": the beach's sand and sea read as far as the sky, so the pass left them as FF8 drew them)
    constexpr float OWN_SLOPE = 1.5235901e-05f, DEPTH_BIAS = 0.0015f;
    constexpr uint32_t STAGE_SLOPE = 0xB7CEE0, STAGE_OFFSET_A = 0x1CA9E9C, STAGE_OFFSET_B = 0x1CA882C;
    // The look's balance (the same as FF9's: mods/ff9-globe BattleScene.cs): ambient, main light, wrap, contact
    // (stronger than FF9's: FF8's own colours carry their own soft light; Richard 2026-10-22: "I can't see any other
    // changes": a surface turned to the light about 24% brighter, one turned away or in shadow about 35% darker)
    constexpr float AMBIENT = 0.35f, KEY = 1.1f, WRAP = 0.35f, CONTACT = 0.85f;
    // The haze (as FF9's outdoors: BattleScene.cs): from half again the depth FF8's 3D stands at, full (its most) at its
    // backdrop (the sky dome: FF8 draws its sky as far 3D, with depth), toward the backdrop's own colour. The shader finds
    // the backdrop from the picture's top (as FF7's does: never from one far vertex, which a summon moves); the backdrop
    // itself is neither lit nor hazed.
    constexpr float HAZE_MOST = 0.5f, HAZE_START = 1.5f;
    // FF8's battle camera: its view matrix as FF8's LookAt writes it each frame (0x503230 -> 0x50CB40: int16 rows,
    // 4.12, view = M * world; side-session research 2026-10-22 battle-camera-stick.md), so the main light stays put in
    // FF8's battle world as its cameras swing. Toward the light in that world (y down): high, from one side.
    constexpr uint32_t BATTLE_VIEW_MATRIX = 0x1D97778;
    constexpr float LIGHT_X = -0.6f, LIGHT_Y = -0.75f, LIGHT_Z = 0.25f;
    // ... and, should the matrix read as nothing, toward it in view space (x right, y down, z ahead): above, to the
    // left, a little toward the camera.
    constexpr float VIEW_LIGHT_X = -0.45f, VIEW_LIGHT_Y = -0.75f, VIEW_LIGHT_Z = -0.48f;

    int programState = 0;   // 0 not tried, 1 ready, -1 unavailable
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uniformLens = BGFX_INVALID_HANDLE, uniformCam = BGFX_INVALID_HANDLE, uniformFrame = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uniformDepth = BGFX_INVALID_HANDLE, uniformLight = BGFX_INVALID_HANDLE, uniformLook = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uniformHaze = BGFX_INVALID_HANDLE, uniformStage = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle samplerScene = BGFX_INVALID_HANDLE, samplerDepth = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout quadLayout;
    bgfx::FrameBufferHandle litTarget = BGFX_INVALID_HANDLE;
    uint16_t litWidth = 0, litHeight = 0;
    bgfx::ViewId litView = 0;   // the view that last drew into it (given no frame buffer before it is remade)
    bool passedThisFrame = false;
    uint32_t passes = 0;
    // This frame's 3D draws: the depth they stand at (1 / rhw, every fourth vertex) and how FF8 wrote their vertices'
    // depths: its own way, the stage's (one slope and offset in a frame, its depths apart from the own ones: told apart
    // in the shader by depth alone), or neither (counted).
    double sumX = 0.0;
    uint32_t samples = 0;
    struct Depths
    {
        uint32_t own = 0, stage = 0, neither = 0;   // (vertices)
        float ownMin = 2.0f, ownMax = -1.0f, stageMin = 2.0f, stageMax = -1.0f, slope = 0.0f, offset = 0.0f;
        bool stageWay = false, mixed = false;   // (the stage's draws made in more than one way)
    } depths;
    // A diagnostic (one battle frame in 600): every tenth 3D draw's first vertices logged with their place in FF8's
    // battle world (its camera's matrix and translation), and the heights of every fourth vertex counted in 50-unit bands.
    bool recording = false;
    uint32_t drawsThisFrame = 0;
    constexpr int BANDS = 160;
    uint32_t heights[BANDS] = {};

    struct QuadVertex { float x, y, z, u, v; };
}

// A vertex's height in FF8's battle world (y down), from its screen place and 1 / rhw as the view depth, FF8's lens and its
// battle camera (view = translation + matrix * world, the matrix int16 4.12 at BATTLE_VIEW_MATRIX, its translation three
// int32 after it, as FF8's LookAt 0x50CB40 writes them)
static float worldHeight(const struct nvertex &vertex)
{
    const float scaleX = *(const float *)SSIGPU_SCALE_X * 8.0f, scaleY = *(const float *)SSIGPU_SCALE_Y * 8.0f;
    const float lensH = float(*(const uint32_t *)GTE_H & 0xFFFF);
    if (lensH <= 0.0f || !(scaleX > 0.0f) || !(scaleY > 0.0f)) return 0.0f;
    const float Z = 1.0f / vertex.color.w;
    const float view[3] = {
        ((vertex._.x - *(const float *)SSIGPU_OFFSET_X) / scaleX - float(*(const int16_t *)GTE_OFX)) / lensH * Z,
        ((vertex._.y - *(const float *)SSIGPU_OFFSET_Y) / scaleY - float(*(const int16_t *)GTE_OFY)) / lensH * Z, Z };
    const int16_t *matrix = (const int16_t *)BATTLE_VIEW_MATRIX;
    const int32_t *translation = (const int32_t *)(BATTLE_VIEW_MATRIX + 0x14);
    float height = 0.0f;
    for (int row = 0; row < 3; row++)
        height += matrix[row * 3 + 1] / 4096.0f * (view[row] - float(translation[row]));
    return height;
}

static void recordDraw(const struct nvertex *vertices, uint32_t vertexcount)
{
    for (uint32_t i = 0; i < vertexcount; i += 4)
        if (vertices[i].color.w > 0.0f && vertices[i].color.w < 0.5f)
        {
            const int band = int(floorf(worldHeight(vertices[i]) / 50.0f)) + BANDS / 2;
            heights[std::clamp(band, 0, BANDS - 1)]++;
        }
    if (drawsThisFrame++ % 10 != 0)
        return;
    char line[512];
    int used = _snprintf(line, sizeof(line), "FFU FF8 depth diagnostic: draw %u, %u vertices, stage globals %.6g %.6g %.6g:", drawsThisFrame - 1,
                         vertexcount, *(const float *)STAGE_SLOPE, *(const float *)STAGE_OFFSET_A, *(const float *)STAGE_OFFSET_B);
    for (uint32_t i = 0; i < vertexcount && i < 3 && used > 0 && used < int(sizeof(line)) - 80; i++)
        used += _snprintf(line + used, sizeof(line) - used, " (%.1f, %.1f, 1/rhw %.1f, z %.6f, height %.0f)", vertices[i]._.x, vertices[i]._.y,
                          vertices[i].color.w > 0.0f ? 1.0f / vertices[i].color.w : 0.0f, vertices[i]._.z, worldHeight(vertices[i]));
    ffnx_info("%s\n", line);
}

// How FF8 wrote this 3D draw's depths (sub_461220), vertex by vertex: its own way, or the stage's from its globals now
// (they still hold what the draw's vertices were made with); each way's range of depths kept.
static void countDepths(const struct nvertex *vertices, uint32_t vertexcount)
{
    const float slope = *(const float *)STAGE_SLOPE, offset = *(const float *)STAGE_OFFSET_A * *(const float *)STAGE_OFFSET_B;
    bool stageSeen = false;
    for (uint32_t i = 0; i < vertexcount; i++)
    {
        const float rhw = vertices[i].color.w, z = vertices[i]._.z;
        if (!(rhw > 0.0f && rhw < 0.5f)) continue;
        const float view = 1.0f / rhw;
        const bool own = fabsf(z - (OWN_SLOPE * view + DEPTH_BIAS)) < 1e-5f + 1e-5f * z;
        const bool stage = fabsf(z - (slope * view + offset + DEPTH_BIAS)) < 1e-5f + 1e-4f * z;
        if (own && stage)
            continue;   // (where the two ways meet: the same view depth either way)
        if (own)
        {
            depths.own++;
            depths.ownMin = std::min(depths.ownMin, z);
            depths.ownMax = std::max(depths.ownMax, z);
        }
        else if (stage)
        {
            depths.stage++;
            depths.stageMin = std::min(depths.stageMin, z);
            depths.stageMax = std::max(depths.stageMax, z);
            stageSeen = true;
        }
        else
            depths.neither++;
    }
    if (!stageSeen)
        return;
    if (!depths.stageWay)
    {
        depths.stageWay = true;
        depths.slope = slope;
        depths.offset = offset;
    }
    else if (fabsf(slope - depths.slope) > 1e-6f * fabsf(depths.slope) + 1e-12f || fabsf(offset - depths.offset) > 1e-6f)
        depths.mixed = true;
}

// The stage's depth this frame for the shader: x its slope, y its offset, z where its depths begin, w 1 when they lie
// beyond the own ones, -1 when nearer, 0 when the two can't be told apart by depth (then all is read the own way).
static void stageDepths(float stage[4])
{
    stage[0] = depths.slope;
    stage[1] = depths.offset + DEPTH_BIAS;
    stage[2] = 2.0f;
    stage[3] = 0.0f;
    if (depths.stage == 0 || depths.mixed || !(depths.slope > 1e-9f))
        return;
    if (depths.own == 0 || depths.stageMin > depths.ownMax + 1e-4f)
    {
        stage[2] = depths.own == 0 ? depths.stageMin - 1e-4f : 0.5f * (depths.ownMax + depths.stageMin);
        stage[3] = 1.0f;
    }
    else if (depths.stageMax < depths.ownMin - 1e-4f)
    {
        stage[2] = 0.5f * (depths.stageMax + depths.ownMin);
        stage[3] = -1.0f;
    }
}

// From gl_draw_indexed_primitive, for every draw of an FF8 battle frame before it is drawn: its 3D vertices feed the depth
// depths; true at the frame's first flat draw after some 3D, when the lighting pass goes first.
bool ffuFF8LookDraw(uint32_t vertextype, const struct nvertex *vertices, uint32_t vertexcount)
{
    if (!ff8 || passedThisFrame || vertextype != TLVERTEX || getmode_cached()->driver_mode != MODE_BATTLE || !(ffuLookLighting() || ffuLookAtmosphere())
        || !ffuLookShown())
        return false;
    bool flat = true;
    for (uint32_t i = 0; i < vertexcount; i++)
        if (vertices[i].color.w != 1.0f)
        {
            flat = false;
            break;
        }
    if (!flat)
    {
        countDepths(vertices, vertexcount);
        if (recording) recordDraw(vertices, vertexcount);
        for (uint32_t i = 0; i < vertexcount; i += 4)
        {
            const float rhw = vertices[i].color.w, z = vertices[i]._.z;
            if (!(rhw > 0.0f && rhw < 0.5f) || !(z > 0.0f && z < 1.0f)) continue;   // (FFNx maps an infinite rhw to 1)
            sumX += 1.0 / rhw;
            samples++;
        }
        return false;
    }
    if (samples == 0)
        return false;   // (a flat draw before any 3D, a fade or a flash: the pass waits for the HUD after the 3D)
    passedThisFrame = true;
    return true;
}

// Once a frame (ffuLookFrame, at the flip, before the frame is shown): a frame with 3D and no flat draw is lit here;
// outside battles the pass's program and target are made, so nothing is made as a battle starts (Rules 59, 89).
void ffuFF8LookFrame()
{
    if (ff8 && getmode_cached()->driver_mode != MODE_BATTLE)
        newRenderer.ffuFF8Prepare();
    if (ff8 && !passedThisFrame && samples > 0 && getmode_cached()->driver_mode == MODE_BATTLE && (ffuLookLighting() || ffuLookAtmosphere()) && ffuLookShown())
        newRenderer.ffuFF8Light();
    if (recording)
    {
        // (the five commonest heights: a flat floor is many vertices at one height)
        char line[400];
        int used = _snprintf(line, sizeof(line), "FFU FF8 depth diagnostic: heights (50-unit bands, y down)");
        for (int k = 0; k < 5 && used > 0 && used < int(sizeof(line)) - 40; k++)
        {
            int best = 0;
            for (int b = 1; b < BANDS; b++) if (heights[b] > heights[best]) best = b;
            if (heights[best] == 0) break;
            used += _snprintf(line + used, sizeof(line) - used, " %d..%d: %u;", (best - BANDS / 2) * 50, (best - BANDS / 2 + 1) * 50, heights[best]);
            heights[best] = 0;
        }
        ffnx_info("%s camera translation %d %d %d\n", line, ((const int32_t *)(BATTLE_VIEW_MATRIX + 0x14))[0],
                  ((const int32_t *)(BATTLE_VIEW_MATRIX + 0x14))[1], ((const int32_t *)(BATTLE_VIEW_MATRIX + 0x14))[2]);
    }
    recording = ff8 && getmode_cached()->driver_mode == MODE_BATTLE && passes % 600 == 300 && passedThisFrame;
    drawsThisFrame = 0;
    memset(heights, 0, sizeof(heights));
    passedThisFrame = false;
    sumX = 0.0;
    samples = 0;
    depths = Depths();
}

bool Renderer::ffuFF8Ready()
{
    if (programState != 0) return programState > 0;
    programState = -1;
    const char *vertex = "shaders/FFNx.ffuff8look.flat.vk.vert", *fragment = "shaders/FFNx.ffuff8look.flat.vk.frag";
    char vertexPath[MAX_PATH], fragmentPath[MAX_PATH];
    _snprintf(vertexPath, sizeof(vertexPath), "%s/%s", basedir, vertex);
    _snprintf(fragmentPath, sizeof(fragmentPath), "%s/%s", basedir, fragment);
    if (bgfx::getCaps()->rendererType != bgfx::RendererType::Vulkan || !fileExists(vertexPath) || !fileExists(fragmentPath))
    {
        ffnx_error("FFU FF8 lighting: unavailable (needs Vulkan and %s)\n", vertex);
        return false;
    }
    program = bgfx::createProgram(getShader(vertex), getShader(fragment), true);
    uniformLens = bgfx::createUniform("u_ff8Lens", bgfx::UniformType::Vec4);
    uniformCam = bgfx::createUniform("u_ff8Cam", bgfx::UniformType::Vec4);
    uniformFrame = bgfx::createUniform("u_ff8Frame", bgfx::UniformType::Vec4);
    uniformDepth = bgfx::createUniform("u_ff8Depth", bgfx::UniformType::Vec4);
    uniformLight = bgfx::createUniform("u_ff8Light", bgfx::UniformType::Vec4);
    uniformLook = bgfx::createUniform("u_ff8Look", bgfx::UniformType::Vec4);
    uniformHaze = bgfx::createUniform("u_ff8Haze", bgfx::UniformType::Vec4);
    uniformStage = bgfx::createUniform("u_ff8Stage", bgfx::UniformType::Vec4);
    samplerScene = bgfx::createUniform("s_ff8Scene", bgfx::UniformType::Sampler);
    samplerDepth = bgfx::createUniform("s_ff8Depth", bgfx::UniformType::Sampler);
    quadLayout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float).end();
    programState = bgfx::isValid(program) ? 1 : -1;
    ffnx_info("FFU FF8 lighting: %s\n", programState > 0 ? "ready" : "its program failed");
    return programState > 0;
}

// The pass's program, and its target at the frame's size (outside battles: ffuFF8LookFrame; Rules 59, 89).
void Renderer::ffuFF8Prepare()
{
    if (internalState.bIsHDR || enable_antialiasing > 0 || !ffuFF8Ready())
        return;
    const uint16_t width = framebufferWidth, height = framebufferHeight;
    if (bgfx::isValid(litTarget) && litWidth == width && litHeight == height)
        return;
    if (bgfx::isValid(litTarget))
    {
        if (litView) bgfx::setViewFrameBuffer(litView, BGFX_INVALID_HANDLE);
        bgfx::destroy(litTarget);
    }
    litTarget = bgfx::createFrameBuffer(width, height, bgfx::TextureFormat::RGBA16,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    litWidth = width;
    litHeight = height;
    ffnx_info("FFU FF8 lighting: target made (%ux%u)\n", width, height);
}

// Toward the main light in FF8's view space, from FF8's battle camera this frame (false: its matrix read as nothing,
// the view's own light instead); `up` is the battle world's up in view space (for the log: a camera looking down at the
// stage sees it up the screen and toward itself, y and z both negative).
static bool towardLight(float toLight[3], float up[3])
{
    const int16_t *matrix = (const int16_t *)BATTLE_VIEW_MATRIX;
    const float world[3] = { LIGHT_X, LIGHT_Y, LIGHT_Z };
    float length = 0.0f;
    for (int row = 0; row < 3; row++)
    {
        toLight[row] = 0.0f;
        for (int column = 0; column < 3; column++)
            toLight[row] += matrix[row * 3 + column] / 4096.0f * world[column];
        up[row] = -matrix[row * 3 + 1] / 4096.0f;
        length += toLight[row] * toLight[row];
    }
    const float worldLength = sqrtf(world[0] * world[0] + world[1] * world[1] + world[2] * world[2]);
    length = sqrtf(length) / worldLength;   // (1 for a rotation)
    const bool fromWorld = length > 0.5f && length < 2.0f;
    if (!fromWorld)
    {
        toLight[0] = VIEW_LIGHT_X;
        toLight[1] = VIEW_LIGHT_Y;
        toLight[2] = VIEW_LIGHT_Z;
    }
    const float size = sqrtf(toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2]);
    for (int i = 0; i < 3; i++) toLight[i] /= size;
    return fromWorld;
}

// The pass (gl_draw_indexed_primitive, just before FF8's first flat draw of a battle frame, or the flip).
void Renderer::ffuFF8Light()
{
    if (internalState.bIsHDR || enable_antialiasing > 0 || ffuLayerActive || ffuBattleLayerFrame || backendViewId > 240
        || !ffuFF8Ready())
        return;
    if (bgfx::getAvailTransientVertexBuffer(6, quadLayout) < 6)
        return;
    const float scaleX = *(const float *)SSIGPU_SCALE_X * 8.0f, scaleY = *(const float *)SSIGPU_SCALE_Y * 8.0f;
    const float lensH = float(*(const uint32_t *)GTE_H & 0xFFFF);
    if (lensH <= 0.0f || !(scaleX > 0.0f) || !(scaleY > 0.0f))
        return;
    ffuFF8Prepare();   // (made outside battles already; again only if the frame's size changed)
    const uint16_t width = framebufferWidth, height = framebufferHeight;
    if (!bgfx::isValid(litTarget))
        return;

    float stage[4];
    stageDepths(stage);

    // 1: the picture so far drawn lit into the pass's own target (FFNx's colour and FF8's depth read). Each of the
    // pass's views draws in order, as FFNx's own view 1 (bgfx's default mode sorts: the HUD after it would be sorted)
    ++backendViewId;
    litView = backendViewId;
    bgfx::setViewMode(backendViewId, bgfx::ViewMode::Sequential);
    bgfx::setViewFrameBuffer(backendViewId, litTarget);
    bgfx::setViewRect(backendViewId, 0, 0, width, height);
    bgfx::setViewClear(backendViewId, BGFX_CLEAR_NONE);
    bgfx::setViewTransform(backendViewId, nullptr, nullptr);
    const float lens[4] = { scaleX, scaleY, *(const float *)SSIGPU_OFFSET_X, *(const float *)SSIGPU_OFFSET_Y };
    const float left = widescreen_enabled ? float(*ff8_externals.ssigpu_viewport_x_dword_1CA89D8) : 0.0f;
    const float gameWidth = widescreen_enabled ? float(*ff8_externals.ssigpu_viewport_width_dword_B7CBF8) : float(game_width);
    const float cam[4] = { lensH, float(*(const int16_t *)GTE_OFX), float(*(const int16_t *)GTE_OFY), left };
    const float frame[4] = { gameWidth, float(game_height), 1.0f / width, 1.0f / height };
    const float depthFit[4] = { OWN_SLOPE, DEPTH_BIAS, ffuGlowWanted() ? 1.0f : 0.0f, ffuLookLighting() ? 1.0f : 0.0f };   // (w: the lighting itself; the pass also runs for the haze alone)
    float toLight[3], up[3];
    const bool fromWorld = towardLight(toLight, up);
    // Richard's sliders scale the light, the light from all around and the contact shading; the exposure stays, so a
    // stronger light is a brighter battle
    const float light[4] = { toLight[0], toLight[1], toLight[2], KEY * ffuLookLevel(0) };
    const float look[4] = { AMBIENT * ffuLookLevel(1), 1.0f / (1.0f + WRAP), 1.0f / (AMBIENT + 0.5f * KEY), std::min(CONTACT * ffuLookLevel(2), 1.0f) };
    const float standsAt = samples > 0 ? float(sumX / double(samples)) : 10000.0f;   // (the depth FF8's 3D stands at, on average)
    const float hazeMost = ffuLookAtmosphere() ? std::min(HAZE_MOST * ffuLookLevel(4), 0.9f) : 0.0f;
    const float haze[4] = { HAZE_START * standsAt, ffuLookSplit(), hazeMost, 0.0f };   // (y: the test split, FFU_LOOK_SPLIT)
    bgfx::setUniform(uniformLens, lens);
    bgfx::setUniform(uniformCam, cam);
    bgfx::setUniform(uniformFrame, frame);
    bgfx::setUniform(uniformDepth, depthFit);
    bgfx::setUniform(uniformLight, light);
    bgfx::setUniform(uniformLook, look);
    bgfx::setUniform(uniformHaze, haze);
    bgfx::setUniform(uniformStage, stage);
    bgfx::setTexture(0, samplerScene, backendFrameBufferRT[0], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_POINT);
    bgfx::setTexture(1, samplerDepth, backendFrameBufferRT[1], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_POINT);
    bgfx::TransientVertexBuffer vertices;
    bgfx::allocTransientVertexBuffer(&vertices, 6, quadLayout);
    const QuadVertex corners[6] = {
        { -1.0f, 1.0f, 0.5f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.5f, 1.0f, 0.0f }, { 1.0f, -1.0f, 0.5f, 1.0f, 1.0f },
        { -1.0f, 1.0f, 0.5f, 0.0f, 0.0f }, { 1.0f, -1.0f, 0.5f, 1.0f, 1.0f }, { -1.0f, -1.0f, 0.5f, 0.0f, 1.0f },
    };
    memcpy(vertices.data, corners, sizeof(corners));
    bgfx::setVertexBuffer(0, &vertices);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(backendViewId, program);

    // 2: copied back over FFNx's colour
    ++backendViewId;
    bgfx::setViewMode(backendViewId, bgfx::ViewMode::Sequential);
    bgfx::setViewClear(backendViewId, BGFX_CLEAR_NONE);
    bgfx::blit(backendViewId, {.handle = backendFrameBufferRT[0]}, {.handle = bgfx::getTexture(litTarget, 0), .x = 0, .y = 0, .width = width, .height = height});
    bgfx::touch(backendViewId);

    // 3: FFNx carries on (the HUD) in a view of its own, after these (its draws set their frame buffer; nothing here
    // marks the frame as drawn or not)
    ++backendViewId;
    bgfx::setViewMode(backendViewId, bgfx::ViewMode::Sequential);
    bgfx::setViewClear(backendViewId, BGFX_CLEAR_NONE);
    bgfx::touch(backendViewId);
    if (passes++ % 600 == 0)
        ffnx_info("FFU FF8 lighting: pass %u (depth: %u vertices its own way, z %.4f..%.4f; %u the stage's%s, z %.4f..%.4f, %.4g x + %.4g, "
                  "read from %.4f %s; %u neither; lens H %.0f, scale %.3f x %.3f; view %u; light %s %.2f %.2f %.2f, the world's up in view "
                  "%.2f %.2f %.2f)\n", passes, depths.own, depths.ownMin, depths.ownMax, depths.stage, depths.mixed ? " (mixed)" : "",
                  depths.stageMin, depths.stageMax, stage[0], stage[1], stage[2], stage[3] > 0.0f ? "up" : stage[3] < 0.0f ? "down" : "(not told apart)",
                  depths.neither, lensH, scaleX, scaleY, backendViewId, fromWorld ? "from FF8's camera" : "fixed to the view",
                  toLight[0], toLight[1], toLight[2], up[0], up[1], up[2]);
}
