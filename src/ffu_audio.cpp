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

// Final Fantasy Universe: audio that survives the hub's freezes, and the hub's music control.
//
// The hub freezes a game you aren't in (SIGSTOP). miniaudio's WASAPI loop waits at most 5 s for the device's
// next period (MA_WASAPI_WAIT_TIMEOUT_MILLISECONDS) and treats a longer wait as a dead device: the wait's
// timeout is kept by wineserver, which keeps running, so any freeze over 5 s ends the loop and the device
// stays stopped for good. Every frame we restart it when it has stopped on its own.
//
// The hub mutes a game's music while another game's airship theme plays (CLAUDE.md Rule 66), through
// `ffu_audio_music` from the bridge thread; the request is applied on the game thread, which owns the engine.
//
// The hub keeps every game silent while its start screen is up (Richard: "all three games must be fully muted"):
// launched with FFU_SILENT=1, the engine starts with its global volume at 0 (music, sound effects, voices, ambient,
// movies: everything FFNx plays), and `ffu_audio_silence(0)` from the bridge gives it back once the menu has gone.

#include <windows.h>
#include <atomic>
#include <algorithm>
#include <cstring>

#include "audio.h"
#include "log.h"

// SoLoud's miniaudio backend (soloud_miniaudio.cpp) keeps its one playback device in this global.
struct ma_device;
namespace SoLoud { extern ma_device gDevice; }
extern "C" int ma_device_start(ma_device *pDevice);
extern "C" int ma_device_get_state(const ma_device *pDevice);

namespace
{
    constexpr int MA_DEVICE_STATE_STOPPED = 1;   // ma_device_state_stopped
    constexpr DWORD RESTART_INTERVAL_MS = 500;

    std::atomic<uint32_t> requestedGainBits{0};  // float bits of the latest requested gain
    std::atomic<uint32_t> requestedFadeMs{0};
    std::atomic<uint32_t> requestSequence{0};
    std::atomic<int> requestedSilence{-1};       // -1 nothing new, 0 its sound back, 1 silent
    uint32_t appliedSequence = 0;
    DWORD lastRestart = 0;
}

extern "C" __declspec(dllexport) volatile LONG ffu_audio_restarts = 0;   // device restarts so far (tests read it)

// gain 0..1 on top of the game's own music volume, faded over `seconds`.
extern "C" __declspec(dllexport) void ffu_audio_music(float gain, float seconds)
{
    gain = std::clamp(gain, 0.0f, 1.0f);
    uint32_t bits;
    static_assert(sizeof bits == sizeof gain);
    memcpy(&bits, &gain, sizeof bits);
    requestedGainBits.store(bits, std::memory_order_relaxed);
    requestedFadeMs.store((uint32_t)(std::clamp(seconds, 0.0f, 60.0f) * 1000.0f + 0.5f), std::memory_order_relaxed);
    requestSequence.fetch_add(1, std::memory_order_release);
}

// 1: every sound of the engine silent (the hub's start screen); 0: back. Any thread; applied on the game thread.
extern "C" __declspec(dllexport) void ffu_audio_silence(int on)
{
    requestedSilence.store(on ? 1 : 0, std::memory_order_release);
}

// As the engine starts: silent from the very first sound when the hub launched this game behind its start screen.
void NxAudioEngine::ffuStartSilence()
{
    char value[4] = "";
    if (GetEnvironmentVariableA("FFU_SILENT", value, sizeof value) && value[0] == '1') {
        _ffuSilenced = true;
        _engine.setGlobalVolume(0.0f);
        ffnx_info("FFU audio: silent from the start (FFU_SILENT=1)\n");
    }
}

void NxAudioEngine::setFfuMusicGain(float gain, double time)
{
    _ffuMusicGain = std::clamp(gain, 0.0f, 1.0f);
    resetMusicVolume(time);
}

// Once a frame, on the game thread (common_flip).
void NxAudioEngine::ffuFrame()
{
    if (!_engineInitialized) return;

    int silence = requestedSilence.exchange(-1, std::memory_order_acquire);
    if (silence >= 0 && (silence == 1) != _ffuSilenced) {
        _ffuSilenced = silence == 1;
        _engine.setGlobalVolume(_ffuSilenced ? 0.0f : 1.0f);
        ffnx_info("FFU audio: %s\n", _ffuSilenced ? "silent (the hub's start screen)" : "its sound back");
    }

    uint32_t sequence = requestSequence.load(std::memory_order_acquire);
    if (sequence != appliedSequence) {
        appliedSequence = sequence;
        uint32_t bits = requestedGainBits.load(std::memory_order_relaxed);
        float gain;
        memcpy(&gain, &bits, sizeof gain);
        double fade = requestedFadeMs.load(std::memory_order_relaxed) / 1000.0;
        ffnx_info("FFU audio: music gain %.2f over %.2f s\n", gain, fade);
        setFfuMusicGain(gain, fade);
    }

    if (ma_device_get_state(&SoLoud::gDevice) == MA_DEVICE_STATE_STOPPED) {
        DWORD now = GetTickCount();
        if (lastRestart != 0 && now - lastRestart < RESTART_INTERVAL_MS) return;
        lastRestart = now;
        int result = ma_device_start(&SoLoud::gDevice);
        LONG restarts = InterlockedIncrement(&ffu_audio_restarts);
        if (result == 0 || restarts <= 5 || restarts % 100 == 0)   // a device that keeps failing: not every 0.5 s
            ffnx_info("FFU audio: the device had stopped (a freeze over 5 s); restart %ld, result %d\n", restarts, result);
    }
}
