// Blackout: tray app for screens off. RGB, DDC monitor power and window layout follow the display state, and the
// wake rules decide which input may turn the screens back on.
// One hidden window. The only periodic work is the optional 4 s tick (iCUE LINK lighting keep-alive). Helpers
// (OpenRGB, furyled) run once and exit. Nothing here needs admin rights.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <powrprof.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

#define WM_TRAY      (WM_APP + 1)
#define ID_SCREENS   1
#define ID_RGB       2
#define ID_STARTUP   4
#define ID_EXIT      5
#define ID_SYNC      6
#define ID_HOTKEY    7
#define TIMER_OFF    1
#define TIMER_BUSY   2
#define TIMER_LINK   3
#define TIMER_LAYOUT 4
#define TIMER_IDLE   5
#define TIMER_ICON   6
#define MAX_LINK_LEDS 160
#define MAX_WINDOWS  256
#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APP_KEY      L"Software\\Blackout"
#define APP_NAME     L"Blackout"
#define CONFIG       L"blackout.ini"

static NOTIFYICONDATAW nid;
static UINT taskbarCreated;
static BOOL busy;
static int pending = -1;
static BOOL hasRgb, hasOpenRgb, hasRam;
static BOOL screensOn = TRUE;
static WCHAR iniPath[MAX_PATH], openrgb[MAX_PATH], rgbOff[4][1024], rgbOn[4][1024];
static BYTE color[3] = {255, 0, 0};
static int linkLeds;
static WCHAR ddcIds[256];
static int ddcOff = 4, savedPaths;
static HANDLE link = INVALID_HANDLE_VALUE;
static struct { HWND hwnd; WINDOWPLACEMENT wp; } saved[MAX_WINDOWS];
static int savedCount;
static DWORD idleRestore = (DWORD)-1;
static HPOWERNOTIFY displayNotify;
static HWND panel;
static void panelUpdate(void);
// The switches, read from HKCU\Software\Blackout once at startup and written through when changed, so neither the
// 4 s tick nor a display event nor the panel reads the registry. litWant is what the user wants while the screens are
// on (the RGB switch), litOn the lighting right now, which also goes off with the screens when syncOn is set.
static BOOL litOn = TRUE, litWant = TRUE, syncOn = TRUE, hotkeyOn, wakeClick = TRUE, wakeEsc = TRUE;
static BOOL linkOk; // the iCUE LINK hub took the last frame
// What woke the screens. Kept in memory only, newest first. icon: 'M' mouse, 'K' keyboard, 0 neither. slip: the input
// got past the wake rules.
#define MAX_WAKES 5
static struct { SYSTEMTIME at; WCHAR icon, name[64]; BOOL slip; } wakes[MAX_WAKES];
static int wakeCount;
static HANDLE rawDevice, hookThread;
static DWORD rawType, rawTick, hotkeyCause, hookThreadId;

static void ddcPower(BOOL on);

static DWORD getDword(LPCWSTR name, DWORD fallback) {
    DWORD v, n = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, APP_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &n) == ERROR_SUCCESS ? v : fallback;
}

static void setDword(LPCWSTR name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, APP_KEY, name, REG_DWORD, &v, sizeof(v));
}

// Start with Windows is a plain Run entry for this user: nothing Blackout does needs admin rights.
static BOOL startupEnabled(void) {
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void toggleStartup(void) {
    WCHAR path[MAX_PATH + 3] = L"\"";
    if (startupEnabled()) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME);
        return;
    }
    DWORD len = GetModuleFileNameW(NULL, path + 1, MAX_PATH);
    path[len + 1] = L'"';
    path[len + 2] = 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, REG_SZ, path, (len + 3) * sizeof(WCHAR));
}

static void spawn(LPWSTR cmd) {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static void exeDir(LPWSTR out, LPCWSTR file) {
    DWORD n = GetModuleFileNameW(NULL, out, MAX_PATH);
    while (n && out[n - 1] != L'\\') n--;
    lstrcpyW(out + n, file);
}

static void applyRgb(BOOL on) {
    WCHAR cmd[1400], exe[MAX_PATH];
    // Steps run one after another in a single cmd.exe, so a device that is missing from one step (OpenRGB then fails
    // that whole call) can't stop the others. Keep devices that come and go in their own step (off2=, on2=, ...).
    if (hasOpenRgb) {
        WCHAR (*steps)[1024] = on ? rgbOn : rgbOff, chain[4 * 1400];
        lstrcpyW(chain, L"cmd.exe /d /c \"");
        for (int i = 0, first = 1; i < 4; i++) {
            if (!steps[i][0]) continue;
            wsprintfW(cmd, L"%s\"%s\" %s", first ? L"" : L" & ", openrgb, steps[i]);
            lstrcatW(chain, cmd);
            first = 0;
        }
        lstrcatW(chain, L"\"");
        spawn(chain);
    }
    // Optional FURY module: furyled.exe beside blackout.exe drives Kingston FURY DDR5 through Kingston's own service.
    if (hasRam) {
        exeDir(exe, L"furyled.exe");
        if (on) wsprintfW(cmd, L"\"%s\" %02X%02X%02X", exe, color[0], color[1], color[2]);
        else wsprintfW(cmd, L"\"%s\" off", exe);
        spawn(cmd);
    }
}

static void loadConfig(void) {
    WCHAR raw[MAX_PATH], hex[16];
    exeDir(iniPath, CONFIG);
    GetPrivateProfileStringW(L"rgb", L"openrgb", L"", raw, MAX_PATH, iniPath);
    ExpandEnvironmentStringsW(raw, openrgb, MAX_PATH);
    for (int i = 0; i < 4; i++) {
        WCHAR offKey[8] = L"off", onKey[8] = L"on";
        if (i) wsprintfW(offKey, L"off%d", i + 1), wsprintfW(onKey, L"on%d", i + 1);
        GetPrivateProfileStringW(L"rgb", offKey, i ? L"" : L"--noautoconnect -m Static -c 000000", rgbOff[i], 1024, iniPath);
        GetPrivateProfileStringW(L"rgb", onKey, i ? L"" : L"--noautoconnect -m Static -c FF0000", rgbOn[i], 1024, iniPath);
    }
    GetPrivateProfileStringW(L"rgb", L"color", L"FF0000", hex, 16, iniPath);
    DWORD c = 0;
    for (WCHAR *h = hex; *h; h++) c = c * 16 + (*h <= L'9' ? *h - L'0' : (*h | 32) - L'a' + 10);
    color[0] = (BYTE)(c >> 16), color[1] = (BYTE)(c >> 8), color[2] = (BYTE)c;
    GetPrivateProfileStringW(L"ddc", L"monitors", L"", ddcIds, 256, iniPath);
    CharUpperW(ddcIds);
    ddcOff = GetPrivateProfileIntW(L"ddc", L"off", 4, iniPath);
    linkLeds = GetPrivateProfileIntW(L"link", L"leds", 0, iniPath);
    linkLeds = linkLeds < 0 ? 0 : linkLeds > MAX_LINK_LEDS ? MAX_LINK_LEDS : linkLeds;
}

// Opens the first HID interface matching both substrings (lowercase), e.g. the iCUE LINK hub's MI_00.
static HANDLE openHid(LPCWSTR id, LPCWSTR iface, DWORD flags) {
    GUID g;
    HANDLE found = INVALID_HANDLE_VALUE;
    HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return found;
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &g, i, &ifd); i++) {
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; BYTE raw[1024]; } u;
        u.d.cbSize = sizeof(u.d);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, &u.d, sizeof(u), NULL, NULL)) continue;
        CharLowerW(u.d.DevicePath);
        if (!wcsstr(u.d.DevicePath, id) || (iface && !wcsstr(u.d.DevicePath, iface))) continue;
        found = CreateFileW(u.d.DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, flags, NULL);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// iCUE LINK hub protocol (from OpenRGB's CorsairICueLinkController): 513-byte reports [0, 0, 1, cmd..., data...],
// each answered by one 513-byte input report. The hub drops back to its stored hardware lighting ~5 s after the last
// colour frame, so Blackout re-sends the frame every 4 s. Each tick is five small HID exchanges.
static HANDLE ioEvent;

// One HID exchange with 1 s timeouts: send a report (if any), then read one (if wanted).
static BOOL hidIo(HANDLE h, const BYTE *out, DWORD outLen, BYTE *in, DWORD inLen) {
    OVERLAPPED ov = {0};
    DWORD n;
    BOOL ok = TRUE;
    if (!ioEvent) ioEvent = CreateEventW(NULL, TRUE, FALSE, NULL); // one event for the life of the process
    ov.hEvent = ioEvent;
    if (out) {
        ResetEvent(ioEvent);
        ok = WriteFile(h, out, outLen, NULL, &ov) || GetLastError() == ERROR_IO_PENDING;
        ok = ok && WaitForSingleObject(ioEvent, 1000) == WAIT_OBJECT_0 && GetOverlappedResult(h, &ov, &n, FALSE);
    }
    if (ok && in) {
        ResetEvent(ioEvent);
        ok = ReadFile(h, in, inLen, NULL, &ov) || GetLastError() == ERROR_IO_PENDING;
        ok = ok && WaitForSingleObject(ioEvent, 1000) == WAIT_OBJECT_0 && GetOverlappedResult(h, &ov, &n, FALSE);
    }
    if (!ok) CancelIo(h);
    return ok;
}

static BOOL linkIo(BYTE *out, BYTE *in) { return hidIo(link, out, 513, in, 513); }

// Windows gives every open handle a copy of each input report. When another program (HWiNFO, iCUE) talks to the hub,
// its replies queue up on our handle too, and every later read would take a stale reply and stay out of step. So each
// frame starts with an empty queue, and a failed exchange drops the handle (a new one starts in step).
static void linkSync(void) { HidD_FlushQueue(link); }

static BOOL linkCmd(const BYTE *cmd, int cmdLen, const BYTE *data, int dataLen) {
    BYTE out[513] = {0}, in[513];
    out[2] = 1;
    memcpy(out + 3, cmd, cmdLen);
    if (data) memcpy(out + 3 + cmdLen, data, dataLen);
    return linkIo(out, in);
}

static BOOL linkColor(BYTE r, BYTE g, BYTE b) {
    static const BYTE softMode[] = {0x01, 0x03, 0x00, 0x02}, close[] = {0x05, 0x01, 0x01}, open[] = {0x0d, 0x00};
    static const BYTE write[] = {0x06, 0x00}, endpoint[] = {0x22};
    BYTE frame[6 + MAX_LINK_LEDS * 3];
    int len = linkLeds * 3 + 2;
    if (link == INVALID_HANDLE_VALUE) link = openHid(L"vid_1b1c&pid_0c3f", L"mi_00", FILE_FLAG_OVERLAPPED);
    if (link == INVALID_HANDLE_VALUE) return FALSE;
    linkSync();
    frame[0] = (BYTE)len, frame[1] = (BYTE)(len >> 8), frame[2] = frame[3] = 0, frame[4] = 0x12, frame[5] = 0x00;
    for (int i = 0; i < linkLeds; i++) frame[6 + i * 3] = r, frame[7 + i * 3] = g, frame[8 + i * 3] = b;
    BOOL ok = linkCmd(softMode, sizeof(softMode), NULL, 0) && linkCmd(close, sizeof(close), endpoint, 1) &&
              linkCmd(open, sizeof(open), endpoint, 1) && linkCmd(write, sizeof(write), frame, 6 + linkLeds * 3) &&
              linkCmd(close, sizeof(close), endpoint, 1);
    if (!ok) {
        CloseHandle(link);
        link = INVALID_HANDLE_VALUE;
    }
    return ok;
}

// Only with [link] leds= set: a hub that is not set up is never opened.
static BOOL linkRefresh(void) {
    if (!linkLeds) return FALSE;
    return linkOk = litOn ? linkColor(color[0], color[1], color[2]) : linkColor(0, 0, 0);
}

// Monitors that drop off the bus while asleep make Windows reshuffle windows; snapshot on sleep, restore on wake.
static BOOL CALLBACK saveWindow(HWND h, LPARAM lp) {
    if (savedCount < MAX_WINDOWS && IsWindowVisible(h) && !(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) && !GetWindow(h, GW_OWNER)) {
        saved[savedCount].wp.length = sizeof(WINDOWPLACEMENT);
        if (GetWindowPlacement(h, &saved[savedCount].wp)) saved[savedCount++].hwnd = h;
    }
    return TRUE;
}

static void restoreWindows(void) {
    for (int i = 0; i < savedCount; i++) {
        WINDOWPLACEMENT now = {sizeof(now)};
        if (!IsWindow(saved[i].hwnd) || !GetWindowPlacement(saved[i].hwnd, &now)) continue;
        if (now.showCmd == SW_SHOWMINIMIZED) continue;
        WINDOWPLACEMENT wp = saved[i].wp;
        if (wp.showCmd == SW_SHOWMINIMIZED) wp.showCmd = SW_SHOWNORMAL;
        wp.flags = 0;
        SetWindowPlacement(saved[i].hwnd, &wp);
    }
}

// Screens off the same way Windows' own idle timeout does it: drop the display timeout to 1 s, then put it back once
// the displays report off. The legacy SC_MONITORPOWER message leaves some outputs (e.g. the Intel iGPU's DisplayPort)
// half-alive, which makes the monitor on it keep flashing "No Signal". SC_MONITORPOWER stays as a fallback.
// A PC has two display timeouts, plugged in and on battery: the one in use when the click came is the one changed.
static BOOL idleOnBattery;

static void setIdleTimeout(DWORD seconds) {
    GUID *scheme;
    if (PowerGetActiveScheme(NULL, &scheme) != ERROR_SUCCESS) return;
    (idleOnBattery ? PowerWriteDCValueIndex : PowerWriteACValueIndex)(NULL, scheme, &GUID_VIDEO_SUBGROUP, &GUID_VIDEO_POWERDOWN_TIMEOUT, seconds);
    PowerSetActiveScheme(NULL, scheme);
    LocalFree(scheme);
}

static void screensOff(HWND hwnd) {
    GUID *scheme;
    DWORD current;
    SYSTEM_POWER_STATUS power;
    // DDC monitors go to standby first, while their link is still up; once the GPU output sleeps, DDC can't reach them.
    ddcPower(FALSE);
    if (idleRestore == (DWORD)-1 && PowerGetActiveScheme(NULL, &scheme) == ERROR_SUCCESS) {
        idleOnBattery = GetSystemPowerStatus(&power) && power.ACLineStatus == 0;
        if ((idleOnBattery ? PowerReadDCValueIndex : PowerReadACValueIndex)(NULL, scheme, &GUID_VIDEO_SUBGROUP, &GUID_VIDEO_POWERDOWN_TIMEOUT, &current) == ERROR_SUCCESS) {
            idleRestore = current;
            setIdleTimeout(1);
        }
        LocalFree(scheme);
    }
    SetTimer(hwnd, TIMER_IDLE, 6000, NULL);
}

static void restoreIdle(void) {
    if (idleRestore == (DWORD)-1) return;
    setIdleTimeout(idleRestore);
    idleRestore = (DWORD)-1;
}

// DDC/CI power control (VCP 0xD6: 1 = on, 4 = standby, 5 = off) for monitors listed in [ddc] monitors=, matched
// against the monitor's hardware ID (e.g. BNQ7F2F). For screens whose GPU output keeps waking them to show "No Signal".
// Hard off (5) makes some monitors drop off the bus and ignore the wake command, so [ddc] off= defaults to standby (4).
static BOOL CALLBACK ddcMonitor(HMONITOR m, HDC dc, LPRECT r, LPARAM power) {
    MONITORINFOEXW mi = {0};
    DISPLAY_DEVICEW dd = {0};
    DWORD count;
    mi.cbSize = sizeof(mi);
    dd.cb = sizeof(dd);
    if (!GetMonitorInfoW(m, (MONITORINFO *)&mi) || !EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0)) return TRUE;
    WCHAR *id = wcschr(dd.DeviceID, L'\\'), list[256], *tok, *ctx;
    if (!id) return TRUE;
    lstrcpyW(list, ddcIds);
    for (tok = wcstok(list, L", ", &ctx); tok; tok = wcstok(NULL, L", ", &ctx)) {
        if (!wcsstr(id, tok)) continue;
        PHYSICAL_MONITOR pm[4];
        if (GetNumberOfPhysicalMonitorsFromHMONITOR(m, &count) && count && count <= 4 && GetPhysicalMonitorsFromHMONITOR(m, count, pm)) {
            for (DWORD i = 0; i < count; i++) SetVCPFeature(pm[i].hPhysicalMonitor, 0xD6, (DWORD)power);
            DestroyPhysicalMonitors(count, pm);
        }
        break;
    }
    return TRUE;
}

static void ddcPower(BOOL on) {
    if (ddcIds[0]) EnumDisplayMonitors(NULL, NULL, ddcMonitor, on ? 1 : ddcOff);
}

static int activePaths(void) {
    UINT32 np = 0, nm = 0;
    return GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS ? (int)np : 0;
}

// If a monitor dropped out of the desktop while asleep, re-extend. Windows restores the saved arrangement for that
// monitor set from its own display database.
static void restoreTopology(void) {
    if (savedPaths && activePaths() < savedPaths) SetDisplayConfig(0, NULL, 0, NULL, SDC_TOPOLOGY_EXTEND | SDC_APPLY);
}

static void trim(void) {
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static void setRgb(HWND hwnd, BOOL on);
static void rgbToggle(HWND hwnd) {
    litWant = !litWant;
    setDword(L"RgbWant", litWant);
    setRgb(hwnd, litWant);
}

// OpenRGB takes a few seconds per run, so changes that arrive mid-run are queued and the last one wins.
static void setRgb(HWND hwnd, BOOL on) {
    if (busy) {
        pending = on;
        return;
    }
    busy = TRUE;
    pending = -1;
    litOn = on;
    setDword(L"RgbOn", on);
    linkRefresh();
    applyRgb(on);
    SetTimer(hwnd, TIMER_BUSY, 6000, NULL);
}

static void showMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_SCREENS, L"Screens off");
    if (hasRgb) {
        AppendMenuW(menu, MF_STRING, ID_RGB, litWant ? L"RGB off" : L"RGB on");
        AppendMenuW(menu, MF_STRING | (syncOn ? MF_CHECKED : 0), ID_SYNC, L"RGB off with the screens");
    }
    AppendMenuW(menu, MF_STRING | (hotkeyOn ? MF_CHECKED : 0), ID_HOTKEY,
                wakeClick && wakeEsc ? L"Wake the screens only with a left-click or Esc" : wakeClick ? L"Wake the screens only with a left-click" : L"Wake the screens only with Esc");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (startupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    trim();
}

// Wake log. While the screens are off, raw input is watched so the device behind a wake can be named. The handler
// only stores a handle and a tick; the name is looked up once, when the screens come on.
static void wakeWatch(HWND hwnd, BOOL on) {
    RAWINPUTDEVICE rid[2] = {{1, 2, on ? RIDEV_INPUTSINK : RIDEV_REMOVE, on ? hwnd : NULL}, {1, 6, on ? RIDEV_INPUTSINK : RIDEV_REMOVE, on ? hwnd : NULL}};
    RegisterRawInputDevices(rid, 2, sizeof(*rid));
    if (on) rawDevice = NULL, rawTick = 0, hotkeyCause = 0;
}

static void wakeRecord(void) {
    WCHAR path[256], name[48] = L"", icon = 0, *kind = L"No input: Windows or an app";
    UINT n = 256;
    BOOL slip = FALSE;
    if (hotkeyCause) {
        icon = hotkeyCause == 1 ? L'M' : L'K', kind = hotkeyCause == 1 ? L"Left-click" : L"Esc";
    } else if (rawTick && GetTickCount() - rawTick < 5000) {
        if (rawDevice && GetRawInputDeviceInfoW(rawDevice, RIDI_DEVICENAME, path, &n) != (UINT)-1) {
            HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                if (!HidD_GetProductString(h, name, sizeof(name))) name[0] = 0;
                CloseHandle(h);
            }
        }
        name[40] = 0;
        icon = rawType == RIM_TYPEMOUSE ? L'M' : rawType == RIM_TYPEKEYBOARD ? L'K' : 0;
        kind = icon == L'M' ? L"Mouse" : icon ? L"Keyboard" : L"Input device";
        // With the wake rules armed, input should not have been able to wake the screens; say so if it did.
        slip = hookThread != NULL;
    }
    for (int i = MAX_WAKES - 1; i > 0; i--) wakes[i] = wakes[i - 1];
    GetLocalTime(&wakes[0].at);
    wakes[0].icon = icon, wakes[0].slip = slip;
    // The icon says mouse or keyboard, so a named device is listed by its name alone.
    if (name[0] && icon) lstrcpyW(wakes[0].name, name);
    else wsprintfW(wakes[0].name, name[0] ? L"%s: %s" : L"%s", kind, name);
    if (wakeCount < MAX_WAKES) wakeCount++;
}

// Wake rules ("Wake the screens only with", HKCU HotkeyMode): while the screens are off, low-level hooks swallow all
// keyboard and mouse input except the chosen wake inputs (left-click and/or Esc), so a bumped mouse or desk vibration
// can't wake the screens. The hooks live on a short-lived thread that exists only while the screens are off.
// Ctrl+Alt+Del always gets through (Windows). Windows does not show a normal program the input that goes to a
// program running as admin, so with such a window in front the rules are not in force (README, Limits).
#define WM_WAKE (WM_APP + 2)
static HWND mainWnd;

static LRESULT CALLBACK hookKeys(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION || (((KBDLLHOOKSTRUCT *)lp)->flags & LLKHF_INJECTED)) return CallNextHookEx(NULL, code, wp, lp);
    if (wakeEsc && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && ((KBDLLHOOKSTRUCT *)lp)->vkCode == VK_ESCAPE) PostMessageW(mainWnd, WM_WAKE, 2, 0);
    return 1;
}

static LRESULT CALLBACK hookMouse(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION || (((MSLLHOOKSTRUCT *)lp)->flags & LLMHF_INJECTED)) return CallNextHookEx(NULL, code, wp, lp);
    if (wakeClick && wp == WM_LBUTTONDOWN) PostMessageW(mainWnd, WM_WAKE, 1, 0);
    return 1;
}

static DWORD WINAPI hookMain(LPVOID unused) {
    MSG m;
    HHOOK k = SetWindowsHookExW(WH_KEYBOARD_LL, hookKeys, GetModuleHandleW(NULL), 0);
    HHOOK ms = SetWindowsHookExW(WH_MOUSE_LL, hookMouse, GetModuleHandleW(NULL), 0);
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    if (k) UnhookWindowsHookEx(k);
    if (ms) UnhookWindowsHookEx(ms);
    return 0;
}

static void hotkeyArm(void) {
    if (hookThread || !hotkeyOn || (!wakeClick && !wakeEsc)) return; // with neither input nothing could wake the screens
    hookThread = CreateThread(NULL, 0, hookMain, NULL, 0, &hookThreadId);
}

static void hotkeyDisarm(void) {
    if (!hookThread) return;
    PostThreadMessageW(hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(hookThread, 1000);
    CloseHandle(hookThread);
    hookThread = NULL;
}

// Wake input seen: drop the hooks and wake the screens with a 1 px injected nudge.
static void hotkeyWake(void) {
    INPUT in[2] = {0};
    hotkeyDisarm();
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dx = 1, in[1].mi.dx = -1;
    in[0].mi.dwFlags = in[1].mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(2, in, sizeof(INPUT));
    SetThreadExecutionState(ES_DISPLAY_REQUIRED);
}

#include "panel.c"

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            // Short delay so the click release doesn't wake the screens straight back up.
            SetTimer(hwnd, TIMER_OFF, 400, NULL);
            return 0;
        case WM_CONTEXTMENU:
            if (!panelShow(hwnd)) showMenu(hwnd);
            return 0;
        }
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, wp);
        if (wp == TIMER_ICON) {
            static int tries;
            if (Shell_NotifyIconW(NIM_ADD, &nid)) Shell_NotifyIconW(NIM_SETVERSION, &nid);
            else if (++tries < 30) SetTimer(hwnd, TIMER_ICON, 2000, NULL);
            return 0;
        }
        if (wp == TIMER_LINK) {
            // Back off if the hub isn't answering so a missing device can't keep stalling the message loop.
            SetTimer(hwnd, TIMER_LINK, linkRefresh() ? 4000 : 30000, NULL);
            return 0;
        }
        if (wp == TIMER_LAYOUT) {
            restoreTopology();
            restoreWindows();
        } else if (wp == TIMER_OFF) {
            screensOff(hwnd);
        } else if (wp == TIMER_IDLE) {
            // Idle path didn't take (e.g. something is holding the display on): fall back to the legacy message.
            restoreIdle();
            if (screensOn) DefWindowProcW(hwnd, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
        } else {
            busy = FALSE;
            int queued = pending;
            pending = -1;
            if (queued >= 0 && queued != (int)litOn) setRgb(hwnd, queued);
            panelUpdate();
        }
        trim();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SCREENS: SetTimer(hwnd, TIMER_OFF, 400, NULL); return 0;
        case ID_RGB: rgbToggle(hwnd); return 0;
        case ID_SYNC: setDword(L"Sync", syncOn = !syncOn); return 0;
        case ID_HOTKEY: setDword(L"HotkeyMode", hotkeyOn = !hotkeyOn); return 0;
        case ID_STARTUP: toggleStartup(); return 0;
        case ID_EXIT: DestroyWindow(hwnd); return 0;
        }
        return 0;
    case WM_POWERBROADCAST:
        // Display state: 0 = off, 1 = on, 2 = dimmed (ignored). Covers our own click and Windows' idle timeout.
        if (wp == PBT_POWERSETTINGCHANGE) {
            POWERBROADCAST_SETTING *ps = (POWERBROADCAST_SETTING *)lp;
            if (IsEqualGUID(&ps->PowerSetting, &GUID_CONSOLE_DISPLAY_STATE) && ps->DataLength >= sizeof(DWORD)) {
                DWORD state = *(DWORD *)ps->Data;
                if (state != 2) {
                    if (state == 0) {
                        KillTimer(hwnd, TIMER_IDLE);
                        restoreIdle();
                    }
                    if (state == 0 && screensOn) {
                        savedCount = 0;
                        savedPaths = activePaths();
                        EnumWindows(saveWindow, 0);
                        ddcPower(FALSE);
                    } else if (state == 1 && !screensOn) {
                        ddcPower(TRUE);
                        SetTimer(hwnd, TIMER_LAYOUT, 4000, NULL);
                    }
                    if (state == 1 && !screensOn) wakeRecord();
                    if ((state == 1) != screensOn) wakeWatch(hwnd, state == 0);
                    screensOn = state == 1;
                    if (screensOn) hotkeyDisarm();
                    else hotkeyArm();
                    if (hasRgb && syncOn) setRgb(hwnd, screensOn && litWant);
                    panelUpdate();
                }
            }
        }
        return TRUE;
    case WM_DISPLAYCHANGE:
        // Monitors re-enumerate a few seconds after waking; restore once they have settled.
        if (screensOn && savedCount) SetTimer(hwnd, TIMER_LAYOUT, 2500, NULL);
        return 0;
    case WM_WAKE:
        hotkeyCause = (DWORD)wp;
        hotkeyWake();
        return 0;
    case WM_INPUT: {
        RAWINPUTHEADER head;
        UINT size = sizeof(head);
        if (!screensOn && GetRawInputData((HRAWINPUT)lp, RID_HEADER, &head, &size, sizeof(head)) == sizeof(head))
            rawDevice = head.hDevice, rawType = head.dwType, rawTick = GetTickCount();
        break; // DefWindowProc releases the input
    }
    case WM_DESTROY:
        hotkeyDisarm();
        restoreIdle();
        if (link != INVALID_HANDLE_VALUE) {
            static const BYTE hardMode[] = {0x01, 0x03, 0x00, 0x01};
            linkCmd(hardMode, sizeof(hardMode), NULL, 0);
        }
        if (displayNotify) UnregisterPowerSettingNotification(displayNotify);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == taskbarCreated) {
        Shell_NotifyIconW(NIM_ADD, &nid);
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show) {
    // Developer modes for the panel (preview, off-screen shot, benchmark): see the end of panel.c.
    if (wcsstr(cmd, L"--panel-")) return panelDev(cmd);
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\BlackoutTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = APP_NAME;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, APP_NAME, APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    mainWnd = hwnd;
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.uVersion = NOTIFYICON_VERSION_4;
    int px = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    nid.hIcon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, 0);
    lstrcpyW(nid.szTip, L"Blackout: click for screens off, right-click for the panel");
    // Started at logon, Blackout can be faster than the taskbar: then the icon is refused, and the taskbar's
    // "created" broadcast may already have gone. Try again every 2 s for a minute.
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) SetTimer(hwnd, TIMER_ICON, 2000, NULL);
    else Shell_NotifyIconW(NIM_SETVERSION, &nid);

    // Registering sends the current display state straight away, which also re-asserts the lighting at login.
    WCHAR fury[MAX_PATH];
    loadConfig();
    exeDir(fury, L"furyled.exe");
    hasOpenRgb = openrgb[0] && GetFileAttributesW(openrgb) != INVALID_FILE_ATTRIBUTES;
    hasRam = GetFileAttributesW(fury) != INVALID_FILE_ATTRIBUTES;
    litOn = getDword(L"RgbOn", 1), litWant = getDword(L"RgbWant", 1), syncOn = getDword(L"Sync", 1);
    hotkeyOn = getDword(L"HotkeyMode", 0), wakeClick = getDword(L"WakeClick", 1), wakeEsc = getDword(L"WakeEsc", 1);
    // Blackout needs no admin rights, but may be started with them. An elevated window drops messages from normal
    // processes unless allowed (tray clicks and remote "screens off").
    ChangeWindowMessageFilterEx(hwnd, WM_COMMAND, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(hwnd, WM_TRAY, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(hwnd, taskbarCreated, MSGFLT_ALLOW, NULL);
    if (linkLeds) SetTimer(hwnd, TIMER_LINK, 1000, NULL);
    hasRgb = hasOpenRgb || hasRam || linkLeds;
    displayNotify = RegisterPowerSettingNotification(hwnd, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
    trim();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        DispatchMessageW(&m);
    }
    CloseHandle(mutex);
    return 0;
}