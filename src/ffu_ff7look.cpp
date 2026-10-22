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

// Final Fantasy Universe: FF7's battle haze and cinematic bars (docs/battle-look.md "FF7 haze", "Cinematic bars"), the
// battle look's atmosphere and item 5 for FF7, whose battles FFNx already lights itself (ffu_look.cpp).
//
// The survey (ffu_survey.cpp, FFU_LOOK_SURVEY=1) found that in FF7's battle frames its 3D (untransformed draws) comes
// first, about 140 draws, and its flat 2D (the menus: every rhw 1) after, with a handful of 3D draws (cursors, effects)
// among them; and FFNx clears FF7's depth just before the menus (its battle_depth_clear). So just there (or at the
// frame's end, in a frame without menus) this pass draws the picture so far into a target of its own and copies it back,
// as FF8's does (ffu_ff8look.cpp):
// - the haze: outdoors (FF7's backdrop, its far scenery, seen at the picture's top), what stands beyond its stage fades
//   toward the backdrop's own colour; the view depth comes from FF7's own depth and the projection FFNx drew its 3D with
//   (clip = view x projection x viewport, FFNx.vert); the stage's size from FF7's battle camera (its distance to what it
//   looks at);
// - the bars: while FF7 plays a summon (command 0x03, W-Summon 0x16) or a Limit (0x14) and one of its effects runs,
//   black bars ease in above and below to a 2.2:1 frame; FF7's menus are drawn after, on top.
// Off with the look's atmosphere and cinema bits off, in HDR, with MSAA, and in the live layer.

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

bool ffuLookAtmosphere();   // ffu_look.cpp
bool ffuLookCinema();
bool ffuLookLighting();
bool ffuLookSpell();
bool ffuLookRim();
bool ffuLookShaking();
void ffuLookShake(float shake[4]);
bool ffuLookDof();
bool ffuLookMotion();
bgfx::TextureHandle ffuGlowLight();   // ffu_glow.cpp
bool ffuLookShown();
float ffuLookLevel(int which);
float ffuLookSplit();

namespace
{
    constexpr float HAZE_MOST = 0.5f, HAZE_START = 1.5f, CINEMA_ASPECT = 2.2f;   // (0.35 was too faint to see: Richard 2026-10-22)
    // Contact shading with the lighting on (Richard 2026-10-22, "I can't see any other changes": FFNx's own lighting is
    // already lit 4 to 1, so the look's balance alone hardly showed): its strength (the contact slider scales it) and
    // its ring's radius across the picture
    constexpr float CONTACT = 0.7f, CONTACT_RING = 0.012f;
    // The spells' light (Richard's next list, item 1): the light FF7's effects added (the glow's mask, blurred about a
    // tenth of the picture wide at a sixteenth of it by ffu_glow.cpp, the last frame's), multiplied into what stands
    // round it: its strength at the slider's 1
    constexpr float SPELL = 6.0f, SPELL_DISC = 0.02f;   // (its smoothing taps' offset)
    // The rim light (Richard's next list, item 2): a light edge on what stands well in front of what is behind it (a
    // character against the far ground or sky), its strength and how far in from the edge it reaches (across the picture)
    constexpr float RIM = 1.0f, RIM_REACH = 0.006f;   // (0.004, and half the light, hardly showed)
    // The cinematics (Richard's next list, item 3): while a summon or Limit plays, depth of field (the blur at infinity and
    // its most, shares of the picture's width) and motion blur (its share of a frame's move, and its most); a frame whose
    // camera jumped further than CUT (clip units at what it looks at) is a cut, never blurred
    constexpr float DOF_APERTURE = 0.007f, DOF_MOST = 0.01f, MOTION_SHARE = 1.0f, MOTION_MOST = 0.04f, CUT = 0.3f;
    constexpr byte SUMMON = 0x03, LIMIT = 0x14, W_SUMMON = 0x16;

    int programState = 0;   // 0 not tried, 1 ready, -1 unavailable
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uniformDepth = BGFX_INVALID_HANDLE, uniformHaze = BGFX_INVALID_HANDLE, uniformSplit = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uniformContact = BGFX_INVALID_HANDLE, uniformSpell = BGFX_INVALID_HANDLE, uniformRim = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uniformShake = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle samplerScene = BGFX_INVALID_HANDLE, samplerDepth = BGFX_INVALID_HANDLE, samplerLight = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout quadLayout;
    bgfx::FrameBufferHandle target = BGFX_INVALID_HANDLE;
    uint16_t targetWidth = 0, targetHeight = 0;
    bgfx::ViewId targetViews[4] = {};   // the views that drew into the target last (unbound before it is made again)
    bool seen3D = false, passedThisFrame = false;
    float bars = 0.0f, barsHold = 0.0f;   // the bars' weight 0-1 and the time they are held after a cinematic
    ULONGLONG lastTick = 0;
    uint32_t passes = 0;

    // The cinematics' program (FFNx.ffucine: optional, the look works without it) and its state
    int cineState = 0;   // 0 not tried, 1 ready, -1 unavailable
    bgfx::ProgramHandle cineProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle cineReproject = BGFX_INVALID_HANDLE, cineDepth = BGFX_INVALID_HANDLE, cineFocus = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle cineFrame = BGFX_INVALID_HANDLE, cineShake = BGFX_INVALID_HANDLE, cineMotion = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle cineScene = BGFX_INVALID_HANDLE, cineDepthMap = BGFX_INVALID_HANDLE;
    float cine = 0.0f, cineHold = 0.0f;   // the cinematics' weight 0-1 and the time held after one (as the bars)
    ULONGLONG cineTick = 0;
    // The last battle frame's camera (FF7's view matrix, FFNx's view x projection x viewport) and the frames counted, for
    // the motion; and the last move, kept for a repeated frame (FF7's battles at 30 a second drawn at 60)
    uint32_t battleFrames = 0, lastFrame = 0, lastMove = 0;
    bool lastValid = false, moveValid = false;
    float lastView[16], lastClip[16], lastReproject[16];
    float middleMove[2] = {};   // the last move of what the camera looks at, in clip units (the log's)

    struct QuadVertex { float x, y, z, u, v; };

    bool cinematicCommand(byte command)
    {
        return command == SUMMON || command == W_SUMMON || command == LIMIT;
    }

    bool cinematicPlaying()
    {
        if (!ff7_externals.anim_event_queue.data() || !ff7_externals.effect100_counter) return false;
        const byte command = ff7_externals.anim_event_queue[0].commandIndex;
        const byte last = ff7_externals.battle_context ? ff7_externals.battle_context->lastCommandIdx : 0;
        const uint16_t e100 = *ff7_externals.effect100_counter, e60 = ff7_externals.effect60_counter ? *ff7_externals.effect60_counter : 0,
            e10 = ff7_externals.effect10_counter ? *ff7_externals.effect10_counter : 0;
        const bool effects = e100 > 0 || e60 > 0 || e10 > 0;   // (checked live: Barret's Big Shot is 0x14 with effects 0 5 0, then 1 2 0)
        // Decided as FF7's effects begin, from the command then: the command fields are left as they were between actions,
        // so a later spell's effects never raise the bars on a Limit's old command (Sonnet review)
        static bool hadEffects = false, cinematic = false;
        if (effects && !hadEffects)
            cinematic = cinematicCommand(command) || cinematicCommand(last);
        hadEffects = effects;
        return effects && cinematic;
    }

    // The bars' weight this frame, eased: in over 0.45 s, held 1 s after the cinematic (a Limit's effects pause about 0.9 s
    // between its parts), out over 0.6 s.
    float barsWeight()
    {
        const ULONGLONG nowTick = GetTickCount64();
        const float dt = lastTick ? std::min(float(nowTick - lastTick) / 1000.0f, 0.1f) : 0.0f;
        lastTick = nowTick;
        const bool playing = ffuLookCinema() && cinematicPlaying();
        static bool wasPlaying = false;
        if (playing != wasPlaying && ff7_externals.anim_event_queue.data() && ff7_externals.effect100_counter)
            ffnx_info("FFU FF7 bars: %s (command 0x%02X, effects %u %u %u)\n", playing ? "a summon or Limit plays" : "it ended",
                      ff7_externals.anim_event_queue[0].commandIndex, *ff7_externals.effect100_counter,
                      ff7_externals.effect60_counter ? *ff7_externals.effect60_counter : 0, ff7_externals.effect10_counter ? *ff7_externals.effect10_counter : 0);
        wasPlaying = playing;
        barsHold = playing ? 1.0f : std::max(barsHold - dt, 0.0f);
        const bool wanted = playing || barsHold > 0.0f;
        bars = wanted ? std::min(bars + dt / 0.45f, 1.0f) : std::max(bars - dt / 0.6f, 0.0f);
        return bars * bars * (3.0f - 2.0f * bars);
    }
}

// The cinematics' weight this frame (item 3): eased in over 0.45 s while a summon or Limit plays with depth of field or
// motion blur on, held 1 s after, out over 0.6 s, as the bars.
static float cineWeight()
{
    const ULONGLONG nowTick = GetTickCount64();
    const float dt = cineTick ? std::min(float(nowTick - cineTick) / 1000.0f, 0.1f) : 0.0f;
    cineTick = nowTick;
    const bool playing = (ffuLookDof() || ffuLookMotion()) && cinematicPlaying();
    cineHold = playing ? 1.0f : std::max(cineHold - dt, 0.0f);
    cine = playing || cineHold > 0.0f ? std::min(cine + dt / 0.45f, 1.0f) : std::max(cine - dt / 0.6f, 0.0f);
    return cine * cine * (3.0f - 2.0f * cine);
}

// FF7 plays a summon or a Limit now (the impact's hard shake as one begins: ffu_look.cpp)
bool ffuFF7Cinematic()
{
    return !ff8 && getmode_cached()->driver_mode == MODE_BATTLE && cinematicPlaying();
}

// From gl_draw_indexed_primitive for every draw of an FF7 battle frame (with FFNx's lighting on, as its deferred draws
// are drawn at the flip): notes that FF7's 3D has begun. The pass goes just before FF7 clears its depth for its menus
// (ffuFF7BeforeDepthClear: FFNx's battle_depth_clear, deferred in draw order), else at its first flat draw after the
// 3D, else at the frame's end.
bool ffuFF7LookDraw(uint32_t vertextype, const struct nvertex *vertices, uint32_t vertexcount)
{
    if (ff8 || getmode_cached()->driver_mode != MODE_BATTLE)
        return false;
    if (vertextype != TLVERTEX)
    {
        seen3D = true;
        return false;
    }
    // A frame whose menus never clear the depth (a summon's, a limit's): its first flat draw after the 3D (its banner,
    // its numbers) has the pass go first, so FF7's 2D stays on top (Sonnet review)
    if (passedThisFrame || !seen3D || !ffuLookShown()) return false;
    for (uint32_t i = 0; i < vertexcount; i++)
        if (vertices[i].color.w != 1.0f) return false;
    passedThisFrame = true;
    return true;
}

// ff7::battle::battle_depth_clear, as it clears FF7's depth before the menus: the picture so far is FF7's 3D.
void ffuFF7BeforeDepthClear()
{
    if (ff8 || passedThisFrame || !seen3D || getmode_cached()->driver_mode != MODE_BATTLE || !ffuLookShown())
        return;
    passedThisFrame = true;
    newRenderer.ffuFF7Pass();
}

// At the flip, before FFNx's lighting (ffuLookFrame): outside battles the pass's program and target are made, so nothing
// is made as a battle starts (Rules 59, 89).
void ffuFF7LookFrame()
{
    if (ff8) return;
    if (getmode_cached()->driver_mode != MODE_BATTLE)
    {
        newRenderer.ffuFF7Prepare();
        bars = barsHold = 0.0f;
        cine = cineHold = 0.0f;
        lastValid = moveValid = false;
    }
}

// After the frame's deferred draws (ffuLookAfterDeferred): a battle frame with 3D whose menus never cleared the depth
// (a summon's, a limit's) gets the pass here; then the frame's notes start again.
void ffuFF7LookFrameEnd()
{
    if (ff8) return;
    if (!passedThisFrame && seen3D && getmode_cached()->driver_mode == MODE_BATTLE && ffuLookShown())
        newRenderer.ffuFF7Pass();
    if (getmode_cached()->driver_mode == MODE_BATTLE) battleFrames++;
    seen3D = passedThisFrame = false;
}

bool Renderer::ffuFF7Ready()
{
    if (programState != 0) return programState > 0;
    programState = -1;
    const char *vertex = "shaders/FFNx.ffuff7look.flat.vk.vert", *fragment = "shaders/FFNx.ffuff7look.flat.vk.frag";
    char vertexPath[MAX_PATH], fragmentPath[MAX_PATH];
    _snprintf(vertexPath, sizeof(vertexPath), "%s/%s", basedir, vertex);
    _snprintf(fragmentPath, sizeof(fragmentPath), "%s/%s", basedir, fragment);
    if (bgfx::getCaps()->rendererType != bgfx::RendererType::Vulkan || !fileExists(vertexPath) || !fileExists(fragmentPath))
    {
        ffnx_error("FFU FF7 haze: unavailable (needs Vulkan and %s)\n", vertex);
        return false;
    }
    program = bgfx::createProgram(getShader(vertex), getShader(fragment), true);
    uniformDepth = bgfx::createUniform("u_ff7Depth", bgfx::UniformType::Vec4);
    uniformHaze = bgfx::createUniform("u_ff7Haze", bgfx::UniformType::Vec4);
    uniformSplit = bgfx::createUniform("u_ff7Split", bgfx::UniformType::Vec4);
    uniformContact = bgfx::createUniform("u_ff7Contact", bgfx::UniformType::Vec4);
    uniformSpell = bgfx::createUniform("u_ff7Spell", bgfx::UniformType::Vec4);
    uniformRim = bgfx::createUniform("u_ff7Rim", bgfx::UniformType::Vec4);
    uniformShake = bgfx::createUniform("u_ff7Shake", bgfx::UniformType::Vec4);
    samplerLight = bgfx::createUniform("s_ff7Light", bgfx::UniformType::Sampler);
    samplerScene = bgfx::createUniform("s_ff7Scene", bgfx::UniformType::Sampler);
    samplerDepth = bgfx::createUniform("s_ff7Depth", bgfx::UniformType::Sampler);
    quadLayout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float).end();
    programState = bgfx::isValid(program) ? 1 : -1;
    ffnx_info("FFU FF7 haze: %s\n", programState > 0 ? "ready" : "its program failed");
    // The cinematics' program (its own shaders: an older install without them keeps the rest of the look)
    const char *cineVertex = "shaders/FFNx.ffucine.flat.vk.vert", *cineFragment = "shaders/FFNx.ffucine.flat.vk.frag";
    _snprintf(vertexPath, sizeof(vertexPath), "%s/%s", basedir, cineVertex);
    _snprintf(fragmentPath, sizeof(fragmentPath), "%s/%s", basedir, cineFragment);
    cineState = -1;
    if (programState > 0 && fileExists(vertexPath) && fileExists(fragmentPath))
    {
        cineProgram = bgfx::createProgram(getShader(cineVertex), getShader(cineFragment), true);
        cineReproject = bgfx::createUniform("u_cineReproject", bgfx::UniformType::Mat4);
        cineDepth = bgfx::createUniform("u_cineDepth", bgfx::UniformType::Vec4);
        cineFocus = bgfx::createUniform("u_cineFocus", bgfx::UniformType::Vec4);
        cineFrame = bgfx::createUniform("u_cineFrame", bgfx::UniformType::Vec4);
        cineShake = bgfx::createUniform("u_cineShake", bgfx::UniformType::Vec4);
        cineMotion = bgfx::createUniform("u_cineMotion", bgfx::UniformType::Vec4);
        cineScene = bgfx::createUniform("s_cineScene", bgfx::UniformType::Sampler);
        cineDepthMap = bgfx::createUniform("s_cineDepth", bgfx::UniformType::Sampler);
        cineState = bgfx::isValid(cineProgram) ? 1 : -1;
    }
    ffnx_info("FFU FF7 cinematics: %s\n", cineState > 0 ? "ready" : "unavailable (needs FFNx.ffucine's shaders)");
    return programState > 0;
}

void Renderer::ffuFF7Prepare()
{
    if (internalState.bIsHDR || enable_antialiasing > 0 || !ffuFF7Ready())
        return;
    if (bgfx::isValid(target) && targetWidth == framebufferWidth && targetHeight == framebufferHeight)
        return;
    if (bgfx::isValid(target))
    {
        for (bgfx::ViewId &view : targetViews)
            if (view) { bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE); view = 0; }
        bgfx::destroy(target);
    }
    target = bgfx::createFrameBuffer(framebufferWidth, framebufferHeight, bgfx::TextureFormat::RGBA16,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    targetWidth = framebufferWidth;
    targetHeight = framebufferHeight;
    ffnx_info("FFU FF7 haze: target made (%ux%u)\n", targetWidth, targetHeight);
}

// The camera's move since the last battle frame, for the cinematics' motion blur: out maps this frame's (x, y, depth, 1)
// on the screen (row vectors, as FFNx.vert) to the last frame's clip, through FF7's view matrix (FFNx's, from FF7's
// camera: world to view) and FFNx's view x projection x viewport (n) of each frame. False on a battle's first frame, after
// a gap, at a cut (what the camera looks at jumped further than CUT), and when the camera didn't move (a repeated frame
// takes the last move once: FF7's battles at 30 a second drawn at 60).
// The view depth of what FF7's battle camera looks at (its focal point through FF7's view matrix), 0 unknown.
static float focalDepth(const float *view)
{
    if (!ff7_externals.g_battle_camera_focal_point) return 0.0f;
    const float point[4] = { float(ff7_externals.g_battle_camera_focal_point->x), float(ff7_externals.g_battle_camera_focal_point->y),
        float(ff7_externals.g_battle_camera_focal_point->z), 1.0f };
    float inView[4];
    bx::vec4MulMtx(inView, point, view);
    return inView[2] > 1.0f ? inView[2] : 0.0f;
}

static bool cineMotionThisFrame(const float n[4][4], const float *viewNow, bool viewSet, float out[16])
{
    if (!(ffuLookMotion() || ffuLookDof()) || !viewSet)
    {
        lastValid = moveValid = false;
        return false;
    }
    float clipNow[16];
    memcpy(clipNow, n, sizeof(clipNow));
    const bool continuous = lastValid && lastFrame + 1 == battleFrames;
    const bool same = continuous && memcmp(viewNow, lastView, sizeof(lastView)) == 0 && memcmp(clipNow, lastClip, sizeof(lastClip)) == 0;
    bool moved = false;
    if (continuous && !same)
    {
        float unclip[16], unview[16], back[16], step[16];
        bx::mtxInverse(unclip, clipNow);
        bx::mtxInverse(unview, viewNow);
        bx::mtxMul(back, unclip, unview);      // the screen to the world (this frame)
        bx::mtxMul(step, back, lastView);      // to the last frame's view
        bx::mtxMul(out, step, lastClip);       // to its clip
        // (a cut: the middle of the picture, at what the camera looks at, came from far off the last frame's)
        const float focus = focalDepth(viewNow);
        const float z = focus > 0.0f ? focus : 1000.0f;
        const float depth = (z * n[2][2] + n[3][2]) / (z * n[2][3] + n[3][3]);
        const float middle[4] = { 0.0f, 0.0f, depth, 1.0f };
        float then[4];
        bx::vec4MulMtx(then, middle, out);
        moved = then[3] > 0.0f && fabsf(then[0] / then[3]) < CUT && fabsf(then[1] / then[3]) < CUT;
        middleMove[0] = then[3] > 0.0f ? then[0] / then[3] : 99.0f;
        middleMove[1] = then[3] > 0.0f ? then[1] / then[3] : 99.0f;
        if (moved)
        {
            memcpy(lastReproject, out, sizeof(lastReproject));
            lastMove = battleFrames;
        }
        moveValid = moved;
    }
    else if (same && moveValid && lastMove + 1 == battleFrames)
    {
        memcpy(out, lastReproject, sizeof(lastReproject));
        moved = true;
        moveValid = false;   // (once)
    }
    memcpy(lastView, viewNow, sizeof(lastView));
    memcpy(lastClip, clipNow, sizeof(lastClip));
    lastFrame = battleFrames;
    lastValid = true;
    return moved;
}

// The pass (just before FF7 clears its depth for its menus, or at the frame's end).
void Renderer::ffuFF7Pass()
{
    const float barsAt = barsWeight();
    const bool haze = ffuLookAtmosphere();
    if ((!haze && barsAt <= 0.0f && !ffuLookLighting() && !ffuLookSpell() && !ffuLookRim() && !ffuLookShaking() && !ffuLookDof() && !ffuLookMotion()) || internalState.bIsHDR || enable_antialiasing > 0 || ffuLayerActive || ffuBattleLayerFrame
        || backendViewId > 240 || !ffuFF7Ready())
        return;
    ffuFF7Prepare();
    if (!bgfx::isValid(target) || bgfx::getAvailTransientVertexBuffer(24, quadLayout) < 24)
        return;

    // FF7's view depth from its depth: clip = (view x projection x viewport) as FFNx draws its 3D (row vectors, D3D),
    // so along the view axis depth = (z n22 + n32) / (z n23 + n33).
    float n[4][4];
    for (int row = 0; row < 4; row++)
        for (int column = 0; column < 4; column++)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += internalState.d3dProjectionMatrix[row * 4 + k] * internalState.d3dViewMatrix[k * 4 + column];
            n[row][column] = sum;
        }
    float stage = 0.0f;
    if (ff7_externals.g_battle_camera_position && ff7_externals.g_battle_camera_focal_point)
    {
        const float dx = float(ff7_externals.g_battle_camera_position->x - ff7_externals.g_battle_camera_focal_point->x);
        const float dy = float(ff7_externals.g_battle_camera_position->y - ff7_externals.g_battle_camera_focal_point->y);
        const float dz = float(ff7_externals.g_battle_camera_position->z - ff7_externals.g_battle_camera_focal_point->z);
        stage = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    const float contact = ffuLookLighting() && stage > 1.0f ? std::min(CONTACT * ffuLookLevel(2), 1.0f) : 0.0f;   // (its fade needs the stage's size)
    const bgfx::TextureHandle light = ffuGlowLight();
    const float spell = ffuLookSpell() && stage > 1.0f && bgfx::isValid(light) ? SPELL * ffuLookLevel(5) : 0.0f;
    const float rim = ffuLookRim() && stage > 1.0f ? RIM : 0.0f;   // (its fade needs the stage's size)
    const bool shaking = ffuLookShaking();
    const float cineAt = cineState > 0 ? cineWeight() : 0.0f;
    float reproject[16];
    const bool moved = cineMotionThisFrame(n, internalState.viewMatrix, internalState.isViewMatrixSet, reproject);   // (kept every frame with a cinematics switch on)
    const float dof = ffuLookDof() && cineAt > 0.0f ? DOF_APERTURE * cineAt : 0.0f;
    const bool motion = ffuLookMotion() && cineAt > 0.0f && moved;
    const bool look = haze || barsAt > 0.0f || contact > 0.0f || spell > 0.0f || rim > 0.0f || shaking;
    if (!look && dof <= 0.0f && !motion)
        return;
    const float aspect = float(framebufferWidth) / float(std::max<uint16_t>(framebufferHeight, 1));
    const float barHeight = std::max(0.0f, (1.0f - aspect / CINEMA_ASPECT) * 0.5f) * barsAt;
    const float hazeMost = haze && stage > 1.0f ? std::min(HAZE_MOST * ffuLookLevel(4), 0.9f) : 0.0f;
    const float depthRows[4] = { n[2][2], n[3][2], n[2][3], n[3][3] };
    const float hazeData[4] = { HAZE_START * stage, hazeMost, barHeight, ffuGlowWanted() ? 1.0f : 0.0f };

    // Each step draws the picture so far into the target and copies it back: the look's own, then the cinematics'.
    int step = 0;
    float shake[4];
    ffuLookShake(shake);   // (the impact's: ffu_look.cpp)
    auto draw = [&](bgfx::ProgramHandle which)
    {
        ++backendViewId;
        targetViews[std::min(step++, 3)] = backendViewId;
        bgfx::setViewMode(backendViewId, bgfx::ViewMode::Sequential);
        bgfx::setViewFrameBuffer(backendViewId, target);
        bgfx::setViewRect(backendViewId, 0, 0, framebufferWidth, framebufferHeight);
        bgfx::setViewClear(backendViewId, BGFX_CLEAR_NONE);
        bgfx::setViewTransform(backendViewId, nullptr, nullptr);
        bgfx::TransientVertexBuffer vertices;
        bgfx::allocTransientVertexBuffer(&vertices, 6, quadLayout);
        const QuadVertex corners[6] = {
            { -1.0f, 1.0f, 0.5f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.5f, 1.0f, 0.0f }, { 1.0f, -1.0f, 0.5f, 1.0f, 1.0f },
            { -1.0f, 1.0f, 0.5f, 0.0f, 0.0f }, { 1.0f, -1.0f, 0.5f, 1.0f, 1.0f }, { -1.0f, -1.0f, 0.5f, 0.0f, 1.0f },
        };
        memcpy(vertices.data, corners, sizeof(corners));
        bgfx::setVertexBuffer(0, &vertices);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
        bgfx::submit(backendViewId, which);

        ++backendViewId;
        bgfx::setViewMode(backendViewId, bgfx::ViewMode::Sequential);
        bgfx::setViewClear(backendViewId, BGFX_CLEAR_NONE);
        bgfx::blit(backendViewId, {.handle = backendFrameBufferRT[0]}, {.handle = bgfx::getTexture(target, 0), .x = 0, .y = 0, .width = framebufferWidth, .height = framebufferHeight});
        bgfx::touch(backendViewId);
    };

    if (look)
    {
        bgfx::setUniform(uniformDepth, depthRows);
        bgfx::setUniform(uniformHaze, hazeData);
        const float split[4] = { ffuLookSplit(), 0.0f, 0.0f, 0.0f };
        bgfx::setUniform(uniformSplit, split);
        const float contactData[4] = { contact, CONTACT_RING, 1.0f / float(framebufferWidth), 1.0f / float(std::max<uint16_t>(framebufferHeight, 1)) };
        bgfx::setUniform(uniformContact, contactData);
        const float spellData[4] = { spell, SPELL_DISC, SPELL_DISC * aspect, 0.0f };   // (round on the screen)
        bgfx::setUniform(uniformSpell, spellData);
        const float rimData[4] = { rim, RIM_REACH, RIM_REACH * aspect, 0.0f };   // (round on the screen)
        bgfx::setUniform(uniformRim, rimData);
        bgfx::setUniform(uniformShake, shake);
        // (no light: the scene itself bound, never read, as bgfx wants every sampler the program declares)
        bgfx::setTexture(2, samplerLight, spell > 0.0f ? light : backendFrameBufferRT[0], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        bgfx::setTexture(0, samplerScene, backendFrameBufferRT[0], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_POINT);
        bgfx::setTexture(1, samplerDepth, backendFrameBufferRT[1], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_POINT);
        draw(program);
    }

    // The cinematics (item 3): depth of field across and down, then the motion, each over the last step's picture
    const float focus = focalDepth(internalState.viewMatrix) > 0.0f ? focalDepth(internalState.viewMatrix) : stage;
    const float texel[2] = { 1.0f / float(framebufferWidth), 1.0f / float(std::max<uint16_t>(framebufferHeight, 1)) };
    auto cinePass = [&](float which)
    {
        static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        bgfx::setUniform(cineReproject, motion ? reproject : identity);
        bgfx::setUniform(cineDepth, depthRows);
        const float focusData[4] = { focus, dof, DOF_MOST * cineAt, which };
        bgfx::setUniform(cineFocus, focusData);
        const float frameData[4] = { barHeight, ffuLookSplit(), ffuGlowWanted() ? 1.0f : 0.0f, aspect };
        bgfx::setUniform(cineFrame, frameData);
        bgfx::setUniform(cineShake, shake);
        const float motionData[4] = { MOTION_SHARE * cineAt, MOTION_MOST, texel[0], texel[1] };
        bgfx::setUniform(cineMotion, motionData);
        bgfx::setTexture(0, cineScene, backendFrameBufferRT[0], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        bgfx::setTexture(1, cineDepthMap, backendFrameBufferRT[1], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_POINT);
        draw(cineProgram);
    };
    if (dof > 0.0f && focus > 1.0f)
    {
        cinePass(0.0f);
        cinePass(1.0f);
    }
    if (motion)
        cinePass(2.0f);
    static bool cineLogged = false;
    if ((dof > 0.0f || motion) && (!cineLogged || passes % 60 == 0))
        ffnx_info("FFU FF7 cinematics: depth of field %s (focus %.0f, stage %.0f), motion %s (the middle came %.1f %.1f px), weight %.2f\n",
                  dof > 0.0f ? "on" : "off", focus, stage, motion ? "on" : "off", middleMove[0] * 0.5f * framebufferWidth,
                  middleMove[1] * 0.5f * framebufferHeight, cineAt);
    cineLogged = dof > 0.0f || motion;

    ++backendViewId;   // FFNx carries on (the menus) in a view of its own, after these
    bgfx::setViewMode(backendViewId, bgfx::ViewMode::Sequential);
    bgfx::setViewClear(backendViewId, BGFX_CLEAR_NONE);
    bgfx::touch(backendViewId);
    if (passes++ % 600 == 0)
        ffnx_info("FFU FF7 haze: pass %u (stage %.0f, depth rows %.4g %.4g %.4g %.4g, bars %.3f)\n", passes, stage,
                  n[2][2], n[3][2], n[2][3], n[3][3], barHeight);
}
