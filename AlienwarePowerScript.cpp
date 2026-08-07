// ============================================================================
//  AlienwarePowerScript.cpp
//
//  Watches AC/battery status on Windows and automatically applies a
//  battery-friendly or performance profile:
//
//    UNPLUGGED  -> Energy Saver threshold forced on, RGB decoy-game launch,
//                  refresh rate locked to 60Hz, brightness -> 0%
//
//    PLUGGED IN -> Energy Saver threshold restored, refresh rate maxed
//
//  A balloon notification is shown on every transition.
//
//  BUILD (MinGW-w64 / g++):
//    g++ -O2 -municode -mwindows -o AlienwarePowerScript.exe AlienwarePowerScript.cpp -luser32 -lgdi32 -lole32 -loleaut32 -lwbemuuid -lshell32
//
//  BUILD (MSVC "x64 Native Tools" prompt):
//    cl /O2 /EHsc /DUNICODE /D_UNICODE AlienwarePowerScript.cpp /link user32.lib gdi32.lib ole32.lib oleaut32.lib wbemuuid.lib shell32.lib /SUBSYSTEM:WINDOWS
//
//  NOTES:
//    - Run once "as Administrator" the first time; some powercfg writes
//      to the active scheme can silently no-op without it.
//    - See README.md for the RGB / Energy Saver caveats.
// ============================================================================

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <powersetting.h>
#include <wbemidl.h>
#include <comdef.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")

// ---------------------------------------------------------------------------
// Windows Energy Saver automatic battery threshold.
//
// Use PowerCfg's documented aliases instead of hard-coded GUIDs:
//   SUB_ENERGYSAVER  = Energy Saver settings subgroup
//   ESBATTTHRESHOLD  = battery percentage at which Energy Saver turns on
//
// On battery we temporarily set the threshold to 100%, so Energy Saver
// engages immediately after AC is unplugged. When AC returns, restore 50%.
// ---------------------------------------------------------------------------

static const wchar_t* APP_NAME  = L"AlienwarePowerScript";
static const UINT     WM_TRAYICON = WM_APP + 1;
static const UINT     ID_TRAY_EXIT = 1001;

static NOTIFYICONDATAW g_nid = {};
static HWND  g_hwnd = nullptr;
static HICON g_hIcon = nullptr;
static bool  g_haveLastState = false;
static bool  g_lastOnAC = true;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void ApplyBatteryMode();
void ApplyPerformanceMode();
void ShowToast(const std::wstring& title, const std::wstring& msg);
void SetRefreshRate(bool maxOut, int fallbackHz);
bool SetBrightnessWMI(BYTE percent);
void LaunchBatteryGame();
void KillBatteryGame();
bool RunPowercfg(const std::wstring& args);
void HandlePowerTransition();
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void CreateTrayIcon(HWND hwnd);
void RemoveTrayIcon();

// ---------------------------------------------------------------------------
// Helper: launch powercfg.exe with the given argument string and wait for it.
// Used only for the Energy Saver threshold trick — there is no public
// Win32 API to toggle Energy Saver directly.
// ---------------------------------------------------------------------------
bool RunPowercfg(const std::wstring& args)
{
    wchar_t cmdline[512];
    swprintf(cmdline, 512, L"powercfg.exe %ls", args.c_str());

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessW(
        nullptr, cmdline, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    if (!ok) return false;

    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// ---------------------------------------------------------------------------
// Refresh rate: enumerate available modes for the primary display and pick
// either the maximum available frequency, or a specific fallback (e.g. 60Hz).
// ---------------------------------------------------------------------------
void SetRefreshRate(bool maxOut, int fallbackHz)
{
    DEVMODEW current = {};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current))
        return;

    int targetHz = fallbackHz;

    if (maxOut)
    {
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        int modeIndex = 0;
        int bestHz = current.dmDisplayFrequency;

        while (EnumDisplaySettingsW(nullptr, modeIndex, &dm))
        {
            // Only consider modes matching the current resolution/color depth
            if (dm.dmPelsWidth == current.dmPelsWidth &&
                dm.dmPelsHeight == current.dmPelsHeight &&
                dm.dmBitsPerPel == current.dmBitsPerPel)
            {
                if (dm.dmDisplayFrequency > bestHz)
                    bestHz = dm.dmDisplayFrequency;
            }
            modeIndex++;
        }
        targetHz = bestHz;
    }

    if (targetHz == current.dmDisplayFrequency)
        return; // already at target, avoid an unnecessary mode switch

    DEVMODEW dmSet = current;
    dmSet.dmDisplayFrequency = targetHz;
    dmSet.dmFields = DM_DISPLAYFREQUENCY;

    ChangeDisplaySettingsExW(nullptr, &dmSet, nullptr, CDS_UPDATEREGISTRY, nullptr);
}

// ---------------------------------------------------------------------------
// Brightness via WMI (root\WMI, WmiMonitorBrightnessMethods::WmiSetBrightness)
// Works for most internal laptop panels on a driver that exposes the
// standard ACPI brightness interface. Some vendor GPU/driver combos ignore it.
// ---------------------------------------------------------------------------
bool SetBrightnessWMI(BYTE percent)
{
    bool comInitializedHere = false;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == S_OK) comInitializedHere = true;
    else if (hr != S_FALSE && hr != RPC_E_CHANGED_MODE)
        return false;

    bool success = false;
    IWbemLocator* pLoc = nullptr;
    IWbemServices* pSvc = nullptr;
    IEnumWbemClassObject* pEnum = nullptr;

    hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                           IID_IWbemLocator, (LPVOID*)&pLoc);
    if (FAILED(hr)) goto cleanup;

    hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
                              0, nullptr, nullptr, &pSvc);
    if (FAILED(hr)) goto cleanup;

    hr = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                            nullptr, EOAC_NONE);
    if (FAILED(hr)) goto cleanup;

    hr = pSvc->CreateInstanceEnum(_bstr_t(L"WmiMonitorBrightnessMethods"),
                                   WBEM_FLAG_FORWARD_ONLY, nullptr, &pEnum);
    if (FAILED(hr)) goto cleanup;

    {
        IWbemClassObject* pInst = nullptr;
        ULONG returned = 0;
        while (pEnum->Next(WBEM_INFINITE, 1, &pInst, &returned) == S_OK && returned)
        {
            VARIANT vPath;
            VariantInit(&vPath);
            pInst->Get(L"__PATH", 0, &vPath, nullptr, nullptr);

            IWbemClassObject* pClass = nullptr;
            IWbemClassObject* pInParamsDef = nullptr;
            IWbemClassObject* pInParams = nullptr;

            if (SUCCEEDED(pSvc->GetObject(_bstr_t(L"WmiMonitorBrightnessMethods"), 0, nullptr, &pClass, nullptr)) &&
                SUCCEEDED(pClass->GetMethod(L"WmiSetBrightness", 0, &pInParamsDef, nullptr)) &&
                SUCCEEDED(pInParamsDef->SpawnInstance(0, &pInParams)))
            {
                VARIANT vTimeout, vBrightness;
                VariantInit(&vTimeout);
                VariantInit(&vBrightness);
                vTimeout.vt = VT_UI4;      vTimeout.ulVal = 0;
                vBrightness.vt = VT_UI1;   vBrightness.bVal = percent;

                pInParams->Put(L"Timeout", 0, &vTimeout, 0);
                pInParams->Put(L"Brightness", 0, &vBrightness, 0);

                IWbemClassObject* pOutParams = nullptr;
                hr = pSvc->ExecMethod(vPath.bstrVal, _bstr_t(L"WmiSetBrightness"),
                                       0, nullptr, pInParams, &pOutParams, nullptr);
                if (SUCCEEDED(hr)) success = true;

                if (pOutParams) pOutParams->Release();
                VariantClear(&vTimeout);
                VariantClear(&vBrightness);
            }

            if (pInParams) pInParams->Release();
            if (pInParamsDef) pInParamsDef->Release();
            if (pClass) pClass->Release();
            VariantClear(&vPath);
            pInst->Release();
        }
    }

cleanup:
    if (pEnum) pEnum->Release();
    if (pSvc) pSvc->Release();
    if (pLoc) pLoc->Release();
    if (comInitializedHere) CoUninitialize();
    return success;
}

// ---------------------------------------------------------------------------
// RGB keyboard control via the AWCC "per-game profile" trick.
//
// Alienware Command Center lets you manually add any .exe to its Game
// Library and assign it a Lighting/Thermal profile. AWCC then watches for
// that process and swaps the profile in automatically whenever it's
// running, reverting the instant it exits. That's a real, supported
// mechanism — so instead of fighting the Fn+F7 hotkey, we drive it by
// launching/killing a do-nothing decoy "game" (AlienwareBatteryGame.exe).
//
// One-time manual setup required (see README.md):
//   1. Build AlienwareBatteryGame.cpp alongside this app.
//   2. In AWCC -> Games -> Add Game, browse to AlienwareBatteryGame.exe.
//   3. Set that game's Lighting profile to "Off" / no color.
//
// After that, this script just needs to start/stop the process.
// ---------------------------------------------------------------------------
static const wchar_t* BATTERY_GAME_EXE = L"AlienwareBatteryGame.exe";

std::wstring GetBatteryGamePath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring path(exePath);
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"" : path.substr(0, slash + 1);
    return dir + BATTERY_GAME_EXE;
}

bool IsBatteryGameRunning()
{
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, BATTERY_GAME_EXE) == 0)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

void LaunchBatteryGame()
{
    if (IsBatteryGameRunning())
        return;

    std::wstring path = GetBatteryGamePath();

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    // CreateProcessW may modify the command line buffer, so use a mutable copy.
    std::wstring cmdline = L"\"" + path + L"\"";
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');

    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    // If this fails (exe missing/not yet built), we simply skip the RGB
    // side-effect — everything else (refresh rate, brightness)
    // still applies normally.
}

void KillBatteryGame()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, BATTERY_GAME_EXE) == 0)
            {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc)
                {
                    TerminateProcess(hProc, 0);
                    CloseHandle(hProc);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// ---------------------------------------------------------------------------
// Windows "Energy Saver" (formerly "Battery Saver") via the documented
// automatic battery-threshold setting. Setting the DC threshold to 100%
// makes Energy Saver engage as soon as the machine is running on battery.
// When AC returns, the normal 50% automatic threshold is restored.
// ---------------------------------------------------------------------------
void ForceEnergySaverOn()
{
    // 100% means Energy Saver is eligible immediately whenever we're on DC.
    RunPowercfg(L"/setdcvalueindex SCHEME_CURRENT SUB_ENERGYSAVER ESBATTTHRESHOLD 100");

    // Re-apply the current scheme so Windows immediately picks up the new value.
    // This does NOT switch to a different power plan.
    RunPowercfg(L"/setactive SCHEME_CURRENT");
}

void RestoreEnergySaverDefault()
{
    // Restore the user's normal automatic Energy Saver threshold on AC.
    RunPowercfg(L"/setdcvalueindex SCHEME_CURRENT SUB_ENERGYSAVER ESBATTTHRESHOLD 50");
    RunPowercfg(L"/setactive SCHEME_CURRENT");
}

// ---------------------------------------------------------------------------
// Mode application
// ---------------------------------------------------------------------------
void ApplyBatteryMode()
{
    ForceEnergySaverOn();
    LaunchBatteryGame();   // triggers AWCC's "no color" profile for this decoy game
    SetRefreshRate(/*maxOut=*/false, /*fallbackHz=*/60);
    SetBrightnessWMI(0);

    ShowToast(L"Alienware Power Script", L"Laptop unplugged -- Battery mode initiated");
}

void ApplyPerformanceMode()
{
    RestoreEnergySaverDefault();
    KillBatteryGame();     // AWCC reverts to your normal/default lighting profile
    SetRefreshRate(/*maxOut=*/true, /*fallbackHz=*/60);

    ShowToast(L"Alienware Power Script", L"Laptop plugged in -- Performance mode initiated");
}

// ---------------------------------------------------------------------------
// Balloon notification (classic tray balloon — works everywhere without
// needing the WinRT toast/AppUserModelID plumbing).
// ---------------------------------------------------------------------------
void ShowToast(const std::wstring& title, const std::wstring& msg)
{
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle) - 1);
    wcsncpy(nid.szInfo, msg.c_str(), ARRAYSIZE(nid.szInfo) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------------------
// Tray icon setup / teardown
// ---------------------------------------------------------------------------
void CreateTrayIcon(HWND hwnd)
{
    g_hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_hIcon;
    wcsncpy(g_nid.szTip, APP_NAME, ARRAYSIZE(g_nid.szTip) - 1);

    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_hIcon) DestroyIcon(g_hIcon);
}

// ---------------------------------------------------------------------------
// Core logic: decide AC vs battery and apply the right mode, but only when
// the state actually changed (avoid re-applying on every unrelated
// WM_POWERBROADCAST notification).
// ---------------------------------------------------------------------------
void HandlePowerTransition()
{
    SYSTEM_POWER_STATUS sps = {};
    if (!GetSystemPowerStatus(&sps))
        return;

    bool onAC = (sps.ACLineStatus == 1);

    if (g_haveLastState && onAC == g_lastOnAC)
        return; // no real change

    g_lastOnAC = onAC;
    g_haveLastState = true;

    if (onAC)
        ApplyPerformanceMode();
    else
        ApplyBatteryMode();
}

// ---------------------------------------------------------------------------
// Window procedure — hidden message-only-style window used to receive
// WM_POWERBROADCAST and tray icon messages.
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMPOWERSTATUSCHANGE || wParam == PBT_POWERSETTINGCHANGE)
            HandlePowerTransition();
        return TRUE;

    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP)
        {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_EXIT)
        {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    // Only one instance at a time.
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\AlienwarePowerScript_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = APP_NAME;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, APP_NAME, APP_NAME, WS_OVERLAPPEDWINDOW,
                              0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) return 1;

    CreateTrayIcon(g_hwnd);

    // Subscribe to fine-grained AC/DC power source change notifications.
    RegisterPowerSettingNotification(g_hwnd, &GUID_ACDC_POWER_SOURCE,
                                      DEVICE_NOTIFY_WINDOW_HANDLE);

    // Apply the correct mode immediately on startup.
    HandlePowerTransition();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hMutex)
    {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return (int)msg.wParam;
}
