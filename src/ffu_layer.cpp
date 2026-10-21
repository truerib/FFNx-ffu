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

// Final Fantasy Universe: the live layer (docs/crossover-design.md, spikes/live-layer-transport). Every game
// draws its own airship, live, and the game you are in lays those pixels into its picture.
// - Publishing (FFU_FF7_LAYER=1 / FFU_FF8_LAYER=1): what a caller captured (the game's own flown airship,
//   src/ff7/world/ffu.cpp, src/ff8/world/renderer.cpp: the state it was drawn with and a function that draws it
//   again) is drawn a second time at the end of the frame, after all of FFNx's own views (interleaving the
//   layer's views with FFNx's took the airship out of the game's own picture), into an offscreen colour + depth
//   target. A box round the airship is read back and published in Z:\tmp\ffu-layer-<game>.shm.
// - Compositing (ffuLayerLive / ffuLayerComposite): another game's published airship is uploaded and drawn on
//   a quad over its crop, inside this game's world pass, with its depth turned into ours (FFNx.ffulayer.frag),
//   while the caller stops drawing its copy of that airship (Rule 64). FF9 composites the same files itself
//   (mods/ff9-globe/LiveLayer.cs) and publishes its own (LivePublish.cs).
// Every 120 readbacks / received frames the log gives what each step cost.

#include <windows.h>
#include <cstring>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>

#include "renderer.h"
#include "log.h"
#include "utils.h"
#include "common.h"

namespace
{
    constexpr uint32_t LAYER_MAGIC = 0x5259414Cu;   // 'LAYR', as spikes/live-layer-transport/layer_shm.h
    constexpr uint32_t LAYER_VERSION = 4;
    // Header v4: 128 bytes, no implicit padding; then RGBA8 colour rows (top row first), then 4-byte depth rows.
    // The first 64 bytes are v3's. Readers: mods/ff9-globe/LiveLayer.cs, and receive() here.
    struct LayerHeader
    {
        uint32_t magic, version;          // version 4
        volatile LONG seq;                // odd while being written
        uint32_t width, height;           // the crop that follows
        uint32_t frame;
        uint32_t fullWidth, fullHeight;   // the picture the crop was taken from
        int64_t stamp, frequency;         // QueryPerformanceCounter when published
        float depthAt30k, depthAt60k;     // FFU_LAYER_DEPTH_FF7: the game's depth at those view distances,
        float farEnd;                     //   and which end of the range is far (FFNx.ffu7.sh WorldDepth)
        uint16_t cropX, cropY;            // where the crop sits in the picture (top-left, pixels)
        uint32_t depthFormat;             // FFU_LAYER_DEPTH_*
        float unitsPerR;                  // the publisher's units per planet radius for the flight camera this frame
        float nearDistance, farDistance;  // FFU_LAYER_DEPTH_PACKED: the range t spans (publisher's units)
        char id[24];                      // what it shows: the airship's model id (mesh/airships/<id>.gltf)
        uint32_t covered;                 // pixels with alpha on the publisher's sampling grid: 0 = nothing shown
        uint32_t reserved[5];
    };
    static_assert(sizeof(LayerHeader) == 128, "the readers expect a 128-byte header");
    constexpr uint32_t FFU_LAYER_DEPTH_FF7 = Renderer::FFU_LAYER_DEPTH_FF7, FFU_LAYER_DEPTH_PACKED = Renderer::FFU_LAYER_DEPTH_PACKED,
                       FFU_LAYER_DEPTH_FF8 = Renderer::FFU_LAYER_DEPTH_FF8;
    constexpr double LIVE_SECONDS = 0.3;   // a layer older than this isn't live (its game was frozen or stopped)

    const char *ownGame() { return ff8 ? "ff8" : "ff7"; }

    // Z:\tmp\ffu-layer-<game>.shm in Richard's Play copy (FFU_CHANNEL=play), ffu-layer-<game>-dev.shm in the dev copy
    // (docs/play-copy.md), so the two copies never read each other's layers. Same rule as mods/ff9-globe LivePublish.cs.
    void layerPath(char (&path)[64], const char *game)
    {
        char channel[8];
        const DWORD length = GetEnvironmentVariableA("FFU_CHANNEL", channel, sizeof(channel));
        const bool play = length && length < sizeof(channel) && !strcmp(channel, "play");
        _snprintf(path, sizeof(path), "Z:\\tmp\\ffu-layer-%s%s.shm", game, play ? "" : "-dev");
        path[sizeof(path) - 1] = 0;
    }

    // ---- Publishing ----
    uint8_t *shared = nullptr;
    size_t sharedSize = 0;
    LARGE_INTEGER frequency;
    // The box read back each frame round the airship, at the layer's full resolution: a fixed 2048 x 1600 (the
    // Ragnarok at the default zoom fits; a 1280 x 800 box cut it in half), so nothing is remade in flight (see
    // ffuLayerPrepare) and a host's textures keep their size. It follows the airship from the alpha of the last
    // frames (the chase camera always looks at it, so it hardly moves).
    constexpr uint16_t MAX_CROP_WIDTH = 2048, MAX_CROP_HEIGHT = 1600;   // the shared file's room
    uint16_t cropWidth = MAX_CROP_WIDTH, cropHeight = MAX_CROP_HEIGHT;
    DWORD mapRetryAt = 0;       // the shared file couldn't be mapped: try again from then
    float cropCentre[2] = { -1.0f, -1.0f };
    double readbackTotal = 0, readbackMax = 0, publishTotal = 0, publishMax = 0;
    uint32_t framesWaitedTotal = 0, published = 0, skipped = 0;
    struct Meta { char id[24]; float unitsPerR; uint32_t depthFormat; };
    Meta captured = {};
    Meta slotMeta[3] = {};

    // FFNx.ffu7.sh GameDepthAt on the CPU: the shader's mul(mul(viewport, projection), v) on FF7's row-major
    // matrices is the row vector (0, 0, w, 1) * projection * viewport.
    float gameDepthAt(const float *viewport, const float *projection, float w)
    {
        float row[4], clip[4];
        for (int j = 0; j < 4; ++j) row[j] = w * projection[8 + j] + projection[12 + j];
        for (int k = 0; k < 4; ++k) clip[k] = row[0] * viewport[k] + row[1] * viewport[4 + k] + row[2] * viewport[8 + k] + row[3] * viewport[12 + k];
        return clip[3] != 0.0f ? clip[2] / clip[3] : 0.0f;
    }

    // Mixed parties (docs/shared-engine.md "the guest's effects"): the FF7 bridge asks for whole battle frames in the layer
    // while its hidden battle plays a guest's move (mods/bridge-ff7 layer_ff7.c hides FF7's stage, bodies, numbers and
    // menu and sets the host's camera), so the host lays the guest's own effects into its battle: {'F7BL', version, on}.
    struct BattleAsk { uint32_t magic, version; volatile LONG on; };
    const BattleAsk *battleAsk()
    {
        static const BattleAsk *found = nullptr;
        static DWORD lastLook = 0;
        if (found || ff8) return found;
        const DWORD now = GetTickCount();
        if (lastLook && now - lastLook < 1000) return nullptr;   // look again once a second
        lastLook = now;
        HMODULE bridge = GetModuleHandleA("dinput8.dll");
        auto candidate = bridge ? (const BattleAsk *)GetProcAddress(bridge, "ffu_ff7_battle_layer") : nullptr;
        if (candidate && candidate->magic == 0x4C423746u && candidate->version >= 1) found = candidate;
        return found;
    }

    bool battleWanted()
    {
        const BattleAsk *ask = battleAsk();
        return ask && ask->on && getmode_cached()->driver_mode == MODE_BATTLE;
    }

    bool layerWanted()
    {
        static int wanted = -1;
        if (wanted < 0)
        {
            const char *value = getenv(ff8 ? "FFU_FF8_LAYER" : "FFU_FF7_LAYER");
            wanted = value && value[0] == '1';
            QueryPerformanceFrequency(&frequency);
            const char *crop = getenv(ff8 ? "FFU_FF8_LAYER_CROP" : "FFU_FF7_LAYER_CROP");   // WxH (tests)
            unsigned w = 0, h = 0;
            if (crop && sscanf(crop, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64 && w <= MAX_CROP_WIDTH && h <= MAX_CROP_HEIGHT) { cropWidth = (uint16_t)w; cropHeight = (uint16_t)h; }
        }
        return wanted == 1;
    }

    // The shared file, mapped once at the size of the first layer (enough for a full-size one).
    uint8_t *mapShared(size_t needed)
    {
        if (shared && sharedSize >= needed) return shared;
        if (shared) return nullptr;   // bigger than the room made for the largest crop: not supported
        if (mapRetryAt && (LONG)(GetTickCount() - mapRetryAt) < 0) return nullptr;
        needed = std::max(needed, sizeof(LayerHeader) + (size_t)MAX_CROP_WIDTH * MAX_CROP_HEIGHT * 8);
        char path[64];
        layerPath(path, ownGame());
        mapRetryAt = GetTickCount() + 5000;   // until it works, at most one try every 5 s
        HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) { ffnx_error("FFU layer: no shared file %s (%lu)\n", path, GetLastError()); return nullptr; }
        HANDLE map = CreateFileMappingA(file, nullptr, PAGE_READWRITE, 0, (DWORD)needed, nullptr);
        CloseHandle(file);
        if (!map) { ffnx_error("FFU layer: no file mapping (%lu)\n", GetLastError()); return nullptr; }
        shared = (uint8_t *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, needed);
        CloseHandle(map);   // the view keeps the mapping alive
        if (!shared) { ffnx_error("FFU layer: no view (%lu)\n", GetLastError()); return nullptr; }
        sharedSize = needed;
        mapRetryAt = 0;
        // The file outlives the game: a run stopped mid-publish leaves its sequence odd ("being written"), which
        // would turn this run's meaning of odd and even round and every reader would refuse every frame.
        LayerHeader *header = (LayerHeader *)shared;
        if (header->seq & 1) InterlockedIncrement(&header->seq);
        return shared;
    }

    // ---- Compositing: the other games' layers ----
    struct Guest
    {
        const char *game;
        const uint8_t *view = nullptr;
        size_t size = 0;
        DWORD nextTry = 0;
        LONG lastSeq = -1;
        LARGE_INTEGER arrived = {};          // when the last new frame came in (our clock)
        uint32_t polledFrame = UINT32_MAX;   // our frame it was last looked at in
        LayerHeader header = {};             // the last frame's
        bgfx::TextureHandle colour = BGFX_INVALID_HANDLE, depth = BGFX_INVALID_HANDLE;
        uint16_t width = 0, height = 0;
        uint32_t depthFormat = UINT32_MAX;
        std::vector<uint8_t> pixels[3];      // what bgfx uploads from (bgfx::makeRef), reused every third frame
        int next = 0;
        uint32_t frames = 0, torn = 0, composited = 0;
        double copyMs = 0.0;
        bool lastLive = false;               // what the last ffuLayerLive said, for the switch log
    };
    Guest guests[] = { { "ff7" }, { "ff8" }, { "ff9" } };

    Guest *guestFor(const char *id)
    {
        if (!id) return nullptr;
        for (auto &guest : guests)
            if (!strncmp(id, guest.game, 3) && id[3] == '_' && strcmp(guest.game, ownGame())) return &guest;
        return nullptr;
    }

    bool mapGuest(Guest &guest)
    {
        if (guest.view) return true;
        const DWORD now = GetTickCount();
        if (guest.nextTry && (LONG)(now - guest.nextTry) < 0) return false;
        guest.nextTry = now + 1000;
        char path[64];
        layerPath(path, guest.game);
        HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;   // that game hasn't published yet
        LARGE_INTEGER size = {};
        GetFileSizeEx(file, &size);
        HANDLE map = size.QuadPart > (LONGLONG)sizeof(LayerHeader) ? CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
        CloseHandle(file);
        if (!map) return false;
        guest.view = (const uint8_t *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(map);
        if (!guest.view) return false;
        guest.size = (size_t)size.QuadPart;
        ffnx_info("FFU layer: compositing %s's layer from %s (%u bytes)\n", guest.game, path, (uint32_t)guest.size);
        return true;
    }

    // A newly published frame, if there is one: copied out under its seqlock and uploaded.
    void receive(Guest &guest, uint32_t frame)
    {
        if (guest.polledFrame == frame || !mapGuest(guest)) return;
        guest.polledFrame = frame;
        const LayerHeader *header = (const LayerHeader *)guest.view;
        const LONG seq = header->seq;
        if (header->magic != LAYER_MAGIC || header->version < LAYER_VERSION || (seq & 1) || seq == guest.lastSeq) return;
        LayerHeader copy;
        ::memcpy(&copy, header, sizeof(copy));
        const size_t pixels = (size_t)copy.width * copy.height;
        if (!copy.width || !copy.height || copy.width > 4096 || copy.height > 4096 || copy.fullWidth < copy.width
            || copy.fullHeight < copy.height || copy.depthFormat > FFU_LAYER_DEPTH_FF8 || sizeof(LayerHeader) + pixels * 8 > guest.size) return;
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        auto &buffer = guest.pixels[guest.next];
        buffer.resize(pixels * 8);
        ::memcpy(buffer.data(), guest.view + sizeof(LayerHeader), pixels * 8);
        MemoryBarrier();
        if (header->seq != seq) { ++guest.torn; return; }   // rewritten while we copied
        QueryPerformanceCounter(&end);
        guest.copyMs += (end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
        guest.lastSeq = seq;
        guest.header = copy;
        guest.arrived = end;
        const bool packed = copy.depthFormat == FFU_LAYER_DEPTH_PACKED;
        if (!bgfx::isValid(guest.colour) || guest.width != copy.width || guest.height != copy.height || guest.depthFormat != copy.depthFormat)
        {
            if (bgfx::isValid(guest.colour)) bgfx::destroy(guest.colour);
            if (bgfx::isValid(guest.depth)) bgfx::destroy(guest.depth);
            const uint64_t flags = BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
            guest.colour = bgfx::createTexture2D(copy.width, copy.height, false, 1, bgfx::TextureFormat::RGBA8, flags);
            guest.depth = bgfx::createTexture2D(copy.width, copy.height, false, 1, packed ? bgfx::TextureFormat::RGBA8 : bgfx::TextureFormat::R32F, flags);
            guest.width = (uint16_t)copy.width;
            guest.height = (uint16_t)copy.height;
            guest.depthFormat = copy.depthFormat;
            ffnx_info("FFU layer: %s frames are %ux%u (depth format %u)\n", guest.game, copy.width, copy.height, copy.depthFormat);
        }
        bgfx::updateTexture2D(guest.colour, 0, 0, 0, 0, guest.width, guest.height, bgfx::makeRef(buffer.data(), (uint32_t)(pixels * 4)));
        bgfx::updateTexture2D(guest.depth, 0, 0, 0, 0, guest.width, guest.height, bgfx::makeRef(buffer.data() + pixels * 4, (uint32_t)(pixels * 4)));
        guest.next = (guest.next + 1) % 3;
        if (++guest.frames % 120 == 0)
        {
            const double latency = copy.frequency > 0 ? (end.QuadPart - copy.stamp) * 1000.0 / copy.frequency : 0.0;
            ffnx_info("FFU layer: %u frames from %s (%s, %ux%u at %u,%u): copy %.2f ms, latency %.1f ms, %u torn, %u composited\n",
                      guest.frames, guest.game, copy.id, copy.width, copy.height, copy.cropX, copy.cropY, guest.copyMs / 120, latency, guest.torn, guest.composited);
            guest.copyMs = 0.0;
        }
    }

    // Live: published less than LIVE_SECONDS ago (the publisher's stamp, on the clock every Wine process shares,
    // so an old frame found in the file after a wake-up isn't taken for a live one) and showing something.
    bool fresh(const Guest &guest)
    {
        if (guest.lastSeq < 0 || !bgfx::isValid(guest.colour) || !guest.header.covered) return false;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const LARGE_INTEGER &published = guest.header.frequency > 0 && guest.header.stamp > 0
            ? *(const LARGE_INTEGER *)&guest.header.stamp : guest.arrived;
        const LONGLONG age = now.QuadPart - published.QuadPart;
        return age > -(LONGLONG)frequency.QuadPart && age < (LONGLONG)(LIVE_SECONDS * frequency.QuadPart);
    }

    bgfx::ProgramHandle compositeProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uniformGuest = BGFX_INVALID_HANDLE, uniformHost = BGFX_INVALID_HANDLE, uniformScale = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle samplerColour = BGFX_INVALID_HANDLE, samplerDepth = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout quadLayout;
    int compositorState = 0;   // 0 not tried, 1 ready, -1 unavailable
}

bool Renderer::ffuLayerWanted()
{
    return layerWanted();
}

// A battle frame into the layer, from its first draw (renderer.cpp draw): the bridge asks (battleWanted).
bool Renderer::ffuBattleLayerStart()
{
    if (ffuBattleLayerFrame || ffuLayerActive || !battleWanted() || !ffuLayerBegin()) return false;
    ffuBattleLayerFrame = true;
    strncpy(captured.id, "ff7_battle", sizeof(captured.id) - 1);
    captured.unitsPerR = 1.0f;
    captured.depthFormat = 0;
    return true;
}

void Renderer::ffuLayerCapture(void (*draw)(), const char *id, float unitsPerR, uint32_t depthFormat)
{
    if (!layerWanted()) return;
    ffuLayerState = internalState;
    ffuLayerProgram = backendProgram;   // FFNx's post-processing switches it before the replay
    ffuLayerDraw = draw;
    strncpy(captured.id, id ? id : "", sizeof(captured.id) - 1);
    captured.unitsPerR = unitsPerR;
    captured.depthFormat = depthFormat;
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

// The layer's target and its read-back textures, made once, at a fixed box (MAX_CROP), from the first world-map
// frame on: under Wine + MoltenVK the first touch of new GPU-visible memory stalls the whole game for about a second
// (CLAUDE.md Rule 59), and remaking them whenever the box changed size left the layer stale for a second each time:
// the host swapped in its copy, a flicker (2026-10-17).
void Renderer::ffuLayerPrepare()
{
    if (!layerWanted() && !battleWanted()) return;
    static int scale = -1;   // FFU_FF7_LAYER_SCALE=2: half resolution (tests); a battle's whole frame: half by default
    if (scale < 0) { const char *value = getenv("FFU_FF7_LAYER_SCALE"); scale = value && atoi(value) > 1 ? atoi(value) : battleWanted() ? 2 : 1; }
    const uint16_t width = framebufferWidth / scale, height = framebufferHeight / scale;
    if (bgfx::isValid(ffuLayerFrameBuffer) && width == ffuLayerWidth && height == ffuLayerHeight) return;
    for (const auto &slot : ffuLayerSlots) if (slot.pending) return;   // a resolution change: once the reads are in
    const uint16_t readWidth = std::min(cropWidth, width), readHeight = std::min(cropHeight, height);
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
        slot.read[0] = bgfx::createTexture2D(readWidth, readHeight, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        slot.read[1] = bgfx::createTexture2D(readWidth, readHeight, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        slot.colour.assign((size_t)readWidth * readHeight * 4, 0);
        slot.depth.assign((size_t)readWidth * readHeight, 0.0f);
        slot.pending = 0;
    }
    ffuLayerWidth = width;
    ffuLayerHeight = height;
    ffuLayerReadWidth = readWidth;
    ffuLayerReadHeight = readHeight;
    cropCentre[0] = cropCentre[1] = -1.0f;
    mapShared(sizeof(LayerHeader) + (size_t)readWidth * readHeight * 8);
    ffnx_info("FFU layer: %ux%u target (1/%d of %ux%u), %ux%u box read back, published as %s\n", width, height, scale,
              framebufferWidth, framebufferHeight, readWidth, readHeight, ownGame());
}

bool Renderer::ffuLayerBegin()
{
    if ((!layerWanted() && !battleWanted()) || ffuLayerActive) return false;
    ffuLayerPrepare();
    if (!bgfx::isValid(ffuLayerFrameBuffer)) return false;
    const uint16_t width = ffuLayerWidth, height = ffuLayerHeight;
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
    // The crop: centred on the airship as last seen (until then a little below the middle, where the chase
    // camera puts it), inside the target.
    const float centreX = cropCentre[0] >= 0.0f ? cropCentre[0] : ffuLayerWidth * 0.5f;
    const float centreY = cropCentre[1] >= 0.0f ? cropCentre[1] : ffuLayerHeight * 0.65f;
    slot->cropX = (uint16_t)std::clamp((int)(centreX - ffuLayerReadWidth * 0.5f), 0, ffuLayerWidth - ffuLayerReadWidth);
    slot->cropY = (uint16_t)std::clamp((int)(centreY - ffuLayerReadHeight * 0.5f), 0, ffuLayerHeight - ffuLayerReadHeight);
    const bgfx::TextureRegion colourFrom = {.handle = bgfx::getTexture(ffuLayerFrameBuffer, 0), .x = slot->cropX, .y = slot->cropY, .width = ffuLayerReadWidth, .height = ffuLayerReadHeight};
    const bgfx::TextureRegion depthFrom = {.handle = bgfx::getTexture(ffuLayerFrameBuffer, 1), .x = slot->cropX, .y = slot->cropY, .width = ffuLayerReadWidth, .height = ffuLayerReadHeight};
    bgfx::blit(backendViewId, {.handle = slot->read[0]}, colourFrom);
    bgfx::blit(backendViewId, {.handle = slot->read[1]}, depthFrom);
    bgfx::touch(backendViewId);
    slot->pending = bgfx::read({.handle = slot->read[0]}, slot->colour.data());
    bgfx::read({.handle = slot->read[1]}, slot->depth.data());
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    slot->requestedAt = now.QuadPart;
    slot->requestedFrame = ffuLastFrame;
    slot->depths[0] = gameDepthAt(internalState.d3dViewMatrix, internalState.d3dProjectionMatrix, 30000.0f);
    slot->depths[1] = gameDepthAt(internalState.d3dViewMatrix, internalState.d3dProjectionMatrix, 60000.0f);
    slotMeta[slot - ffuLayerSlots] = captured;
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
    const size_t pixels = (size_t)ffuLayerReadWidth * ffuLayerReadHeight;
    const Meta &meta = slotMeta[&slot - ffuLayerSlots];
    // Where the airship is in this crop (its alpha's box, every 2nd pixel), before publishing: an empty crop is
    // published as showing nothing, so a host draws its own copy rather than nothing.
    int minX = INT_MAX, minY = INT_MAX, maxX = -1, maxY = -1;
    uint32_t covered = 0;
    for (int y = 0; y < ffuLayerReadHeight; y += 2)
        for (int x = 0; x < ffuLayerReadWidth; x += 2)
            if (slot.colour[((size_t)y * ffuLayerReadWidth + x) * 4 + 3])
            {
                ++covered;
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
    uint8_t *base = mapShared(sizeof(LayerHeader) + pixels * 8);
    if (base)
    {
        LayerHeader *header = (LayerHeader *)base;
        InterlockedIncrement(&header->seq);   // odd: writing
        header->magic = LAYER_MAGIC;
        header->version = LAYER_VERSION;
        header->width = ffuLayerReadWidth;
        header->height = ffuLayerReadHeight;
        header->fullWidth = ffuLayerWidth;
        header->fullHeight = ffuLayerHeight;
        header->frame = slot.pending;
        header->frequency = frequency.QuadPart;
        header->depthAt30k = slot.depths[0];
        header->depthAt60k = slot.depths[1];
        header->farEnd = slot.depths[1] < slot.depths[0] ? 0.0f : 1.0f;
        header->cropX = slot.cropX;
        header->cropY = slot.cropY;
        header->depthFormat = meta.depthFormat;
        header->unitsPerR = meta.unitsPerR;
        header->nearDistance = header->farDistance = 0.0f;
        ::memcpy(header->id, meta.id, sizeof(header->id));
        header->covered = covered;
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
    if (maxX < 0)
    {
        // Nothing in the box: the next boxes start again where the chase camera puts the airship.
        cropCentre[0] = cropCentre[1] = -1.0f;
    }
    else
    {
        cropCentre[0] = slot.cropX + (minX + maxX) * 0.5f;
        cropCentre[1] = slot.cropY + (minY + maxY) * 0.5f;
    }
    if (++published % 120 == 0)
    {
        ffnx_info("FFU layer: %u readbacks of %s %ux%u (at %u,%u): request to ready %.2f ms mean (%.2f max), %.2f frames; publish %.2f ms mean (%.2f max); %u skipped (ring full); last frame %.1f%% covered; depth format %u, %.1f units per R\n",
                  published, meta.id, ffuLayerReadWidth, ffuLayerReadHeight, slot.cropX, slot.cropY, readbackTotal / 120, readbackMax, framesWaitedTotal / 120.0,
                  publishTotal / 120, publishMax, skipped, 400.0 * covered / pixels, meta.depthFormat, meta.unitsPerR);
        readbackTotal = readbackMax = publishTotal = publishMax = 0;
        framesWaitedTotal = 0;
        skipped = 0;
    }
}

// ---- Compositing ----

bool Renderer::ffuLayerLive(const char *id)
{
    static int enabled = -1;   // FFU_LAYER_COMPOSITE=0 turns compositing off (A/B tests)
    if (enabled < 0)
    {
        const char *value = getenv("FFU_LAYER_COMPOSITE");
        enabled = !(value && value[0] == '0');
        QueryPerformanceFrequency(&frequency);
    }
    Guest *guest = enabled ? guestFor(id) : nullptr;
    if (!guest) return false;
    receive(*guest, ffuLastFrame);
    // Every change between the live layer and the host's copy is logged, with why: a flicker between the two is
    // found from these lines (2026-10-17, Richard saw flicker and a doubled airship).
    const char *why = !fresh(*guest) ? (guest->lastSeq < 0 ? "no frame yet" : !guest->header.covered ? "nothing shown" : "stale")
                    : strncmp(guest->header.id, id, sizeof(guest->header.id)) ? "another model" : guest->header.unitsPerR <= 0.0f ? "no scale" : nullptr;
    const bool live = why == nullptr;
    if (live != guest->lastLive)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double age = guest->header.frequency > 0 ? (now.QuadPart - guest->header.stamp) * 1000.0 / guest->header.frequency : -1.0;
        ffnx_info("FFU layer: %s %s (frame %u, last %s frame %.0f ms old)\n", id, live ? "live" : "copy", ffuLastFrame,
                  guest->game, age);
        if (!live) ffnx_info("FFU layer: ... because %s\n", why);
        guest->lastLive = live;
    }
    if (!live) return false;
    // Live only when it can be drawn: the caller then skips its copy.
    if (compositorState == 0)
    {
        compositorState = -1;
        const char *vertex = "shaders/FFNx.ffulayer.flat.vk.vert", *fragment = "shaders/FFNx.ffulayer.flat.vk.frag";
        char vertexPath[MAX_PATH], fragmentPath[MAX_PATH];
        _snprintf(vertexPath, sizeof(vertexPath), "%s/%s", basedir, vertex);
        _snprintf(fragmentPath, sizeof(fragmentPath), "%s/%s", basedir, fragment);
        if (bgfx::getCaps()->rendererType != bgfx::RendererType::Vulkan || !fileExists(vertexPath) || !fileExists(fragmentPath))
            ffnx_error("FFU layer: no compositor (needs Vulkan and %s): visitors stay drawn as copies\n", vertex);
        else
        {
            compositeProgram = bgfx::createProgram(getShader(vertex), getShader(fragment), true);
            uniformGuest = bgfx::createUniform("u_ffuLayerGuest", bgfx::UniformType::Vec4);
            uniformHost = bgfx::createUniform("u_ffuLayerHost", bgfx::UniformType::Vec4);
            uniformScale = bgfx::createUniform("u_ffuLayerScale", bgfx::UniformType::Vec4);
            samplerColour = bgfx::createUniform("s_ffuColour", bgfx::UniformType::Sampler);
            samplerDepth = bgfx::createUniform("s_ffuDepth", bgfx::UniformType::Sampler);
            quadLayout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float).end();
            compositorState = bgfx::isValid(compositeProgram) ? 1 : -1;
            ffnx_info("FFU layer: compositor %s\n", compositorState > 0 ? "ready" : "failed");
        }
    }
    return compositorState > 0;
}

void Renderer::ffuLayerComposite(const char *id, uint32_t hostDepthFormat, float hostUnitsPerR, float hostXScale)
{
    Guest *guest = guestFor(id);
    if (!guest || compositorState <= 0 || !fresh(*guest) || hostUnitsPerR <= 0.0f || guest->header.unitsPerR <= 0.0f) return;
    if (bgfx::getAvailTransientVertexBuffer(6, quadLayout) < 6) return;
    const LayerHeader &h = guest->header;
    // Its crop's place in the picture: both games draw the same flight camera with the same 30-degree vertical
    // lens over their own frames (docs/space.md "Lens"), so a direction keeps its height on screen and its sideways
    // place scales with the frames' aspects (FF7 and FF8 draw at 1.600, FF9 at 1.547), times the host's own
    // horizontal stretch (FF8 on the ground; the layers themselves are always square).
    const float aspects = ((float)h.fullWidth / h.fullHeight) / ((float)framebufferWidth / framebufferHeight);
    const float across = aspects * hostXScale;
    const float left = (2.0f * h.cropX / h.fullWidth - 1.0f) * across, right = (2.0f * (h.cropX + h.width) / h.fullWidth - 1.0f) * across;
    const float top = 1.0f - 2.0f * h.cropY / h.fullHeight, bottom = 1.0f - 2.0f * (h.cropY + h.height) / h.fullHeight;
    bgfx::TransientVertexBuffer vertices;
    bgfx::allocTransientVertexBuffer(&vertices, 6, quadLayout);
    struct Vertex { float x, y, z, u, v; };
    const Vertex corners[6] = {
        { left, top, 0.5f, 0.0f, 0.0f }, { right, top, 0.5f, 1.0f, 0.0f }, { right, bottom, 0.5f, 1.0f, 1.0f },
        { left, top, 0.5f, 0.0f, 0.0f }, { right, bottom, 0.5f, 1.0f, 1.0f }, { left, bottom, 0.5f, 0.0f, 1.0f },
    };
    ::memcpy(vertices.data, corners, sizeof(corners));
    float guestParams[4] = { (float)h.depthFormat, 0.0f, 0.0f, 0.0f };
    if (h.depthFormat == FFU_LAYER_DEPTH_FF7) { guestParams[1] = h.depthAt30k; guestParams[2] = h.depthAt60k; guestParams[3] = h.farEnd; }
    else if (h.depthFormat == FFU_LAYER_DEPTH_PACKED) { guestParams[1] = h.nearDistance; guestParams[2] = h.farDistance; }
    float hostParams[4] = { (float)hostDepthFormat, 0.0f, 0.0f, 0.0f };
    if (hostDepthFormat == FFU_LAYER_DEPTH_FF7)
    {
        hostParams[1] = gameDepthAt(internalState.d3dViewMatrix, internalState.d3dProjectionMatrix, 30000.0f);
        hostParams[2] = gameDepthAt(internalState.d3dViewMatrix, internalState.d3dProjectionMatrix, 60000.0f);
        hostParams[3] = hostParams[2] < hostParams[1] ? 0.0f : 1.0f;
    }
    const float scale[4] = { h.unitsPerR, hostUnitsPerR, 0.0f, 0.0f };
    bgfx::setUniform(uniformGuest, guestParams);
    bgfx::setUniform(uniformHost, hostParams);
    bgfx::setUniform(uniformScale, scale);
    bgfx::setTexture(0, samplerColour, guest->colour);
    bgfx::setTexture(1, samplerDepth, guest->depth);
    bgfx::setVertexBuffer(0, &vertices);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LEQUAL);
    bgfx::submit(backendViewId, compositeProgram);
    ++guest->composited;
}
