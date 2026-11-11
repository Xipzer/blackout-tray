// Blackout: tray app for screens off. RGB, fans, DDC monitor power and window layout follow the display state.
// One hidden window. The only periodic work is the optional 4 s iCUE LINK tick (lighting keep-alive and the coolant fan
// curve). Helpers (OpenRGB, furyled, schtasks) run once and exit.
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
#define ID_CURVE     8
#define ID_FAN_AWAKE 100
#define ID_FAN_SLEEP 200
#define TIMER_OFF    1
#define TIMER_BUSY   2
#define TIMER_LINK   3
#define TIMER_LAYOUT 4
#define TIMER_IDLE   5
#define MAX_LINK_LEDS 160
#define MAX_WINDOWS  256
#define MAX_CURVE    8
#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define TASK_KEY     L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tree\\Blackout"
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
// Coolant fan curve on motherboard headers (see the Super I/O section).
static BOOL hasCurve;
static int fanCh[7], fanChCount, curveT[MAX_CURVE], curveD[MAX_CURVE], curveN, fanHyst = 2, linkSensor = -1;
static int fanDuty = -1, tempFails, fanMax = 100, fanRpm[7];
static WCHAR fanNames[96];
static double coolant = -1;
// Coolant history for the panel graph: 15 min at one sample per 4 s tick, C x100.
#define HISTORY 225
static short history[HISTORY];
static int historyLen, historyPos;
static HWND panel;
static void panelUpdate(void);
// Cached switches, so the 4 s tick never reads the registry. Written through to HKCU when changed.
static BOOL litOn = TRUE, litWant = TRUE, curveOn = TRUE, linkOk;
// Why the coolant fan curve is off (shown in the panel): 0 active, 1 no [fans], 2 not admin, 3 PawnIO or module missing, 4 no chip.
static int fanWhy = 1;
// What woke the screens (panel > Settings > Wake). Kept in memory only, newest first.
#define MAX_WAKES 5
static struct { SYSTEMTIME at; WCHAR cause[96]; } wakes[MAX_WAKES];
static int wakeCount;
static HANDLE rawDevice, hookThread;
static DWORD rawType, rawTick, hotkeyCause, hookThreadId;

static void ddcPower(BOOL on);

static DWORD getDword(LPCWSTR name, DWORD fallback) {
    DWORD v, n = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, APP_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &n) == ERROR_SUCCESS ? v : fallback;
}

static DWORD getDwordAt(HKEY root, LPCWSTR key, LPCWSTR name, DWORD fallback) {
    DWORD v, n = sizeof(v);
    return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, NULL, &v, &n) == ERROR_SUCCESS ? v : fallback;
}

static void setDword(LPCWSTR name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, APP_KEY, name, REG_DWORD, &v, sizeof(v));
}

static BOOL isElevated(void) {
    HANDLE t;
    TOKEN_ELEVATION e = {0};
    DWORD n;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return FALSE;
    GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n);
    CloseHandle(t);
    return e.TokenIsElevated != 0;
}

static BOOL runKeySet(void) {
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static BOOL taskSet(void) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, TASK_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) return FALSE;
    RegCloseKey(key);
    return TRUE;
}

static BOOL startupEnabled(void) {
    return runKeySet() || taskSet();
}

static void runWait(LPWSTR cmd) {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return;
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

// Fan control needs admin (PawnIO), and a Run key can't start an elevated app without a UAC prompt. When running
// elevated, Start with Windows uses a logon task with highest privileges instead, with no time limit or battery stop.
static void createTask(LPCWSTR exe) {
    WCHAR user[256], name[128], xml[2048], file[MAX_PATH], cmd[MAX_PATH + 64];
    DWORD w;
    GetEnvironmentVariableW(L"USERDOMAIN", user, 128);
    GetEnvironmentVariableW(L"USERNAME", name, 128);
    lstrcatW(user, L"\\");
    lstrcatW(user, name);
    // Built with lstrcat: wsprintf output is capped at 1024 characters.
    LPCWSTR parts[] = {L"\xFEFF<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<Triggers><LogonTrigger><UserId>", user, L"</UserId></LogonTrigger></Triggers>"
        L"<Principals><Principal id=\"Author\"><UserId>", user, L"</UserId><LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>5</Priority></Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>", exe, L"</Command></Exec></Actions></Task>"};
    xml[0] = 0;
    for (int i = 0; i < (int)(sizeof(parts) / sizeof(*parts)); i++) lstrcatW(xml, parts[i]);
    GetTempPathW(MAX_PATH, file);
    lstrcatW(file, L"blackout-task.xml");
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, xml, lstrlenW(xml) * sizeof(WCHAR), &w, NULL);
    CloseHandle(f);
    wsprintfW(cmd, L"schtasks.exe /Create /TN Blackout /XML \"%s\" /F", file);
    runWait(cmd);
    DeleteFileW(file);
}

static void toggleStartup(void) {
    HKEY key;
    WCHAR exe[MAX_PATH], cmd[64] = L"schtasks.exe /Delete /TN Blackout /F";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (startupEnabled()) {
        RegDeleteValueW(key, APP_NAME);
        if (taskSet() && isElevated()) runWait(cmd);
    } else if (isElevated()) {
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        createTask(exe);
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

// "30:40,45:100" (coolant C : duty %) into curveT/curveD.
static void parseCurve(LPWSTR list) {
    WCHAR *tok, *ctx;
    curveN = 0;
    for (tok = wcstok(list, L", ", &ctx); tok && curveN < MAX_CURVE; tok = wcstok(NULL, L", ", &ctx)) {
        WCHAR *colon = wcschr(tok, L':');
        if (!colon) continue;
        int d = _wtoi(colon + 1);
        curveT[curveN] = _wtoi(tok);
        curveD[curveN++] = d < 20 ? 20 : d > 100 ? 100 : d;
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
    // [fans] channels=1,3,4  curve=30:40,45:100  (coolant C : duty %)
    WCHAR list[128], *tok, *ctx;
    fanChCount = 0;
    GetPrivateProfileStringW(L"fans", L"channels", L"", list, 128, iniPath);
    for (tok = wcstok(list, L", ", &ctx); tok && fanChCount < 7; tok = wcstok(NULL, L", ", &ctx))
        if (*tok >= L'0' && *tok <= L'6') fanCh[fanChCount++] = *tok - L'0';
    // A curve edited in the panel is kept in HKCU\Software\Blackout\Curve and wins over the ini.
    DWORD n = sizeof(list);
    if (RegGetValueW(HKEY_CURRENT_USER, APP_KEY, L"Curve", RRF_RT_REG_SZ, NULL, list, &n) != ERROR_SUCCESS)
        GetPrivateProfileStringW(L"fans", L"curve", L"30:40,45:100", list, 128, iniPath);
    parseCurve(list);
    // Max duty and hysteresis set in the panel (HKCU FanMax, Hysteresis) win over the ini.
    fanMax = (int)getDword(L"FanMax", GetPrivateProfileIntW(L"fans", L"max", 100, iniPath));
    if (fanMax < 20 || fanMax > 100) fanMax = 100;
    fanHyst = (int)getDword(L"Hysteresis", GetPrivateProfileIntW(L"fans", L"hysteresis", 2, iniPath));
    if (fanHyst < 0 || fanHyst > 10) fanHyst = 2;
    // Optional chain names for the panel, in channel order: names=Top,Side,Bottom
    GetPrivateProfileStringW(L"fans", L"names", L"", fanNames, 96, iniPath);
    linkSensor = GetPrivateProfileIntW(L"fans", L"sensor", -1, iniPath);
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
static HANDLE ioEvent;

static BOOL linkIo(BYTE *out, BYTE *in) {
    OVERLAPPED ov = {0};
    DWORD n;
    if (!ioEvent) ioEvent = CreateEventW(NULL, TRUE, FALSE, NULL); // one event for the life of the process
    ResetEvent(ioEvent);
    ov.hEvent = ioEvent;
    BOOL ok = WriteFile(link, out, 513, NULL, &ov) || GetLastError() == ERROR_IO_PENDING;
    ok = ok && WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0 && GetOverlappedResult(link, &ov, &n, FALSE);
    if (ok) {
        ResetEvent(ov.hEvent);
        ok = ReadFile(link, in, 513, NULL, &ov) || GetLastError() == ERROR_IO_PENDING;
        ok = ok && WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0 && GetOverlappedResult(link, &ov, &n, FALSE);
    }
    if (!ok) CancelIo(link);
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
    return linkOk = litOn ? linkColor(color[0], color[1], color[2]) : linkColor(0, 0, 0);
}

static BOOL linkReadMore(BYTE *in) {
    OVERLAPPED ov = {0};
    DWORD n;
    ResetEvent(ioEvent);
    ov.hEvent = ioEvent;
    BOOL ok = ReadFile(link, in, 513, NULL, &ov) || GetLastError() == ERROR_IO_PENDING;
    ok = ok && WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0 && GetOverlappedResult(link, &ov, &n, FALSE);
    if (!ok) CancelIo(link);
    return ok;
}

// Coolant temperature from the LINK hub (protocol from FanControl.CorsairLink): endpoint 21, data type 10 00.
// Reply: [4] status, [5..6] data type, [7] sensor count, then 3 bytes per sensor: status (0 = present), int16 LE C x10.
// Uses the first present sensor in a sane range, or [fans] sensor= to pick one.
static BOOL linkTemp(double *t) {
    static const BYTE close[] = {0x05, 0x01, 0x01}, open[] = {0x0d, 0x01}, read[] = {0x08, 0x01}, endpoint[] = {0x21};
    BYTE out[513] = {0}, in[513] = {0};
    BOOL ok, got = FALSE;
    if (link == INVALID_HANDLE_VALUE) link = openHid(L"vid_1b1c&pid_0c3f", L"mi_00", FILE_FLAG_OVERLAPPED);
    if (link == INVALID_HANDLE_VALUE) return FALSE;
    ok = linkCmd(close, sizeof(close), endpoint, 1) && linkCmd(open, sizeof(open), endpoint, 1);
    if (ok) {
        out[2] = 1;
        memcpy(out + 3, read, sizeof(read));
        ok = linkIo(out, in);
        for (int i = 0; ok && i < 3 && !(in[5] == 0x10 && in[6] == 0x00); i++) ok = linkReadMore(in);
    }
    if (ok && in[4] == 0 && in[5] == 0x10 && in[6] == 0x00) {
        for (int i = 0; i < in[7] && 10 + i * 3 < 513; i++) {
            if (in[8 + i * 3] != 0 || (linkSensor >= 0 && i != linkSensor)) continue;
            double c = (short)(in[9 + i * 3] | in[10 + i * 3] << 8) / 10.0;
            if (c > 5 && c < 80) {
                *t = c;
                got = TRUE;
                break;
            }
        }
    }
    if (ok) ok = linkCmd(close, sizeof(close), endpoint, 1);
    if (!ok) {
        CloseHandle(link);
        link = INVALID_HANDLE_VALUE;
    }
    return got;
}

// Motherboard fan headers on a Nuvoton NCT679x Super I/O, through PawnIO's LpcIO module (PawnIO must be installed,
// Blackout must run as admin). Register map as in Linux nct6775 and LibreHardwareMonitor: per-channel pages
// 0x100..0xB00, mode at page+0x02 (bits 7..4, 0 = manual), duty at page+0x09 (0..255), RPM at 0x4C0 + 2 * channel.
// Access is shared with HWiNFO/LHM through the standard ISA bus mutex.
typedef HRESULT(WINAPI *PioOpen)(PHANDLE);
typedef HRESULT(WINAPI *PioLoad)(HANDLE, const UCHAR *, SIZE_T);
typedef HRESULT(WINAPI *PioExec)(HANDLE, PCSTR, const ULONG64 *, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
static PioExec pioExec;
static HANDLE pio, isaMutex;
static int sioBase;
static BYTE fanSavedMode[7], fanSavedDuty[7];
static BOOL fanManual;
static const int fanPage[7] = {0x100, 0x200, 0x300, 0x800, 0x900, 0xA00, 0xB00};

static ULONG64 pioCall(PCSTR fn, ULONG64 a, ULONG64 b, SIZE_T nin) {
    ULONG64 in[2] = {a, b}, out = 0;
    SIZE_T n;
    pioExec(pio, fn, in, nin, &out, 1, &n);
    return out;
}

static BOOL isaLock(void) {
    DWORD r = WaitForSingleObject(isaMutex, 500);
    return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
}

static void hwmSelect(int reg) {
    pioCall("ioctl_pio_outb", sioBase + 5, 0x4E, 2);
    pioCall("ioctl_pio_outb", sioBase + 6, reg >> 8, 2);
    pioCall("ioctl_pio_outb", sioBase + 5, reg & 0xFF, 2);
}

static int hwmRead(int reg) {
    hwmSelect(reg);
    return (int)pioCall("ioctl_pio_inb", sioBase + 6, 0, 1);
}

static void hwmWrite(int reg, int v) {
    hwmSelect(reg);
    pioCall("ioctl_pio_outb", sioBase + 6, v, 2);
}

static BOOL sioInit(void) {
    WCHAR raw[MAX_PATH], path[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\PawnIO\\PawnIOLib.dll", path, MAX_PATH);
    fanWhy = 3;
    HMODULE lib = LoadLibraryW(path);
    if (!lib) return FALSE;
    PioOpen pOpen = (PioOpen)GetProcAddress(lib, "pawnio_open");
    PioLoad pLoad = (PioLoad)GetProcAddress(lib, "pawnio_load");
    pioExec = (PioExec)GetProcAddress(lib, "pawnio_execute");
    GetPrivateProfileStringW(L"fans", L"module", L"", raw, MAX_PATH, iniPath);
    if (raw[0]) ExpandEnvironmentStringsW(raw, path, MAX_PATH);
    else exeDir(path, L"LpcIO.bin");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (!pOpen || !pLoad || !pioExec || f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = GetFileSize(f, NULL), got = 0;
    UCHAR *blob = HeapAlloc(GetProcessHeap(), 0, size);
    BOOL ok = blob && ReadFile(f, blob, size, &got, NULL) && got == size;
    CloseHandle(f);
    ok = ok && pOpen(&pio) == S_OK && pLoad(pio, blob, size) == S_OK;
    if (blob) HeapFree(GetProcessHeap(), 0, blob);
    if (!ok) {
        if (!isElevated()) fanWhy = 2;
        return FALSE;
    }
    fanWhy = 4;
    isaMutex = CreateMutexW(NULL, FALSE, L"Global\\Access_ISABUS.HTP.Method");
    for (int slot = 0; slot < 2 && !sioBase && isaLock(); slot++) {
        int port = slot ? 0x4E : 0x2E;
        pioCall("ioctl_select_slot", slot, 0, 1);
        pioCall("ioctl_pio_outb", port, 0x87, 2);
        pioCall("ioctl_pio_outb", port, 0x87, 2);
        int id = (int)pioCall("ioctl_superio_inb", 0x20, 0, 1);
        if (id == 0xD4 || id == 0xD8) {
            pioCall("ioctl_superio_outb", 0x07, 0x0B, 2);
            int base = (int)(pioCall("ioctl_superio_inb", 0x60, 0, 1) << 8 | pioCall("ioctl_superio_inb", 0x61, 0, 1));
            int lock = (int)pioCall("ioctl_superio_inb", 0x28, 0, 1);
            if (lock & 0x10) pioCall("ioctl_superio_outb", 0x28, lock & ~0x10, 2);
            pioCall("ioctl_find_bars", 0, 0, 0);
            if (base > 0x100 && base < 0xFFF8) sioBase = base;
        }
        pioCall("ioctl_pio_outb", port, 0xAA, 2);
        ReleaseMutex(isaMutex);
    }
    if (sioBase) fanWhy = 0;
    return sioBase != 0;
}

static void fansSet(int duty) {
    if (!isaLock()) return;
    for (int i = 0; i < fanChCount; i++) {
        int page = fanPage[fanCh[i]];
        if (!fanManual) {
            fanSavedMode[i] = (BYTE)hwmRead(page + 0x02);
            fanSavedDuty[i] = (BYTE)hwmRead(page + 0x09);
            // Already manual means an earlier instance was killed before it could hand back. Restore to the BIOS
            // SmartFan IV curve (mode 4) at full duty rather than freezing the fans at a stale manual duty.
            if (!(fanSavedMode[i] & 0xF0)) fanSavedMode[i] |= 0x40, fanSavedDuty[i] = 0xFF;
        }
        hwmWrite(page + 0x02, fanSavedMode[i] & 0x0F);
        hwmWrite(page + 0x09, duty * 255 / 100);
    }
    hwmSelect(0);
    ReleaseMutex(isaMutex);
    fanManual = TRUE;
    fanDuty = duty;
    setDword(L"FanDuty", duty);
}

// Hand the headers back to the BIOS curve exactly as found.
static void fansRelease(void) {
    if (!fanManual || !isaLock()) return;
    for (int i = 0; i < fanChCount; i++) {
        hwmWrite(fanPage[fanCh[i]] + 0x09, fanSavedDuty[i]);
        hwmWrite(fanPage[fanCh[i]] + 0x02, fanSavedMode[i]);
    }
    hwmSelect(0);
    ReleaseMutex(isaMutex);
    fanManual = FALSE;
    fanDuty = -1;
    setDword(L"FanDuty", 0);
}

static int curveRaw(double t) {
    if (t <= curveT[0]) return curveD[0];
    for (int i = 1; i < curveN; i++)
        if (t <= curveT[i]) return curveD[i - 1] + (int)((curveD[i] - curveD[i - 1]) * (t - curveT[i - 1]) / (curveT[i] - curveT[i - 1]) + 0.5);
    return curveD[curveN - 1];
}

// [fans] max= caps every duty (e.g. 80 while fans are powered from 1 A motherboard headers).
static int curveAt(double t) {
    int d = curveRaw(t);
    return d > fanMax ? fanMax : d;
}

// Fan RPM for the panel only; read while it is open.
static void fansReadRpm(void) {
    if (!isaLock()) return;
    for (int i = 0; i < fanChCount; i++) fanRpm[i] = hwmRead(0x4C0 + fanCh[i] * 2) << 8 | hwmRead(0x4C1 + fanCh[i] * 2);
    hwmSelect(0);
    ReleaseMutex(isaMutex);
}

// [fans] dump=1: one-shot read-only dump of each channel's HWM page to %TEMP%\blackout-sio.txt (diagnostics).
static void sioDump(void) {
    WCHAR path[MAX_PATH];
    char line[256];
    DWORD w;
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"blackout-sio.txt");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE || !isaLock()) return;
    for (int c = 0; c < 5; c++) {
        int n = wsprintfA(line, "ch%d page %03X:", c, fanPage[c]);
        for (int r = 0; r < 0x20; r++) n += wsprintfA(line + n, " %02X", hwmRead(fanPage[c] + r));
        line[n++] = '\r', line[n++] = '\n';
        WriteFile(f, line, n, &w, NULL);
    }
    int n = wsprintfA(line, "bank0 00-1F:");
    for (int r = 0; r < 0x20; r++) n += wsprintfA(line + n, " %02X", hwmRead(r));
    line[n++] = '\r', line[n++] = '\n';
    WriteFile(f, line, n, &w, NULL);
    hwmSelect(0);
    ReleaseMutex(isaMutex);
    CloseHandle(f);
}

// Re-applies the curve to the current coolant reading straight away (used after a panel edit).
static void curveApplyNow(void) {
    if (coolant < 0 || !curveOn) return;
    int duty = curveAt(coolant);
    if (duty != fanDuty || !fanManual) fansSet(duty);
}

// Tray tooltip with the live reading, rewritten only when the whole-degree value or the duty changes.
static void tipUpdate(void) {
    static int shown = -2;
    int key = coolant < 0 ? -1 : (int)(coolant + 0.5) * 1000 + (fanDuty < 0 ? 999 : fanDuty);
    if (key == shown) return;
    shown = key;
    if (coolant < 0) lstrcpyW(nid.szTip, L"Blackout \x00B7 No water reading \x00B7 Fans on the BIOS curve");
    else if (fanDuty < 0) wsprintfW(nid.szTip, L"Blackout \x00B7 Water %d \x00B0" L"C \x00B7 Fans on the BIOS curve", (int)(coolant + 0.5));
    else wsprintfW(nid.szTip, L"Blackout \x00B7 Water %d \x00B0" L"C \x00B7 Fans %d%%", (int)(coolant + 0.5), fanDuty);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Every 4 s: coolant is averaged over ~30 s, fans go up straight away and come down only once the water is
// [fans] hysteresis C cooler. No reading for 20 s hands the fans back to the BIOS.
static void fanTick(void) {
    double t;
    if (linkTemp(&t)) {
        tempFails = 0;
        coolant = coolant < 0 ? t : coolant + (t - coolant) * 0.13;
        static DWORD written = (DWORD)-1;
        DWORD tenths = (DWORD)(coolant * 10 + 0.5);
        if (tenths != written) setDword(L"Coolant", written = tenths); // registry only when the reading changes
        history[historyPos] = (short)(coolant * 100 + 0.5);
        historyPos = (historyPos + 1) % HISTORY;
        if (historyLen < HISTORY) historyLen++;
    } else if (++tempFails >= 5) {
        coolant = -1;
        fansRelease();
        tipUpdate();
        panelUpdate();
        return;
    }
    if (panel) fansReadRpm();
    if (coolant < 0) return;
    if (!curveOn) {
        fansRelease();
        tipUpdate();
        panelUpdate();
        return;
    }
    int up = curveAt(coolant), down = curveAt(coolant + fanHyst);
    int duty = fanDuty < 0 || up > fanDuty ? up : down < fanDuty ? down : fanDuty;
    if (duty != fanDuty || !fanManual) fansSet(duty);
    tipUpdate();
    panelUpdate();
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

// The RGB switch shows what the user wants while the screens are on (HKCU RgbWant). RgbOn is the state right now,
// which also goes off with the screens when that option is ticked.
static BOOL rgbShown(void) { return litWant; }

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
        AppendMenuW(menu, MF_STRING, ID_RGB, rgbShown() ? L"RGB off" : L"RGB on");
        AppendMenuW(menu, MF_STRING | (getDword(L"Sync", 1) ? MF_CHECKED : 0), ID_SYNC, L"RGB turns off with screens");
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
    if (hasCurve) {
        HMENU curve = CreatePopupMenu();
        WCHAR label[64];
        if (coolant < 0) lstrcpyW(label, L"Water: no reading");
        else if (fanDuty < 0) wsprintfW(label, L"Water %d.%d C, BIOS fans", (int)coolant, (int)(coolant * 10) % 10);
        else wsprintfW(label, L"Water %d.%d C, fans %d%%", (int)coolant, (int)(coolant * 10) % 10, fanDuty);
        AppendMenuW(curve, MF_STRING | MF_GRAYED, 0, label);
        AppendMenuW(curve, MF_STRING | (curveOn ? MF_CHECKED : 0), ID_CURVE, L"Follow water temperature");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)curve, L"Fan curve");
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

// Wake log. While the screens are off, raw input is watched so the device behind a wake can be named. The handler
// only stores a handle and a tick; the name is looked up once, when the screens come on.
static void wakeWatch(HWND hwnd, BOOL on) {
    RAWINPUTDEVICE rid[2] = {{1, 2, on ? RIDEV_INPUTSINK : RIDEV_REMOVE, on ? hwnd : NULL}, {1, 6, on ? RIDEV_INPUTSINK : RIDEV_REMOVE, on ? hwnd : NULL}};
    RegisterRawInputDevices(rid, 2, sizeof(*rid));
    if (on) rawDevice = NULL, rawTick = 0, hotkeyCause = 0;
}

static void wakeRecord(void) {
    WCHAR cause[96] = L"", path[256], name[48] = L"";
    UINT n = 256;
    if (hotkeyCause) {
        lstrcpyW(cause, hotkeyCause == 1 ? L"Left-click (Hotkey mode)" : L"Esc (Hotkey mode)");
    } else if (rawTick && GetTickCount() - rawTick < 5000) {
        if (rawDevice && GetRawInputDeviceInfoW(rawDevice, RIDI_DEVICENAME, path, &n) != (UINT)-1) {
            HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                if (!HidD_GetProductString(h, name, sizeof(name))) name[0] = 0;
                CloseHandle(h);
            }
        }
        name[40] = 0;
        // With Hotkey mode armed, input should not have been able to wake the screens; say so if it did.
        wsprintfW(cause, L"%s%s%s%s", rawType == RIM_TYPEMOUSE ? L"Mouse" : rawType == RIM_TYPEKEYBOARD ? L"Keyboard" : L"Input device",
                  name[0] ? L": " : L"", name, hookThread ? L" (not blocked)" : L"");
    } else {
        lstrcpyW(cause, L"No input seen (Windows or an app)");
    }
    for (int i = MAX_WAKES - 1; i > 0; i--) wakes[i] = wakes[i - 1];
    GetLocalTime(&wakes[0].at);
    lstrcpyW(wakes[0].cause, cause);
    if (wakeCount < MAX_WAKES) wakeCount++;
}

// Hotkey mode: while the screens are off, low-level hooks swallow all keyboard and mouse input except the chosen
// wake inputs (left-click and/or Esc), so a bumped mouse or desk vibration can't wake the screens. The hooks live on
// a short-lived thread that exists only while the screens are off. Ctrl+Alt+Del always gets through (Windows).
#define WM_WAKE (WM_APP + 2)
static HWND mainWnd;
static BOOL wakeClick, wakeEsc;

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
    if (hookThread || !getDword(L"HotkeyMode", 0)) return;
    wakeClick = getDword(L"WakeClick", 1), wakeEsc = getDword(L"WakeEsc", 1);
    if (!wakeClick && !wakeEsc) return; // nothing could wake the screens; stay safe
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
        if (wp == TIMER_LINK) {
            // Back off if the hub isn't answering so a missing device can't keep stalling the message loop. The fan
            // curve keeps the 4 s tick, since its 20 s no-reading rule hands the fans back to the BIOS.
            BOOL ok = !linkLeds || linkRefresh();
            if (hasCurve) fanTick();
            SetTimer(hwnd, TIMER_LINK, ok || hasCurve ? 4000 : 30000, NULL);
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
        case ID_SYNC: setDword(L"Sync", !getDword(L"Sync", 1)); return 0;
        case ID_FANCTL: setDword(L"FanCtl", !getDword(L"FanCtl", 1)); applyFans(); return 0;
        case ID_CURVE:
            curveOn = !curveOn;
            setDword(L"FanCurve", curveOn);
            if (curveOn) fanTick();
            else fansRelease(), tipUpdate();
            return 0;
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
        // Sleep: give the headers back to the BIOS. The chip resets on resume, so the next tick takes over afresh.
        if (wp == PBT_APMSUSPEND) fansRelease();
        if (wp == PBT_APMRESUMEAUTOMATIC) fanManual = FALSE, fanDuty = -1;
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
                    if (hasRgb && getDword(L"Sync", 1)) setRgb(hwnd, screensOn && litWant);
                    applyFans();
                }
            }
        }
        return TRUE;
    case WM_DISPLAYCHANGE:
        // Monitors re-enumerate a few seconds after waking; restore once they have settled.
        if (screensOn && savedCount) SetTimer(hwnd, TIMER_LAYOUT, 2500, NULL);
        return 0;
    case WM_ENDSESSION:
        if (wp) fansRelease();
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
        fansRelease();
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
    Shell_NotifyIconW(NIM_ADD, &nid);
    Shell_NotifyIconW(NIM_SETVERSION, &nid);

    // Registering sends the current display state straight away, which also re-asserts the lighting at login.
    WCHAR fury[MAX_PATH];
    loadConfig();
    exeDir(fury, L"furyled.exe");
    hasOpenRgb = openrgb[0] && GetFileAttributesW(openrgb) != INVALID_FILE_ATTRIBUTES;
    hasRam = GetFileAttributesW(fury) != INVALID_FILE_ATTRIBUTES;
    setFans(-1);
    litOn = getDword(L"RgbOn", 1), litWant = getDword(L"RgbWant", 1), curveOn = getDword(L"FanCurve", 1);
    fanWhy = fanChCount && curveN ? 3 : 1;
    hasCurve = fanChCount && curveN && sioInit();
    if (hasCurve && GetPrivateProfileIntW(L"fans", L"dump", 0, iniPath)) sioDump();
    // An elevated window drops messages from normal processes unless allowed (tray clicks and remote "screens off").
    ChangeWindowMessageFilterEx(hwnd, WM_COMMAND, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(hwnd, WM_TRAY, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(hwnd, taskbarCreated, MSGFLT_ALLOW, NULL);
    if (linkLeds || hasCurve) SetTimer(hwnd, TIMER_LINK, 1000, NULL);
    hasRgb = hasOpenRgb || hasRam || linkLeds;
    if (hasRgb || hasFans || ddcIds[0]) displayNotify = RegisterPowerSettingNotification(hwnd, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
    trim();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    CloseHandle(mutex);
    return 0;
}
