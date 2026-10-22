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

//
// Mixed parties, a visitor's own sounds in another game's battle (docs/shared-engine.md "their own sounds"). As the
// visitor's game (its hidden battle, muted), every sound effect and voice it plays is noted while its bridge records
// (`ffu_audio_sfx_record`), its file as FFNx resolved it (a shuffle or a sequence already picked), and handed over
// (`ffu_audio_sfx_take`): the bridge tags each with the key of the visitor's move it came on. As the host's game,
// `ffu_audio_play_file` plays such a file (any game's, by its Unix path through Z:) as its replay of the move reaches that
// key, through this engine's own sound-effect volume (its silence and pause apply).
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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

    struct Noted { char path[MAX_PATH]; float volume, pan; };
    constexpr int MOST_NOTED = 64;               // a move's sounds between two takes (a battle frame): plenty
    std::mutex notedLock;
    Noted noted[MOST_NOTED];
    int notedCount = 0;
    std::atomic<int> recording{0};

    struct PlayRequest { std::string path; float volume, pan; };
    constexpr size_t MOST_QUEUED = 32;
    std::mutex playLock;
    std::vector<PlayRequest> playQueue;
}

// 1: note every sound this engine plays from now (the bridge's hidden battle for its visitor); 0: stop, and forget them.
extern "C" __declspec(dllexport) void ffu_audio_sfx_record(int on)
{
    recording.store(on ? 1 : 0, std::memory_order_release);
    if (!on) {
        std::lock_guard<std::mutex> guard(notedLock);
        notedCount = 0;
    }
}

// The sounds noted since the last take, oldest first, as lines "path<TAB>volume<TAB>pan\n" in `out` (always ended); the
// number taken. Any thread.
extern "C" __declspec(dllexport) int ffu_audio_sfx_take(char *out, int size)
{
    if (!out || size <= 0) return 0;
    out[0] = 0;
    std::lock_guard<std::mutex> guard(notedLock);
    int at = 0, taken = 0;
    for (; taken < notedCount; taken++) {
        int wrote = _snprintf(out + at, size - at, "%s\t%.3f\t%.3f\n", noted[taken].path, noted[taken].volume, noted[taken].pan);
        if (wrote < 0 || wrote >= size - at) break;   // (no room: the rest stay for the next take)
        at += wrote;
    }
    out[at < size ? at : size - 1] = 0;
    for (int k = taken; k < notedCount; k++) noted[k - taken] = noted[k];
    notedCount -= taken;
    return taken;
}

// Play `path` (an ogg or anything vgmstream reads) at `volume` (0..1, before this engine's sound-effect volume) and `pan`
// (-1..1). Any thread; played on the game thread at its next frame.
extern "C" __declspec(dllexport) void ffu_audio_play_file(const char *path, float volume, float pan)
{
    if (!path || !path[0]) return;
    std::lock_guard<std::mutex> guard(playLock);
    if (playQueue.size() < MOST_QUEUED)
        playQueue.push_back({path, std::clamp(volume, 0.0f, 1.0f), std::clamp(pan, -1.0f, 1.0f)});
}

// Game thread, as a sound effect (layer SFX: `key` its loaded key) or voice (layer -1: `key` its file) starts.
void NxAudioEngine::ffuNote(const char *key, NxAudioEngineLayer layer, float volume, float pan)
{
    if (!recording.load(std::memory_order_acquire) || !key || !key[0]) return;
    Noted entry;
    if (layer == NxAudioEngineLayer::NXAUDIOENGINE_SFX) {
        if (!getFilenameFullPath(entry.path, key, layer)) return;
    } else {
        strncpy(entry.path, key, MAX_PATH - 1);
        entry.path[MAX_PATH - 1] = 0;
    }
    entry.volume = volume;
    entry.pan = pan;
    std::lock_guard<std::mutex> guard(notedLock);
    if (notedCount == MOST_NOTED) {   // (full: the oldest goes)
        for (int k = 1; k < MOST_NOTED; k++) noted[k - 1] = noted[k];
        notedCount--;
    }
    noted[notedCount++] = entry;
}

// Game thread, each frame: the queued files played; the finished ones freed.
void NxAudioEngine::ffuPlayFiles()
{
    std::vector<PlayRequest> requests;
    {
        std::lock_guard<std::mutex> guard(playLock);
        requests.swap(playQueue);
    }
    for (const PlayRequest &request : requests) {
        SoLoud::VGMStream *stream = new SoLoud::VGMStream();
        SoLoud::result res = stream->load(request.path.c_str());
        if (res != SoLoud::SO_NO_ERROR) {
            ffnx_error("FFU audio: a visitor's sound %s couldn't be read (SoLoud error %u)\n", request.path.c_str(), res);
            delete stream;
            continue;
        }
        _ffuPlaying.push_back({stream, _engine.play(*stream, request.volume * getSFXMasterVolume(), request.pan)});
    }
    for (size_t k = 0; k < _ffuPlaying.size();) {
        if (_engine.isValidVoiceHandle(_ffuPlaying[k].second)) { k++; continue; }
        delete _ffuPlaying[k].first;
        _ffuPlaying.erase(_ffuPlaying.begin() + k);
    }
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

    ffuPlayFiles();

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
