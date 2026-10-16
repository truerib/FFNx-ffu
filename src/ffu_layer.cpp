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

// Final Fantasy Universe: the live layer, step 2 (docs/crossover-design.md, spikes/live-layer-transport).
// With FFU_FF7_LAYER=1, what a caller captured (FF7's flown airship, src/ff7/world/ffu.cpp: the state it was
// drawn with and a function that draws it again) is drawn a second time at the end of the frame, after all of
// FFNx's own views (interleaving the layer's views with FFNx's took the airship out of FF7's own picture), into
// an offscreen colour + depth target at half the internal resolution, read back to the CPU, and published in
// Z:\tmp\ffu-layer-ff7.shm for another game to composite. The layer's views are reset after the frame.
// This is the measuring spike: every 120 readbacks the log gives what each step cost.

#include <windows.h>
#include <cstring>
#include <cstdlib>

#include "renderer.h"
#include "log.h"

namespace
{
    constexpr uint32_t LAYER_MAGIC = 0x5259414Cu;   // 'LAYR', as spikes/live-layer-transport/layer_shm.h
    struct LayerHeader   // 64 bytes, no implicit padding; then RGBA8 colour rows, then float depth rows
    {
        uint32_t magic, version;
        volatile LONG seq;                // odd while being written
        uint32_t width, height;
        uint32_t frame;
        uint32_t fullWidth, fullHeight;   // the internal framebuffer the layer is a scaled copy of
        int64_t stamp, frequency;         // QueryPerformanceCounter when published
        float reserved[4];
    };
    static_assert(sizeof(LayerHeader) == 64, "the reader expects a 64-byte header");

    uint8_t *shared = nullptr;
    size_t sharedSize = 0;
    LARGE_INTEGER frequency;
    double readbackTotal = 0, readbackMax = 0, publishTotal = 0, publishMax = 0;
    uint32_t framesWaitedTotal = 0, published = 0, skipped = 0;

    // FFNx.ffu7.sh GameDepthAt on the CPU: the shader's mul(mul(viewport, projection), v) on FF7's row-major
    // matrices is the row vector (0, 0, w, 1) * projection * viewport.
    float gameDepthAt(const float *viewport, const float *projection, float w)
    {
        float row[4], clip[4];
        for (int j = 0; j < 4; ++j) row[j] = w * projection[8 + j] + projection[12 + j];
        for (int k = 0; k < 4; ++k) clip[k] = row[0] * viewport[k] + row[1] * viewport[4 + k] + row[2] * viewport[8 + k] + row[3] * viewport[12 + k];
        return clip[3] != 0.0f ? clip[2] / clip[3] : 0.0f;
    }

    bool layerWanted()
    {
        static int wanted = -1;
        if (wanted < 0)
        {
            const char *value = getenv("FFU_FF7_LAYER");
            wanted = value && value[0] == '1';
            QueryPerformanceFrequency(&frequency);
        }
        return wanted == 1;
    }

    // The shared file, mapped once at the size of the first layer (enough for a full-size one).
    uint8_t *mapShared(size_t needed)
    {
        if (shared && sharedSize >= needed) return shared;
        if (shared) return nullptr;   // a bigger layer than the first: not in the spike
        HANDLE file = CreateFileA("Z:\\tmp\\ffu-layer-ff7.shm", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) { ffnx_error("FFU layer: no shared file (%lu)\n", GetLastError()); return nullptr; }
        HANDLE map = CreateFileMappingA(file, nullptr, PAGE_READWRITE, 0, (DWORD)needed, nullptr);
        if (!map) { ffnx_error("FFU layer: no file mapping (%lu)\n", GetLastError()); return nullptr; }
        shared = (uint8_t *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, needed);
        if (!shared) { ffnx_error("FFU layer: no view (%lu)\n", GetLastError()); return nullptr; }
        sharedSize = needed;
        return shared;
    }
}

bool Renderer::ffuLayerWanted()
{
    return layerWanted();
}

void Renderer::ffuLayerCapture(void (*draw)())
{
    if (!layerWanted()) return;
    ffuLayerState = internalState;
    ffuLayerProgram = backendProgram;   // FFNx's post-processing switches it before the replay
    ffuLayerDraw = draw;
}

void Renderer::ffuLayerRender()
{
    if (!ffuLayerDraw) return;
    void (*draw)() = ffuLayerDraw;
    ffuLayerDraw = nullptr;
    RendererState current = internalState;
    const RendererProgram currentProgram = backendProgram;
    internalState = ffuLayerState;
    backendProgram = ffuLayerProgram;
    if (ffuLayerBegin())
    {
        draw();
        ffuLayerEnd();
    }
    discardAllBindings();
    internalState = current;
    backendProgram = currentProgram;
}

bool Renderer::ffuLayerBegin()
{
    if (!layerWanted() || ffuLayerActive) return false;
    const uint16_t width = framebufferWidth / 2, height = framebufferHeight / 2;
    if (!bgfx::isValid(ffuLayerFrameBuffer) || width != ffuLayerWidth || height != ffuLayerHeight)
    {
        if (bgfx::isValid(ffuLayerFrameBuffer)) bgfx::destroy(ffuLayerFrameBuffer);
        for (auto &slot : ffuLayerSlots)
            for (auto &texture : slot.read) if (bgfx::isValid(texture)) bgfx::destroy(texture);
        bgfx::TextureHandle targets[2] = {
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP),
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT),
        };
        ffuLayerFrameBuffer = bgfx::createFrameBuffer(2, targets, true);
        for (auto &slot : ffuLayerSlots)
        {
            slot.read[0] = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
            slot.read[1] = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
            slot.colour.assign((size_t)width * height * 4, 0);
            slot.depth.assign((size_t)width * height, 0.0f);
            slot.pending = 0;
        }
        ffuLayerWidth = width;
        ffuLayerHeight = height;
        ffnx_info("FFU layer: %ux%u target (half of %ux%u)\n", width, height, framebufferWidth, framebufferHeight);
    }
    ++backendViewId;
    ffuLayerViews[0] = backendViewId;
    bgfx::setViewMode(backendViewId, bgfx::ViewMode::Sequential);
    bgfx::setViewFrameBuffer(backendViewId, ffuLayerFrameBuffer);
    bgfx::setViewRect(backendViewId, 0, 0, width, height);
    bgfx::setViewClear(backendViewId, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f);
    bgfx::touch(backendViewId);
    ffuLayerActive = true;
    return true;
}

void Renderer::ffuLayerEnd()
{
    if (!ffuLayerActive) return;
    ffuLayerActive = false;
    ++backendViewId;
    ffuLayerViews[1] = backendViewId;
    bgfx::setViewMode(backendViewId, bgfx::ViewMode::Sequential);
    FfuLayerSlot *slot = nullptr;
    for (auto &candidate : ffuLayerSlots) if (candidate.pending == 0) { slot = &candidate; break; }
    if (!slot) { ++skipped; return; }   // three already in flight
    bgfx::blit(backendViewId, {.handle = slot->read[0]}, {.handle = bgfx::getTexture(ffuLayerFrameBuffer, 0)});
    bgfx::blit(backendViewId, {.handle = slot->read[1]}, {.handle = bgfx::getTexture(ffuLayerFrameBuffer, 1)});
    bgfx::touch(backendViewId);
    slot->pending = bgfx::read({.handle = slot->read[0]}, slot->colour.data());
    bgfx::read({.handle = slot->read[1]}, slot->depth.data());
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    slot->requestedAt = now.QuadPart;
    slot->requestedFrame = ffuLastFrame;
    slot->depths[0] = gameDepthAt(internalState.d3dViewMatrix, internalState.d3dProjectionMatrix, 30000.0f);
    slot->depths[1] = gameDepthAt(internalState.d3dViewMatrix, internalState.d3dProjectionMatrix, 60000.0f);
}

void Renderer::ffuLayerPoll()
{
    // The layer's views keep their framebuffer and clear until reset, and FFNx may use those ids next frame.
    for (auto &view : ffuLayerViews)
    {
        if (view) bgfx::resetView(view);
        view = 0;
    }
    for (;;)   // every finished readback, oldest first
    {
        FfuLayerSlot *slot = nullptr;
        for (auto &candidate : ffuLayerSlots)
            if (candidate.pending && candidate.pending <= ffuLastFrame && (!slot || candidate.pending < slot->pending)) slot = &candidate;
        if (!slot) return;
        publishLayer(*slot);
        slot->pending = 0;
    }
}

void Renderer::publishLayer(FfuLayerSlot &slot)
{
    LARGE_INTEGER ready, done;
    QueryPerformanceCounter(&ready);
    const size_t pixels = (size_t)ffuLayerWidth * ffuLayerHeight;
    uint8_t *base = mapShared(sizeof(LayerHeader) + pixels * 8);
    if (base)
    {
        LayerHeader *header = (LayerHeader *)base;
        InterlockedIncrement(&header->seq);   // odd: writing
        header->magic = LAYER_MAGIC;
        header->version = 2;
        header->width = ffuLayerWidth;
        header->height = ffuLayerHeight;
        header->fullWidth = framebufferWidth;
        header->fullHeight = framebufferHeight;
        header->frame = slot.pending;
        header->frequency = frequency.QuadPart;
        // To turn the layer's depth (WorldDepth, FFNx.ffu7.sh) back into view distance: the game's depth at
        // 30,000 and 60,000 units and which end of the range is far.
        header->reserved[0] = slot.depths[0];
        header->reserved[1] = slot.depths[1];
        header->reserved[2] = slot.depths[1] < slot.depths[0] ? 0.0f : 1.0f;
        header->reserved[3] = 0.0f;
        ::memcpy(base + sizeof(LayerHeader), slot.colour.data(), pixels * 4);
        ::memcpy(base + sizeof(LayerHeader) + pixels * 4, slot.depth.data(), pixels * 4);
        QueryPerformanceCounter(&done);
        header->stamp = done.QuadPart;
        InterlockedIncrement(&header->seq);   // even: published
    }
    else QueryPerformanceCounter(&done);
    const double readback = (ready.QuadPart - slot.requestedAt) * 1000.0 / frequency.QuadPart;
    const double publish = (done.QuadPart - ready.QuadPart) * 1000.0 / frequency.QuadPart;
    readbackTotal += readback; if (readback > readbackMax) readbackMax = readback;
    publishTotal += publish; if (publish > publishMax) publishMax = publish;
    framesWaitedTotal += ffuLastFrame - slot.requestedFrame;
    if (++published % 120 == 0)
    {
        size_t covered = 0;
        for (size_t i = 3; i < slot.colour.size(); i += 4) covered += slot.colour[i] != 0;
        ffnx_info("FFU layer: %u readbacks of %ux%u: request to ready %.2f ms mean (%.2f max), %.2f frames; publish %.2f ms mean (%.2f max); %u skipped (ring full); last frame %.1f%% covered; depth at 30k/60k %.6f/%.6f\n",
                  published, ffuLayerWidth, ffuLayerHeight, readbackTotal / 120, readbackMax, framesWaitedTotal / 120.0,
                  publishTotal / 120, publishMax, skipped, 100.0 * covered / pixels, slot.depths[0], slot.depths[1]);
        readbackTotal = readbackMax = publishTotal = publishMax = 0;
        framesWaitedTotal = 0;
        skipped = 0;
    }
}
