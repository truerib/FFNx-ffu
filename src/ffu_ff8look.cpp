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
// FF8's battle depth comes in two lines (measured 2026-10-22: depthLines): its stage at 3.05e-6 a unit of view depth
// plus one offset that keeps it behind everything (as the PlayStation's ordering table did), its characters and what
// else it draws with depth on a line of their own; each frame both are found from FF8's own vertices (z against
// 1 / rhw), so the floor is lit, shadowed and hazed at its true depth. The main light stands in FF8's battle world,
// turned into the view by FF8's battle camera matrix each frame.
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
bool ffuLookSpell();
bool ffuLookRim();
bool ffuLookSoftShadows();
float ffuLookDebug();
bgfx::TextureHandle ffuGlowLight();   // ffu_glow.cpp
float ffuLookLevel(int which);
float ffuLookSplit();

namespace
{
    // FF8's lens (as the fork's world renderer reads it: src/ff8/world/renderer.cpp)
    constexpr uint32_t GTE_H = 0x1CA92E4, GTE_OFX = 0x1CA92DE, GTE_OFY = 0x1CA92E2;
    constexpr uint32_t SSIGPU_SCALE_X = 0xB7CC14, SSIGPU_SCALE_Y = 0xB7CC18, SSIGPU_OFFSET_X = 0x1CA89E0, SSIGPU_OFFSET_Y = 0x1CA89E4;
    // FF8's battle depth (Richard 2026-10-22, "I can't see any other changes": read on one line, the beach's sand and sea
    // stood as far as its sky, and the pass left them as FF8 drew them). FFNx 1.24.3.81's diagnostic on the beach: the
    // stage (floor, sea, the mound before the camera) z = 3.05176e-6 Z + 0.3768, that slope the one FF8 keeps at
    // 0xB7CEE0, one offset for all of it; the characters z = 8.754e-6 Z + 0.0029 (within each triangle 3.05e-6: FF8 steps
    // them by its ordering table's slot, too finely to matter); z 0.04-0.18 against 0.38 and up.
    constexpr uint32_t STAGE_SLOPE = 0xB7CEE0;
    constexpr float MEASURED_SLOPE = 3.0517578e-06f, CHARACTERS_SLOPE = 8.754e-06f, CHARACTERS_OFFSET = 0.0029f;
    constexpr uint32_t MOST_SAMPLES = 8192;
    // The stage's offset is looked for among offsets (z less the stage's slope's share) from 0.15 in bands 0.003 wide: the
    // fullest band holding 16 samples or more across 2,000 units of view depth or more (a floor: far characters, or a
    // few far pieces at an offset of their own, span far less; review). A stage found stays for 120 frames without one.
    constexpr int BANDS = 240;
    constexpr float BANDS_FROM = 0.15f, BAND = 0.003f, FLOOR_SPAN = 2000.0f;
    constexpr uint32_t BAND_LEAST = 16, KEPT_FRAMES = 120;
    constexpr float PUSHED = 0.15f;   // (the characters' offsets reach 0.15 at about 27,000 units; the stage's pieces sit above 0.2: its far mountains near 0.25)
    // The look's balance (as FF9's: mods/ff9-globe BattleScene.cs): ambient, main light, wrap, contact. The exposure
    // keeps the floor in the light as FF8 drew it (a wrapped diffuse of FLOOR_DIFFUSE: the floor faces the battle world's
    // up, the main light 49 degrees above it); in shadow, or turned away, about 35% darker; turned to the light about 6%
    // brighter (with the floor lit too, the earlier 24% burnt the beach's sand white)
    constexpr float AMBIENT = 0.5f, KEY = 0.955f, WRAP = 0.35f, CONTACT = 0.85f;
    constexpr float FLOOR_DIFFUSE = 0.7557f / (1.0f + WRAP) + WRAP / (1.0f + WRAP);
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
    bgfx::UniformHandle uniformHaze = BGFX_INVALID_HANDLE, uniformStage = BGFX_INVALID_HANDLE, uniformSpell = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle samplerLight = BGFX_INVALID_HANDLE, uniformRim = BGFX_INVALID_HANDLE, uniformFloor = BGFX_INVALID_HANDLE;
    // The spells' light (Richard's next list, item 1; as FF7's: ffu_ff7look.cpp): the effects' light from the glow's
    // mask (the last frame's, blurred about a tenth of the picture wide by ffu_glow.cpp), multiplied in
    constexpr float SPELL = 6.0f, SPELL_DISC = 0.02f;   // (its smoothing taps' offset)
    // The rim light (Richard's next list, item 2; as FF7's: ffu_ff7look.cpp): its strength and its reach (across the picture)
    constexpr float RIM = 1.0f, RIM_REACH = 0.006f;   // (0.004, and half the light, hardly showed)
    bgfx::UniformHandle samplerScene = BGFX_INVALID_HANDLE, samplerDepth = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout quadLayout;
    bgfx::FrameBufferHandle litTarget = BGFX_INVALID_HANDLE;
    uint16_t litWidth = 0, litHeight = 0;
    bgfx::ViewId litView = 0;   // the view that last drew into it (given no frame buffer before it is remade)
    bool passedThisFrame = false;
    uint32_t passes = 0;
    // This frame's depth-writing 3D draws: every fourth vertex's view depth (1 / rhw) and depth
    struct Sample { float view, z; };
    Sample sampled[MOST_SAMPLES];
    uint32_t samples = 0;
    uint32_t sampleSeed = 1;   // (once full, each sample takes a place at random: every draw keeps its share)
    // FF8's two lines of depth this frame (depthLines)
    struct DepthLines
    {
        float slope = CHARACTERS_SLOPE, offset = CHARACTERS_OFFSET;   // (its characters'; as measured until fitted)
        float stageSlope = MEASURED_SLOPE, stageOffset = 0.0f, stageFrom = 2.0f;   // (its stage's, from that depth up; none: 2)
        uint32_t stage = 0, rest = 0, pushed = 0;
        float standsAt = 10000.0f, pushedLowest = 2.0f, restMost = -1.0f;
        bool kept = false;   // (this frame's stage is an earlier frame's)
    };
    DepthLines lastLines;
    uint32_t lastStageAge = KEPT_FRAMES;

    struct QuadVertex { float x, y, z, u, v; };
}

// FF8's two lines of depth this frame, from its own vertices: the stage is the fullest band of offsets (z less the stage's
// slope, FF8's at STAGE_SLOPE, times the view depth) that spans a floor's depths; the rest (characters, effects drawn with
// depth), below it, fitted on a line of their own. A frame without either keeps the last (120 frames) or the measured.
static DepthLines depthLines()
{
    DepthLines lines;
    if (samples == 0)
        return lines;
    float slope = *(const float *)STAGE_SLOPE;
    if (!(slope > 1e-6f && slope < 1e-5f)) slope = MEASURED_SLOPE;
    lines.stageSlope = slope;
    uint32_t count[BANDS] = {};
    float nearest[BANDS], farthest[BANDS];
    for (int band = 0; band < BANDS; band++) { nearest[band] = 1e30f; farthest[band] = -1.0f; }
    double viewSum = 0.0;
    for (uint32_t i = 0; i < samples; i++)
    {
        viewSum += sampled[i].view;
        const int band = int(floorf((sampled[i].z - slope * sampled[i].view - BANDS_FROM) / BAND));
        if (band < 0 || band >= BANDS) continue;
        count[band]++;
        nearest[band] = std::min(nearest[band], sampled[i].view);
        farthest[band] = std::max(farthest[band], sampled[i].view);
    }
    lines.standsAt = float(viewSum / samples);
    int best = -1;
    for (int band = 0; band < BANDS; band++)
        if (count[band] >= BAND_LEAST && farthest[band] - nearest[band] >= FLOOR_SPAN && (best < 0 || count[band] > count[best]))
            best = band;
    double offsetSum = 0.0;
    if (best >= 0)
    {
        const float centre = BANDS_FROM + (float(best) + 0.5f) * BAND;
        for (uint32_t i = 0; i < samples; i++)
        {
            const float c = sampled[i].z - slope * sampled[i].view;
            if (fabsf(c - centre) > 1.5f * BAND) continue;
            offsetSum += c;
            lines.stage++;
        }
        lines.stageOffset = float(offsetSum / std::max<uint32_t>(lines.stage, 1));
    }
    // What FF8 pushes back (its stage, in pieces at offsets of their own: the floor under the party is not the one before
    // the camera, FFU_LOOK_DEBUG=1, 2026-10-22) is everything whose offset passes PUSHED; the shader reads all of it off
    // the floor plane (or as the backdrop where its ray never meets the floor), the rest (characters, effects drawn
    // with depth) on their own fitted line
    float pushedLowest = 2.0f, restMost = -1.0f;
    uint32_t pushed = 0;
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (uint32_t i = 0; i < samples; i++)
    {
        const float x = sampled[i].view, y = sampled[i].z;
        if (y - slope * x > PUSHED)
        {
            pushed++;
            pushedLowest = std::min(pushedLowest, y);
            continue;
        }
        sx += x; sy += y; sxx += double(x) * x; sxy += double(x) * y;
        lines.rest++;
        restMost = std::max(restMost, y);
    }
    const double n = double(lines.rest), det = n * sxx - sx * sx;
    bool fitted = false;
    if (lines.rest >= 16 && det > 1e-3 * n * n)
    {
        const float fitSlope = float((n * sxy - sx * sy) / det), fitOffset = float((sy - fitSlope * sx) / n);
        if (fitSlope > 2e-6f && fitSlope < 5e-5f) { lines.slope = fitSlope; lines.offset = fitOffset; fitted = true; }
    }
    if (fitted)
    {
        // (once again without what lies off that line by more than 0.01: far pieces of the stage FF8 pushes back less)
        double rx = 0.0, ry = 0.0, rxx = 0.0, rxy = 0.0;
        uint32_t kept = 0;
        for (uint32_t i = 0; i < samples; i++)
        {
            const float x = sampled[i].view, y = sampled[i].z;
            if (y - slope * x > PUSHED || fabsf(y - (lines.slope * x + lines.offset)) > 0.01f) continue;
            rx += x; ry += y; rxx += double(x) * x; rxy += double(x) * y;
            kept++;
        }
        const double m = double(kept), mdet = m * rxx - rx * rx;
        if (kept >= 16 && mdet > 1e-3 * m * m)
        {
            const float refit = float((m * rxy - rx * ry) / mdet), refitOffset = float((ry - refit * rx) / m);
            if (refit > 2e-6f && refit < 5e-5f) { lines.slope = refit; lines.offset = refitOffset; lines.rest = kept; }
        }
    }
    lines.pushed = pushed;
    lines.pushedLowest = pushedLowest;
    lines.restMost = restMost;
    const bool found = pushed >= BAND_LEAST && pushedLowest > restMost + 0.002f;
    if (found)
    {
        lines.stageFrom = pushedLowest - 0.0005f;   // (a pushed-back depth is its lowest sample's or more)
        if (best < 0) lines.stageOffset = lastLines.stageOffset;
        if (!fitted) { lines.slope = lastLines.slope; lines.offset = lastLines.offset; }
        lastLines = lines;
        lastStageAge = 0;
    }
    else if (lastStageAge < KEPT_FRAMES)
    {
        lastStageAge++;
        lines.stageOffset = lastLines.stageOffset;
        lines.stageFrom = lastLines.stageFrom;
        lines.kept = true;
        if (!fitted) { lines.slope = lastLines.slope; lines.offset = lastLines.offset; }
    }
    return lines;
}

// From gl_draw_indexed_primitive, for every draw of an FF8 battle frame before it is drawn: the 3D vertices of the draws
// that write depth are sampled (review: an effect drawn without depth never reaches the depth the pass reads); true at
// the frame's first flat draw after some 3D, when the lighting pass goes first.
bool ffuFF8LookDraw(uint32_t vertextype, const struct nvertex *vertices, uint32_t vertexcount, uint32_t depthWritten)
{
    if (!ff8 || passedThisFrame || vertextype != TLVERTEX || getmode_cached()->driver_mode != MODE_BATTLE || !(ffuLookLighting() || ffuLookAtmosphere() || ffuLookSpell() || ffuLookRim())
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
        for (uint32_t i = 0; depthWritten && i < vertexcount; i += 4)
        {
            const float rhw = vertices[i].color.w, z = vertices[i]._.z;
            if (!(rhw > 0.0f && rhw < 0.5f) || !(z > 0.0f && z < 1.0f)) continue;   // (FFNx maps an infinite rhw to 1)
            if (samples < MOST_SAMPLES)
                sampled[samples++] = { 1.0f / rhw, z };
            else
            {
                sampleSeed = sampleSeed * 1664525u + 1013904223u;
                sampled[(sampleSeed >> 8) % MOST_SAMPLES] = { 1.0f / rhw, z };
            }
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
    {
        newRenderer.ffuFF8Prepare();
        lastStageAge = KEPT_FRAMES;   // (the next battle's stage is its own)
        lastLines = DepthLines();
    }
    if (ff8 && !passedThisFrame && samples > 0 && getmode_cached()->driver_mode == MODE_BATTLE && (ffuLookLighting() || ffuLookAtmosphere() || ffuLookSpell() || ffuLookRim()) && ffuLookShown())
        newRenderer.ffuFF8Light();
    passedThisFrame = false;
    samples = 0;
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
    uniformSpell = bgfx::createUniform("u_ff8Spell", bgfx::UniformType::Vec4);
    uniformRim = bgfx::createUniform("u_ff8Rim", bgfx::UniformType::Vec4);
    uniformFloor = bgfx::createUniform("u_ff8Floor", bgfx::UniformType::Vec4);
    samplerLight = bgfx::createUniform("s_ff8Light", bgfx::UniformType::Sampler);
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
    // (FF8's haze is held back: FF8 sets its far scenery's pieces at depth offsets of their own, so one piece of a mountain
    // range hazed and the next didn't, a box (2026-10-22, build 85); ffuLookAtmosphere() and HAZE_MOST come back with a
    // haze that reads those pieces alike)
    const float hazeMost = 0.0f;
    const bgfx::TextureHandle effectsLight = ffuGlowLight();
    const float spell = ffuLookSpell() && bgfx::isValid(effectsLight) ? SPELL * ffuLookLevel(5) : 0.0f;
    const float rim = ffuLookRim() ? RIM : 0.0f;   // (the soft shadows ride on the lighting: u_ff8Rim.w)
    if (!ffuLookLighting() && hazeMost <= 0.0f && spell <= 0.0f && rim <= 0.0f && ffuLookDebug() <= 0.0f)
        return;   // (the haze alone at nothing: nothing to draw)
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

    const DepthLines lines = depthLines();
    const float stage[4] = { lines.stageSlope, lines.stageOffset, lines.stageFrom, lines.stageFrom < 1.5f ? 1.0f : 0.0f };   // (x also the slope within each triangle)

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
    const float depthFit[4] = { lines.slope, lines.offset, ffuGlowWanted() ? 1.0f : 0.0f, ffuLookLighting() ? 1.0f : 0.0f };   // (w: the lighting itself; the pass also runs for the haze alone)
    float toLight[3], up[3];
    const bool fromWorld = towardLight(toLight, up);
    // Richard's sliders scale the light, the light from all around and the contact shading; the exposure stays, so a
    // stronger light is a brighter battle
    const float light[4] = { toLight[0], toLight[1], toLight[2], KEY * ffuLookLevel(0) };
    const float look[4] = { AMBIENT * ffuLookLevel(1), 1.0f / (1.0f + WRAP), 1.0f / (AMBIENT + KEY * FLOOR_DIFFUSE), std::min(CONTACT * ffuLookLevel(2), 1.0f) };
    const float haze[4] = { HAZE_START * lines.standsAt, ffuLookSplit(), hazeMost, ffuLookDebug() };   // (w: the test view, FFU_LOOK_DEBUG)   // (y: the test split, FFU_LOOK_SPLIT)
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
    const float spellData[4] = { spell, SPELL_DISC, SPELL_DISC * float(width) / float(std::max<uint16_t>(height, 1)), 0.0f };
    bgfx::setUniform(uniformSpell, spellData);
    const float rimData[4] = { rim, RIM_REACH, RIM_REACH * float(width) / float(std::max<uint16_t>(height, 1)), ffuLookSoftShadows() ? 1.0f : 0.0f };
    bgfx::setUniform(uniformRim, rimData);
    // The floor (FF8's battle world's height 0, its battles' floor: the diagnostic's vertices) in the view: for a view ray
    // d, view depth = floor.w / dot(floor.xyz, d), floor.xyz the world's down as the view sees it and w its dot with the
    // camera's translation (view = matrix x world + translation, LookAt 0x50CB40); none when the matrix reads as nothing
    float floorData[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (fromWorld)
    {
        const int16_t *matrix = (const int16_t *)BATTLE_VIEW_MATRIX;
        const int32_t *translation = (const int32_t *)(BATTLE_VIEW_MATRIX + 0x14);
        for (int row = 0; row < 3; row++) floorData[row] = matrix[row * 3 + 1] / 4096.0f;
        floorData[3] = floorData[0] * float(translation[0]) + floorData[1] * float(translation[1]) + floorData[2] * float(translation[2]);
        if (!(floorData[3] > 1.0f)) floorData[0] = floorData[1] = floorData[2] = floorData[3] = 0.0f;   // (the camera under the floor: none)
    }
    bgfx::setUniform(uniformFloor, floorData);
    // (no light: the scene itself bound, never read, as bgfx wants every sampler the program declares)
    bgfx::setTexture(2, samplerLight, spell > 0.0f ? effectsLight : backendFrameBufferRT[0], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
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
        ffnx_info("FFU FF8 lighting: pass %u (depth: pushed back %u samples from z %.4f, the rest to z %.4f; floor %.3f %.3f %.3f %.0f; "
                  "the stage %s%.4g x + %.4f from %u samples, from z %.4f; the rest %.4g x + %.4f from %u; "
                  "lens H %.0f, scale %.3f x %.3f; view %u; light %s %.2f %.2f %.2f, the world's up in view %.2f %.2f %.2f)\n", passes,
                  lines.pushed, lines.pushedLowest, lines.restMost, floorData[0], floorData[1], floorData[2], floorData[3],
                  stage[3] < 0.5f ? "(not told apart) " : lines.kept ? "(kept) " : "", lines.stageSlope, lines.stageOffset, lines.stage,
                  lines.stageFrom, lines.slope, lines.offset, lines.rest, lensH, scaleX, scaleY, backendViewId, fromWorld ? "from FF8's camera" : "fixed to the view",
                  toLight[0], toLight[1], toLight[2], up[0], up[1], up[2]);
}
