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

#include "../../globals.h"
#include "../../renderer.h"
#include "camera.h"
#include "ffu.h"

extern "C" __declspec(dllexport) ffu::ff7::Ffnx ffu_ff7_ffnx = {};

namespace ffu::ff7
{
    const Control *control()
    {
        static const Control *found = nullptr;
        static DWORD lastLook = 0;
        if (found) return found;
        DWORD now = GetTickCount();
        if (lastLook && now - lastLook < 1000) return nullptr;   // look again once a second
        lastLook = now;
        HMODULE bridge = GetModuleHandleA("dinput8.dll");
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
    }
}
