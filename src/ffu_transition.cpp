/****************************************************************************/
//    Final Fantasy Universe: FF8's battles loaded behind the hub's swirl and held at their first lit frame until shown.
/****************************************************************************/

// Richard 2026-10-23: "It should swirl, very quickly with no load and absolutely no fade in from black ... If there has to
// be a load it should be hidden behind the swirl." (docs/battle-look.md "Zero-load battle transitions"; research
// docs/research/battle-transition-ff8-2026-10-23.md.) FF8's bridge arms this for a battle the hub cuts to, FF8's window
// hidden (ffu_ff8_battle_quick(1), at the cut):
// - FF8's own swirl (hidden: the hub draws its own over its own picture) ends after its first frame: its frame counter
//   (0x204E2E8, an int, read as the next frame's argument) is set past both styles' ends (`jl` 0x48 at 0x559847, 0x52 at
//   0x559D6F), so its next frame clears and queues the battle;
// - the frames run unpaced until the hold (ff8_limit_fps returns at once): the battle's file waits count frames (about
//   20 KiB a frame, the PlayStation's CD speed), so it loads as fast as frames come, and nothing of a battle is timed by
//   frames before its menus (step 3);
// - its fade in from black, its first FADE_FRAMES frames of step 2 (0x1D27B04: 0x47DB90's 0xD task, 20 frames), runs
//   hidden too, and it holds on the frame after, pumping its messages (the bridge's show is carried out on this
//   thread), until its window shows: the first frame seen is lit, the battle's opening camera going on from there.
//   Never held longer than HOLD_MOST_MS, never armed longer than ARMED_MOST_MS.
// ffu_ff8_battle_held() says when it holds (1: the bridge's `cut_ready` then), is still loading (0), or is not armed
// (-1: never armed, let go, or given up: the bridge's own readiness then).
// The own battle engine (docs/battle-engine.md; ffu_ff8_battle_quick(2)): FF8's window never comes in front; the engine
// draws the battle's opening itself from the cut, so FF8 isn't held: it runs unpaced through its opening too until its
// opening frame (step 2's frames) reaches the engine's (the bridge's ffu_ff8_own_opening_frame), then paced, in step.

#include <windows.h>
#include <atomic>
#include <cstdint>

#include "common.h"
#include "globals.h"
#include "log.h"

namespace
{
    constexpr uintptr_t SWIRL_COUNTER = 0x204E2E8;
    constexpr int SWIRL_PAST_END = 0x52;
    constexpr uintptr_t BATTLE_STEP = 0x1D27B04;
    constexpr int FADE_FRAMES = 21;   // (its 20-frame fade, and one more: the held frame is lit)
    constexpr DWORD HOLD_MOST_MS = 8000;
    constexpr DWORD ARMED_MOST_MS = 12000;

    std::atomic<int> armed{0}, held{0};
    int mode;            // 1: held at its first lit frame until shown; 2: held at its opening's first frame until the own
                         // battle engine's opening begins (or its window shows), then caught up with it, never held after
    DWORD armedAt, battleFrom;
    int fadeSeen, unpaced;
    bool swirlCut, battleSeen;

    bool shown()
    {
        return gameHwnd && IsWindowVisible(gameHwnd) && !IsIconic(gameHwnd);
    }

    // The bridge's export: the own battle engine's opening frame now (-1: none yet).
    int ownOpeningFrame()
    {
        static int (*opening)() = nullptr;
        static DWORD lastLook = 0;
        if (!opening && GetTickCount() - lastLook >= 1000)
        {
            lastLook = GetTickCount();
            HMODULE bridge = GetModuleHandleA("dinput.dll");
            opening = bridge ? (int (*)())(void *)GetProcAddress(bridge, "ffu_ff8_own_opening_frame") : nullptr;
        }
        return opening ? opening() : -1;
    }

    void disarm(const char *why)
    {
        if (!armed.exchange(0)) return;
        ffnx_info("FFU transition: %s (%d frames unpaced, %u ms after the cut)\n", why, unpaced, unsigned(GetTickCount() - armedAt));
    }

    bool released()
    {
        return shown() || (mode == 2 && ownOpeningFrame() >= 0);
    }

    // Held on FF8's own thread until its window shows (mode 2: or the own battle engine's opening begins, the bridge
    // hearing the hub on a thread of its own): its messages pumped (a WM_QUIT put back for its own loop).
    void hold()
    {
        held = 1;
        const DWORD from = GetTickCount();
        ffnx_info("FFU transition: held at its first lit frame, %u ms after the cut (%u ms after the battle began)\n",
                  unsigned(from - armedAt), unsigned(from - battleFrom));
        MSG message;
        bool quit = false;
        while (!released() && !quit && GetTickCount() - from < HOLD_MOST_MS)
        {
            while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    PostQuitMessage(int(message.wParam));
                    quit = true;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
            if (!quit) Sleep(1);
        }
        held = 0;
        ffnx_info("FFU transition: let go after %u ms (%s)\n", unsigned(GetTickCount() - from),
                  shown() ? "its window shown" : mode == 2 && ownOpeningFrame() >= 0 ? "the engine's opening began"
                  : quit ? "the game quits" : "never shown: held long enough");
    }
}

extern "C" __declspec(dllexport) void ffu_ff8_battle_quick(int on)
{
    if (!ff8) return;
    if (!on)
    {
        disarm("disarmed by the bridge");
        return;
    }
    armedAt = GetTickCount();
    fadeSeen = unpaced = 0;
    swirlCut = battleSeen = false;
    mode = on == 2 ? 2 : 1;
    armed = 1;
    ffnx_info("FFU transition: armed for this battle (%s)\n", mode == 2 ? "the own battle engine's: caught up, never held" : "held until shown");
}

extern "C" __declspec(dllexport) int ffu_ff8_battle_held()
{
    return held.load() ? 1 : armed.load() ? 0 : -1;
}

// Once a frame at the flip (common_flip, FF8's game thread): its swirl's counter put past the end on its first frame.
void ffuTransitionFrame()
{
    if (!ff8 || !armed) return;
    if (GetTickCount() - armedAt > ARMED_MOST_MS)
    {
        disarm("no battle held in time");
        return;
    }
    const int driverMode = getmode_cached()->driver_mode;
    if (driverMode == MODE_SWIRL && !swirlCut)
    {
        swirlCut = true;
        int *counter = (int *)SWIRL_COUNTER;
        ffnx_info("FFU transition: its own swirl ended at its frame %d\n", *counter);
        if (*counter < SWIRL_PAST_END) *counter = SWIRL_PAST_END;
    }
    else if (driverMode == MODE_BATTLE && !battleSeen)
    {
        battleSeen = true;
        battleFrom = GetTickCount();
    }
    else if (driverMode != MODE_SWIRL && driverMode != MODE_BATTLE && battleSeen)
        disarm("the battle ended before it was held");
}

// From ff8_limit_fps (its frame's end, after the flip): true while the frame runs unpaced; holds at its first lit frame.
bool ffuTransitionLimit()
{
    if (!ff8 || !armed) return false;
    if (shown())
    {
        disarm("its window shown before the hold");
        return false;
    }
    const int driverMode = getmode_cached()->driver_mode;   // (not `mode`: this transition's own)
    if (driverMode == MODE_BATTLE)
    {
        const uint32_t step = *(volatile uint32_t *)BATTLE_STEP;
        if (step > 2)
        {
            disarm("past its opening before the hold");
            return false;
        }
        if (step == 2 && mode == 2)
        {
            int target = ownOpeningFrame();
            if (target < 0)   // its opening's first frame drawn before the engine's began (the swirl still up): waits
            {
                hold();
                target = ownOpeningFrame();
                if (target < 0)
                {
                    disarm(shown() ? "its window shown (the engine gave its battle back)" : "the engine's opening never came");
                    return false;
                }
            }
            if (++fadeSeen >= target)
            {
                disarm("caught up with the engine's opening");
                ffnx_info("FFU transition: at its opening frame %d, the engine's %d, %u ms after the cut\n", fadeSeen, target,
                          unsigned(GetTickCount() - armedAt));
                return false;
            }
        }
        else if (step == 2 && ++fadeSeen > FADE_FRAMES)
        {
            hold();
            disarm("held and let go");
            return false;
        }
    }
    else if (driverMode != MODE_SWIRL)
        return false;   // (the world map's last frames before its swirl: paced as ever)
    unpaced++;
    return true;
}
