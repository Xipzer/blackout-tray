// Blackout: tray app for screens off. RGB, fans, DDC monitor power and window layout follow the display state.
// One hidden window, no polling except the optional iCUE LINK keep-alive. Helpers (OpenRGB, furyled) run once and exit.
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
#define ID_FANCTL    7
#define ID_FAN_AWAKE 100
#define ID_FAN_SLEEP 200
#define TIMER_OFF    1
#define TIMER_BUSY   2
#define TIMER_LINK   3
#define TIMER_LAYOUT 4
#define TIMER_IDLE   5
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
static BOOL hasRgb, hasOpenRgb, hasRam, hasFans;
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
static const int fanSteps[] = {25, 30, 35, 40, 45, 50, 55, 60, 70, 100};
static HPOWERNOTIFY displayNotify;

static void ddcPower(BOOL on);

static DWORD getDword(LPCWSTR name, DWORD fallback) {
    DWORD v, n = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, APP_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &n) == ERROR_SUCCESS ? v : fallback;
}

static void setDword(LPCWSTR name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, APP_KEY, name, REG_DWORD, &v, sizeof(v));
}

static BOOL startupEnabled(void) {
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void toggleStartup(void) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (startupEnabled()) {
        RegDeleteValueW(key, APP_NAME);
    } else {
        WCHAR path[MAX_PATH + 2];
        path[0] = L'"';
        DWORD len = GetModuleFileNameW(NULL, path + 1, MAX_PATH);
        path[len + 1] = L'"';
        path[len + 2] = 0;
        RegSetValueExW(key, APP_NAME, 0, REG_SZ, (BYTE *)path, (len + 3) * sizeof(WCHAR));
    }
    RegCloseKey(key);
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
    if (linkLeds > MAX_LINK_LEDS) linkLeds = MAX_LINK_LEDS;
}

// Corsair Commander Pro over raw HID: report [0, 0x23, channel, duty%] sets a fixed duty (liquidctl's protocol).
static void setFans(int duty) {
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &g, i, &ifd); i++) {
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; BYTE raw[1024]; } u;
        u.d.cbSize = sizeof(u.d);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, &u.d, sizeof(u), NULL, NULL)) continue;
        CharLowerW(u.d.DevicePath);
        if (!wcsstr(u.d.DevicePath, L"vid_1b1c&pid_0c10")) continue;
        HANDLE h = CreateFileW(u.d.DevicePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (duty >= 0) {
            for (BYTE ch = 0; ch < 6; ch++) {
                BYTE report[65] = {0, 0x23, ch, (BYTE)duty};
                DWORD w;
                WriteFile(h, report, sizeof(report), &w, NULL);
            }
        }
        hasFans = TRUE;
        CloseHandle(h);
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
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
// colour frame, so Blackout re-sends the frame every few seconds. Each tick is four tiny HID transactions.
static BOOL linkIo(BYTE *out, BYTE *in) {
    OVERLAPPED ov = {0};
    DWORD n;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    BOOL ok = WriteFile(link, out, 513, NULL, &ov) || GetLastError() == ERROR_IO_PENDING;
    ok = ok && WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0 && GetOverlappedResult(link, &ov, &n, FALSE);
    if (ok) {
        ResetEvent(ov.hEvent);
        ok = ReadFile(link, in, 513, NULL, &ov) || GetLastError() == ERROR_IO_PENDING;
        ok = ok && WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0 && GetOverlappedResult(link, &ov, &n, FALSE);
    }
    if (!ok) CancelIo(link);
    CloseHandle(ov.hEvent);
    return ok;
}

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

static BOOL linkRefresh(void) {
    return getDword(L"RgbOn", 1) ? linkColor(color[0], color[1], color[2]) : linkColor(0, 0, 0);
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
static void setIdleTimeout(DWORD seconds) {
    GUID *scheme;
    if (PowerGetActiveScheme(NULL, &scheme) != ERROR_SUCCESS) return;
    PowerWriteACValueIndex(NULL, scheme, &GUID_VIDEO_SUBGROUP, &GUID_VIDEO_POWERDOWN_TIMEOUT, seconds);
    PowerSetActiveScheme(NULL, scheme);
    LocalFree(scheme);
}

static void screensOff(HWND hwnd) {
    GUID *scheme;
    DWORD current;
    // DDC monitors go to standby first, while their link is still up; once the GPU output sleeps, DDC can't reach them.
    ddcPower(FALSE);
    if (idleRestore == (DWORD)-1 && PowerGetActiveScheme(NULL, &scheme) == ERROR_SUCCESS) {
        if (PowerReadACValueIndex(NULL, scheme, &GUID_VIDEO_SUBGROUP, &GUID_VIDEO_POWERDOWN_TIMEOUT, &current) == ERROR_SUCCESS) {
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

static void applyFans(void) {
    if (!hasFans || !getDword(L"FanCtl", 1)) return;
    DWORD sleep = getDword(L"FanSleep", 0);
    setFans(!screensOn && sleep ? sleep : getDword(L"FanAwake", 55));
}

static void trim(void) {
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

// OpenRGB takes a few seconds per run, so changes that arrive mid-run are queued and the last one wins.
static void setRgb(HWND hwnd, BOOL on) {
    if (busy) {
        pending = on;
        return;
    }
    busy = TRUE;
    pending = -1;
    setDword(L"RgbOn", on);
    linkRefresh();
    applyRgb(on);
    SetTimer(hwnd, TIMER_BUSY, 6000, NULL);
}

static void showMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_SCREENS, L"Screens off");
    if (hasRgb) {
        AppendMenuW(menu, MF_STRING, ID_RGB, getDword(L"RgbOn", 1) ? L"RGB off" : L"RGB on");
        AppendMenuW(menu, MF_STRING | (getDword(L"Sync", 1) ? MF_CHECKED : 0), ID_SYNC, L"RGB follows screens");
    }
    if (hasFans) {
        HMENU fans = CreatePopupMenu(), awake = CreatePopupMenu(), slept = CreatePopupMenu();
        WCHAR label[32];
        DWORD a = getDword(L"FanAwake", 55), z = getDword(L"FanSleep", 0);
        AppendMenuW(slept, MF_STRING | (z == 0 ? MF_CHECKED : 0), ID_FAN_SLEEP, L"Same as awake");
        for (int i = 0; i < (int)(sizeof(fanSteps) / sizeof(*fanSteps)); i++) {
            wsprintfW(label, L"%d%%", fanSteps[i]);
            AppendMenuW(awake, MF_STRING | (a == (DWORD)fanSteps[i] ? MF_CHECKED : 0), ID_FAN_AWAKE + fanSteps[i], label);
            AppendMenuW(slept, MF_STRING | (z == (DWORD)fanSteps[i] ? MF_CHECKED : 0), ID_FAN_SLEEP + fanSteps[i], label);
        }
        AppendMenuW(fans, MF_STRING | (getDword(L"FanCtl", 1) ? MF_CHECKED : 0), ID_FANCTL, L"Fan override");
        AppendMenuW(fans, MF_POPUP, (UINT_PTR)awake, L"Screens on");
        AppendMenuW(fans, MF_POPUP, (UINT_PTR)slept, L"Screens off");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)fans, L"Fans");
    }
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
            showMenu(hwnd);
            return 0;
        }
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, wp);
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
            if (pending >= 0 && pending != (int)getDword(L"RgbOn", 1)) setRgb(hwnd, pending);
            pending = -1;
        }
        trim();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SCREENS: SetTimer(hwnd, TIMER_OFF, 400, NULL); return 0;
        case ID_RGB: setRgb(hwnd, !getDword(L"RgbOn", 1)); return 0;
        case ID_SYNC: setDword(L"Sync", !getDword(L"Sync", 1)); return 0;
        case ID_FANCTL: setDword(L"FanCtl", !getDword(L"FanCtl", 1)); applyFans(); return 0;
        case ID_STARTUP: toggleStartup(); return 0;
        case ID_EXIT: DestroyWindow(hwnd); return 0;
        }
        if (LOWORD(wp) >= ID_FAN_SLEEP) setDword(L"FanSleep", LOWORD(wp) - ID_FAN_SLEEP);
        else if (LOWORD(wp) >= ID_FAN_AWAKE) setDword(L"FanAwake", LOWORD(wp) - ID_FAN_AWAKE);
        applyFans();
        return 0;
    case WM_POWERBROADCAST:
        // Display state: 0 = off, 1 = on, 2 = dimmed (ignored). Covers our own click and Windows' idle timeout.
        // Also re-applies fan duty after resume, since the Commander Pro resets to its default on power loss.
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
                    screensOn = state == 1;
                    if (hasRgb && getDword(L"Sync", 1)) setRgb(hwnd, screensOn);
                    applyFans();
                }
            }
        }
        return TRUE;
    case WM_DISPLAYCHANGE:
        // Monitors re-enumerate a few seconds after waking; restore once they have settled.
        if (screensOn && savedCount) SetTimer(hwnd, TIMER_LAYOUT, 2500, NULL);
        return 0;
    case WM_DESTROY:
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
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.uVersion = NOTIFYICON_VERSION_4;
    int px = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    nid.hIcon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, 0);
    lstrcpyW(nid.szTip, L"Blackout: click for screens off, right-click for RGB");
    Shell_NotifyIconW(NIM_ADD, &nid);
    Shell_NotifyIconW(NIM_SETVERSION, &nid);

    // Registering sends the current display state straight away, which also re-asserts the lighting at login.
    WCHAR fury[MAX_PATH];
    loadConfig();
    exeDir(fury, L"furyled.exe");
    hasOpenRgb = openrgb[0] && GetFileAttributesW(openrgb) != INVALID_FILE_ATTRIBUTES;
    hasRam = GetFileAttributesW(fury) != INVALID_FILE_ATTRIBUTES;
    setFans(-1);
    if (linkLeds) SetTimer(hwnd, TIMER_LINK, 1000, NULL);
    hasRgb = hasOpenRgb || hasRam || linkLeds;
    if (hasRgb || hasFans || ddcIds[0]) displayNotify = RegisterPowerSettingNotification(hwnd, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
    trim();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    CloseHandle(mutex);
    return 0;
}
