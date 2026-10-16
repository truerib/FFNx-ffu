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
// With FFU_FF7_LAYER=1, what the caller draws between ffuLayerBegin and ffuLayerEnd (FF7's flown airship,
// src/ff7/world/ffu.cpp) is drawn a second time into an offscreen colour + depth target at half the internal
// resolution, read back to the CPU, and published in Z:\tmp\ffu-layer-ff7.shm for another game to composite.
// This is the measuring spike: every 120 readbacks the log gives what each step cost.

#include <windows.h>
#include <cstring>
#include <cstdlib>

#include "renderer.h"
#include "log.h"

namespace
{
    constexpr uint32_t LAYER_MAGIC = 0x5259414Cu;   // 'LAYR', as spikes/live-layer-transport/layer_shm.h
    struct LayerHeader
    {
        uint32_t magic, version;
        volatile LONG seq;
        uint32_t width, height;
        int64_t stamp, frequency;
        uint32_t frame;
        uint32_t fullWidth, fullHeight;   // the internal framebuffer the layer is a scaled copy of
        uint32_t pad[5];
    };
    static_assert(sizeof(LayerHeader) == 64, "the reader expects a 64-byte header");

    uint8_t *shared = nullptr;
    size_t sharedSize = 0;
    LARGE_INTEGER frequency, requested;
    double readbackTotal = 0, readbackMax = 0, publishTotal = 0, publishMax = 0;
    uint32_t framesWaitedTotal = 0, published = 0, requestedFrame = 0;

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

bool Renderer::ffuLayerBegin()
{
    if (!layerWanted() || ffuLayerActive) return false;
    const uint16_t width = framebufferWidth / 2, height = framebufferHeight / 2;
    if (!bgfx::isValid(ffuLayerFrameBuffer) || width != ffuLayerWidth || height != ffuLayerHeight)
    {
        if (bgfx::isValid(ffuLayerFrameBuffer)) bgfx::destroy(ffuLayerFrameBuffer);
        for (auto &texture : ffuLayerRead) if (bgfx::isValid(texture)) bgfx::destroy(texture);
        bgfx::TextureHandle targets[2] = {
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP),
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT),
        };
        ffuLayerFrameBuffer = bgfx::createFrameBuffer(2, targets, true);
        ffuLayerRead[0] = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        ffuLayerRead[1] = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        ffuLayerColour.assign((size_t)width * height * 4, 0);
        ffuLayerDepth.assign((size_t)width * height, 0.0f);
        ffuLayerWidth = width;
        ffuLayerHeight = height;
        ffuLayerPending = 0;
        ffnx_info("FFU layer: %ux%u target (half of %ux%u), blit %s, read back %s\n", width, height, framebufferWidth, framebufferHeight,
                  (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_BLIT) ? "yes" : "NO",
                  (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_READ_BACK) ? "yes" : "NO");
    }
    ++backendViewId;
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
    bgfx::setViewMode(backendViewId, bgfx::ViewMode::Sequential);
    if (ffuLayerPending == 0)   // one readback in flight at a time
    {
        bgfx::blit(backendViewId, ffuLayerRead[0], 0, 0, bgfx::getTexture(ffuLayerFrameBuffer, 0));
        bgfx::blit(backendViewId, ffuLayerRead[1], 0, 0, bgfx::getTexture(ffuLayerFrameBuffer, 1));
        bgfx::touch(backendViewId);
        ffuLayerPending = bgfx::readTexture(ffuLayerRead[0], ffuLayerColour.data());
        bgfx::readTexture(ffuLayerRead[1], ffuLayerDepth.data());
        QueryPerformanceCounter(&requested);
        requestedFrame = ffuLastFrame;
    }
    ++backendViewId;   // what follows draws on the main framebuffer again (draw() sets it per call)
    bgfx::setViewMode(backendViewId, bgfx::ViewMode::Sequential);
}

void Renderer::ffuLayerPoll()
{
    if (ffuLayerPending == 0 || ffuLastFrame < ffuLayerPending) return;
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
        header->frame = ffuLayerPending;
        header->frequency = frequency.QuadPart;
        ::memcpy(base + sizeof(LayerHeader), ffuLayerColour.data(), pixels * 4);
        ::memcpy(base + sizeof(LayerHeader) + pixels * 4, ffuLayerDepth.data(), pixels * 4);
        QueryPerformanceCounter(&done);
        header->stamp = done.QuadPart;
        InterlockedIncrement(&header->seq);   // even: published
    }
    else QueryPerformanceCounter(&done);
    const double readback = (ready.QuadPart - requested.QuadPart) * 1000.0 / frequency.QuadPart;
    const double publish = (done.QuadPart - ready.QuadPart) * 1000.0 / frequency.QuadPart;
    readbackTotal += readback; if (readback > readbackMax) readbackMax = readback;
    publishTotal += publish; if (publish > publishMax) publishMax = publish;
    framesWaitedTotal += ffuLastFrame - requestedFrame;
    if (++published % 120 == 0)
    {
        size_t covered = 0;
        for (size_t i = 3; i < ffuLayerColour.size(); i += 4) covered += ffuLayerColour[i] != 0;
        ffnx_info("FFU layer: %u readbacks of %ux%u: request to ready %.2f ms mean (%.2f max), %.2f frames; publish %.2f ms mean (%.2f max); last frame %.1f%% covered\n",
                  published, ffuLayerWidth, ffuLayerHeight, readbackTotal / 120, readbackMax, framesWaitedTotal / 120.0,
                  publishTotal / 120, publishMax, 100.0 * covered / pixels);
        readbackTotal = readbackMax = publishTotal = publishMax = 0;
        framesWaitedTotal = 0;
    }
    ffuLayerPending = 0;
}
