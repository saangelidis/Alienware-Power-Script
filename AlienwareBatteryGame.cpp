// ============================================================================
//  AlienwareBatteryGame.cpp
//
//  A do-nothing "game" stand-in. It has no window, no CPU/GPU load, and
//  does nothing except exist as a running process — which is all Alienware
//  Command Center needs to trigger its per-game Lighting/Thermal profile.
//
//  SETUP (one-time, manual, inside AWCC):
//    1. Build this file to AlienwareBatteryGame.exe (see build command below).
//    2. Put it somewhere permanent, e.g. next to AlienwarePowerScript.exe.
//    3. Open Alienware Command Center -> Games tab -> "Add Game" (or the
//       '+' icon) -> browse to AlienwareBatteryGame.exe and add it.
//    4. AWCC will create a profile entry for it. Open that profile's
//       Lighting tab and set every zone to "Off" / no color (and set
//       Thermal to whatever low-power profile you want).
//    5. Done. From then on, AWCC will automatically apply that lighting
//       profile any time this .exe is running, and revert to your default
//       profile the moment it exits — no further AWCC interaction needed.
//
//  AlienwarePowerScript.exe launches this on unplug and terminates it on
//  replug (see LaunchBatteryGame / KillBatteryGame in the main script).
//
//  BUILD (MinGW-w64 / g++):
//    g++ -O2 -municode -mwindows -o AlienwareBatteryGame.exe AlienwareBatteryGame.cpp
//
//  BUILD (MSVC):
//    cl /O2 /EHsc /DUNICODE /D_UNICODE AlienwareBatteryGame.cpp /link /SUBSYSTEM:WINDOWS
// ============================================================================

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Prevent piling up duplicate instances if the power script ever
    // double-launches it.
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\AlienwareBatteryGame_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    // No window, no timers, no work. Just idle until killed.
    // A wait on an event that's never signaled costs effectively zero CPU.
    HANDLE hNever = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WaitForSingleObject(hNever, INFINITE);

    if (hMutex)
    {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return 0;
}
