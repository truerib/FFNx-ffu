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

// Final Fantasy Universe: the battle look's glow (docs/battle-look.md "Glow"), FF7's and FF8's battles alike: only the
// game's own effects glow, as in FF9's (mods/ff9-globe BattleGlow.cs).
//
// While a battle is shown (not while a hidden battle draws into the live layer), every draw into FFNx's frame keeps
// its colour exactly and only its alpha changes: additive draws (the games' effects: PlayStation abr 1 and 3) add their
// alpha, every other draw clears it under itself (ffuGlowState). Before the post-processing pass the bright part where
// alpha is left is extracted at half size and blurred there and at a quarter (ffuGlowBuild); after it, both are added
// onto the picture with ONE+ONE in its own gamma numbers, as the PlayStation frame buffers added (ffuGlowComposite).
// Targets are made once per frame size (Rule 89). Vulkan only, like the fork's other shaders; nothing changes when
// the program is missing, in HDR, or with the glow switched off (ffu_look).

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "renderer.h"
#include "cfg.h"
#include "common.h"
#include "globals.h"
#include "log.h"
#include "utils.h"

bool ffuLookGlow();   // ffu_look.cpp

namespace
{
    constexpr float THRESHOLD = 0.08f, KNEE = 0.4f, SMALL = 0.9f, WIDE = 0.7f;
    // Views of the glow's own, never FFNx's running count: FFNx's post-processing view sets no frame buffer of its own
    // (Renderer::draw), so a view id of ours reused by it in a later frame (FF8's battles never reset theirs) would send
    // the picture into our targets. bgfx runs views in id order: these come after all of FFNx's, its post pass included.
    constexpr bgfx::ViewId FIRST_VIEW = 248, COMPOSITE_VIEW = 254;

    int programState = 0;   // 0 not tried, 1 ready, -1 unavailable
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uniformPass = BGFX_INVALID_HANDLE, uniformParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle samplerA = BGFX_INVALID_HANDLE, samplerB = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout quadLayout;
    bgfx::FrameBufferHandle halves[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
    bgfx::FrameBufferHandle quarters[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
    bgfx::FrameBufferHandle sharpened = BGFX_INVALID_HANDLE;   // the frame anti-aliased (FXAA), the frame's own size
    uint16_t sharpWidth = 0, sharpHeight = 0;
    uint16_t halfWidth = 0, halfHeight = 0;
    bool built = false;   // this frame's glow is in halves[0] and quarters[0]

    struct QuadVertex { float x, y, z, u, v; };

    bgfx::FrameBufferHandle makeTarget(uint16_t width, uint16_t height)
    {
        return bgfx::createFrameBuffer(width, height, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    }

    void targets(uint16_t frameWidth, uint16_t frameHeight)
    {
        uint16_t width = std::max<uint16_t>(frameWidth / 2, 1), height = std::max<uint16_t>(frameHeight / 2, 1);
        if (width == halfWidth && height == halfHeight && bgfx::isValid(halves[0]))
            return;
        for (bgfx::FrameBufferHandle *target : { &halves[0], &halves[1], &quarters[0], &quarters[1] })
            if (bgfx::isValid(*target)) bgfx::destroy(*target);
        halves[0] = makeTarget(width, height);
        halves[1] = makeTarget(width, height);
        quarters[0] = makeTarget(std::max<uint16_t>(width / 2, 1), std::max<uint16_t>(height / 2, 1));
        quarters[1] = makeTarget(std::max<uint16_t>(width / 2, 1), std::max<uint16_t>(height / 2, 1));
        halfWidth = width;
        halfHeight = height;
        ffnx_info("FFU glow: targets made (%ux%u and a quarter)\n", width, height);
    }

    // One full-screen pass of the glow program into `target` (clip space; uv 0,0 at the top left, as the frame's). Its
    // view's sort mode is left alone: one draw each, and a view id of FFNx's count must keep FFNx's own mode.
    bool pass(bgfx::ViewId view, bgfx::FrameBufferHandle target, uint16_t width, uint16_t height, float mode,
              float stepU, float stepV, bgfx::TextureHandle a, bgfx::TextureHandle b)
    {
        if (bgfx::getAvailTransientVertexBuffer(6, quadLayout) < 6) return false;
        bgfx::setViewFrameBuffer(view, target);
        bgfx::setViewRect(view, 0, 0, width, height);
        bgfx::setViewClear(view, BGFX_CLEAR_COLOR, 0x00000000);
        bgfx::setViewTransform(view, nullptr, nullptr);
        bgfx::TransientVertexBuffer vertices;
        bgfx::allocTransientVertexBuffer(&vertices, 6, quadLayout);
        const QuadVertex corners[6] = {
            { -1.0f, 1.0f, 0.5f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.5f, 1.0f, 0.0f }, { 1.0f, -1.0f, 0.5f, 1.0f, 1.0f },
            { -1.0f, 1.0f, 0.5f, 0.0f, 0.0f }, { 1.0f, -1.0f, 0.5f, 1.0f, 1.0f }, { -1.0f, -1.0f, 0.5f, 0.0f, 1.0f },
        };
        memcpy(vertices.data, corners, sizeof(corners));
        const float passData[4] = { mode, stepU, stepV, 0.0f };
        const float params[4] = { THRESHOLD, KNEE, SMALL, WIDE };
        bgfx::setUniform(uniformPass, passData);
        bgfx::setUniform(uniformParams, params);
        bgfx::setTexture(0, samplerA, a, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (bgfx::isValid(b)) bgfx::setTexture(1, samplerB, b, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        bgfx::setVertexBuffer(0, &vertices);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
        bgfx::submit(view, program);
        return true;
    }
}

// The glow's program, made once (Vulkan only, like the fork's other shaders).
bool Renderer::ffuGlowReady()
{
    if (programState != 0) return programState > 0;
    programState = -1;
    const char *vertex = "shaders/FFNx.ffuglow.flat.vk.vert", *fragment = "shaders/FFNx.ffuglow.flat.vk.frag";
    char vertexPath[MAX_PATH], fragmentPath[MAX_PATH];
    _snprintf(vertexPath, sizeof(vertexPath), "%s/%s", basedir, vertex);
    _snprintf(fragmentPath, sizeof(fragmentPath), "%s/%s", basedir, fragment);
    if (bgfx::getCaps()->rendererType != bgfx::RendererType::Vulkan || !fileExists(vertexPath) || !fileExists(fragmentPath)
        || bgfx::getCaps()->limits.maxViews <= COMPOSITE_VIEW)
    {
        ffnx_error("FFU glow: unavailable (needs Vulkan, %s and %u views): no glow in battles\n", vertex, COMPOSITE_VIEW + 1);
        return false;
    }
    program = bgfx::createProgram(getShader(vertex), getShader(fragment), true);
    uniformPass = bgfx::createUniform("u_ffuGlow", bgfx::UniformType::Vec4);
    uniformParams = bgfx::createUniform("u_ffuGlowParams", bgfx::UniformType::Vec4);
    samplerA = bgfx::createUniform("s_ffuGlowA", bgfx::UniformType::Sampler);
    samplerB = bgfx::createUniform("s_ffuGlowB", bgfx::UniformType::Sampler);
    quadLayout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float).end();
    programState = bgfx::isValid(program) ? 1 : -1;
    ffnx_info("FFU glow: %s\n", programState > 0 ? "ready" : "its program failed");
    return programState > 0;
}

// This draw goes into FFNx's frame while a battle is shown (Renderer::draw, its backend frame buffer branch).
bool Renderer::ffuGlowWanted()
{
    return ffuLookGlow() && !internalState.bIsHDR && getmode_cached()->driver_mode == MODE_BATTLE
        && !ffuLayerActive && !ffuBattleLayerFrame && programState >= 0;
}

// The draw's state with its colour blend as FFNx set it and its alpha: added by additive draws, cleared under every
// other draw (and kept under a subtractive one).
uint64_t Renderer::ffuGlowState(uint64_t state)
{
    uint64_t rgbSrc = BGFX_STATE_BLEND_ONE, rgbDst = BGFX_STATE_BLEND_ZERO, aSrc = BGFX_STATE_BLEND_ZERO, aDst = BGFX_STATE_BLEND_ZERO;
    uint64_t rgbEquation = BGFX_STATE_BLEND_EQUATION_ADD;
    switch (internalState.blendMode)
    {
    case RendererBlendMode::BLEND_AVG:
        rgbSrc = BGFX_STATE_BLEND_SRC_ALPHA; rgbDst = BGFX_STATE_BLEND_INV_SRC_ALPHA; aDst = BGFX_STATE_BLEND_INV_SRC_ALPHA;
        break;
    case RendererBlendMode::BLEND_ADD:
        rgbDst = BGFX_STATE_BLEND_ONE; aSrc = BGFX_STATE_BLEND_ONE; aDst = BGFX_STATE_BLEND_ONE;
        break;
    case RendererBlendMode::BLEND_SUB:
        rgbDst = BGFX_STATE_BLEND_ONE; rgbEquation = BGFX_STATE_BLEND_EQUATION_REVSUB; aDst = BGFX_STATE_BLEND_ONE;
        break;
    case RendererBlendMode::BLEND_25P:
        rgbSrc = BGFX_STATE_BLEND_SRC_ALPHA; rgbDst = BGFX_STATE_BLEND_ONE; aSrc = BGFX_STATE_BLEND_SRC_ALPHA; aDst = BGFX_STATE_BLEND_ONE;
        break;
    case RendererBlendMode::BLEND_NONE:
        if (internalState.bIsExternalTexture && !ff8)
        {
            rgbSrc = BGFX_STATE_BLEND_SRC_ALPHA; rgbDst = BGFX_STATE_BLEND_INV_SRC_ALPHA; aDst = BGFX_STATE_BLEND_INV_SRC_ALPHA;
        }
        break;
    default:   // (no blend: written as it is, alpha cleared)
        break;
    }
    state &= ~(BGFX_STATE_BLEND_MASK | BGFX_STATE_BLEND_EQUATION_MASK);
    return state | BGFX_STATE_BLEND_FUNC_SEPARATE(rgbSrc, rgbDst, aSrc, aDst)
        | BGFX_STATE_BLEND_EQUATION_SEPARATE(rgbEquation, BGFX_STATE_BLEND_EQUATION_ADD);
}

bool ffuLookSharp();   // ffu_look.cpp

// Before the glow and the post-processing pass (Renderer::renderFrame), every frame: in a battle with the
// anti-aliasing switched on, the frame through FXAA into a target of its own size, which the glow and the post pass then
// use (alpha kept: the glow's mask; not in the live layer's frames, nor in HDR). It must run before the post pass, so it
// takes the next view of FFNx's own count; FFNx's post view sets no frame buffer of its own, so the view after this one
// (this frame's post view) is given the screen explicitly, every frame, or a later frame's post pass on a view id this
// pass once used would draw into its target instead of the screen.
bgfx::TextureHandle Renderer::ffuSharpen(bgfx::TextureHandle frame, uint16_t frameWidth, uint16_t frameHeight)
{
    struct PostView
    {
        Renderer &renderer;
        ~PostView() { bgfx::setViewFrameBuffer(renderer.backendViewId + 1, BGFX_INVALID_HANDLE); }
    } postView{ *this };
    if (!ffuLookSharp() || internalState.bIsHDR || getmode_cached()->driver_mode != MODE_BATTLE || ffuBattleLayerFrame
        || !ffuGlowReady())
        return frame;
    if (!bgfx::isValid(sharpened) || sharpWidth != frameWidth || sharpHeight != frameHeight)
    {
        if (bgfx::isValid(sharpened)) bgfx::destroy(sharpened);
        sharpened = bgfx::createFrameBuffer(frameWidth, frameHeight, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        sharpWidth = frameWidth;
        sharpHeight = frameHeight;
        ffnx_info("FFU sharpen: target made (%ux%u)\n", frameWidth, frameHeight);
    }
    bgfx::TextureHandle none = BGFX_INVALID_HANDLE;
    if (!pass(++backendViewId, sharpened, frameWidth, frameHeight, 3.0f, 1.0f / frameWidth, 1.0f / frameHeight, frame, none))
        return frame;
    return bgfx::getTexture(sharpened);
}

// Before the post-processing pass (Renderer::renderFrame): this frame's glow, from its frame (full size).
void Renderer::ffuGlowBuild(bgfx::TextureHandle frame, uint16_t frameWidth, uint16_t frameHeight)
{
    built = false;
    const bool drawn = ffuGlowFrame;
    ffuGlowFrame = false;
    if (!drawn || !ffuGlowWanted() || !ffuGlowReady())
        return;
    targets(frameWidth, frameHeight);
    const uint16_t qw = std::max<uint16_t>(halfWidth / 2, 1), qh = std::max<uint16_t>(halfHeight / 2, 1);
    bgfx::TextureHandle none = BGFX_INVALID_HANDLE;
    bgfx::ViewId view = FIRST_VIEW;
    bool ok = pass(view++, halves[0], halfWidth, halfHeight, 0.0f, 0.0f, 0.0f, frame, none)
        && pass(view++, halves[1], halfWidth, halfHeight, 1.0f, 1.0f / halfWidth, 0.0f, bgfx::getTexture(halves[0]), none)
        && pass(view++, halves[0], halfWidth, halfHeight, 1.0f, 0.0f, 1.0f / halfHeight, bgfx::getTexture(halves[1]), none)
        && pass(view++, quarters[0], qw, qh, 1.0f, 0.0f, 0.0f, bgfx::getTexture(halves[0]), none)   // (a plain copy down)
        && pass(view++, quarters[1], qw, qh, 1.0f, 2.0f / qw, 0.0f, bgfx::getTexture(quarters[0]), none)
        && pass(view++, quarters[0], qw, qh, 1.0f, 0.0f, 2.0f / qh, bgfx::getTexture(quarters[1]), none);
    built = ok;
}

// After the post-processing pass: the glow added onto the picture where the frame was drawn (x0..x2 in the post
// pass's game units, its full height).
void Renderer::ffuGlowComposite(float x0, float x2, float gameWidth, float gameHeight)
{
    if (!built || bgfx::getAvailTransientVertexBuffer(6, quadLayout) < 6)
        return;
    built = false;
    const bgfx::ViewId view = COMPOSITE_VIEW;
    bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
    bgfx::setViewRect(view, 0, 0, (uint16_t)window_size_x, (uint16_t)window_size_y);
    bgfx::setViewClear(view, BGFX_CLEAR_NONE);
    bgfx::setViewTransform(view, nullptr, nullptr);
    const float left = 2.0f * x0 / gameWidth - 1.0f, right = 2.0f * x2 / gameWidth - 1.0f;
    bgfx::TransientVertexBuffer vertices;
    bgfx::allocTransientVertexBuffer(&vertices, 6, quadLayout);
    const QuadVertex corners[6] = {
        { left, 1.0f, 0.5f, 0.0f, 0.0f }, { right, 1.0f, 0.5f, 1.0f, 0.0f }, { right, -1.0f, 0.5f, 1.0f, 1.0f },
        { left, 1.0f, 0.5f, 0.0f, 0.0f }, { right, -1.0f, 0.5f, 1.0f, 1.0f }, { left, -1.0f, 0.5f, 0.0f, 1.0f },
    };
    memcpy(vertices.data, corners, sizeof(corners));
    const float passData[4] = { 2.0f, 0.0f, 0.0f, 0.0f };
    const float params[4] = { THRESHOLD, KNEE, SMALL, WIDE };
    bgfx::setUniform(uniformPass, passData);
    bgfx::setUniform(uniformParams, params);
    bgfx::setTexture(0, samplerA, bgfx::getTexture(halves[0]), BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    bgfx::setTexture(1, samplerB, bgfx::getTexture(quarters[0]), BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    bgfx::setVertexBuffer(0, &vertices);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE));
    bgfx::submit(view, program);
}
