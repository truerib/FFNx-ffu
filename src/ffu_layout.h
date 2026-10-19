#pragma once
#include <windows.h>

// The Final Fantasy Universe's "Unified controls" (its pad DLL mods/ffu-pad, export ffu_pad_layout): 1 while one
// PlayStation layout is in use, for which the pad DLL swaps FF7's Cross and Circle and FF8's Triangle and Circle; the
// button prompts follow the same swap. 0 (or no such DLL): each game's own layout.
inline DWORD ffu_pad_layout()
{
  static FARPROC layout = nullptr;
  if (!layout)
  {
    HMODULE pad = GetModuleHandleA("xinput9_1_0.dll");
    if (pad) layout = GetProcAddress(pad, "ffu_pad_layout");
    if (!layout) return 0;
  }
  return ((DWORD(WINAPI*)())layout)();
}
