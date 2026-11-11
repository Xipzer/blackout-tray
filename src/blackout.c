// Blackout: tray app for screens off. RGB, fans, DDC monitor power and window layout follow the display state.
// One hidden window. The only periodic work is the optional 4 s tick (iCUE LINK lighting keep-alive and the fan
// curve). Helpers (OpenRGB, furyled, schtasks) run once and exit. Fan chips and USB controllers are described by the
// definition files in the devices folder (devices.c).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
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
#define ID_DEVICES   9
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
#define MAX_OUT      7
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
// Fan curve: one temperature drives up to MAX_OUT fans (see the Fan hardware section).
static BOOL hasCurve;
static int outCount, curveT[MAX_CURVE], curveD[MAX_CURVE], curveN, fanHyst = 2, linkSensor = -1;
static int fanDuty = -1, tempFails, fanMax = 100, fanRpm[MAX_OUT];
static WCHAR fanNames[96], outSpec[200], srcSpec[64];
// What the curve follows, for the panel and the tooltip: "Water" from "the iCUE LINK hub" unless [fans] source= says otherwise.
static WCHAR srcLabel[24] = L"Water", srcFrom[48] = L"the iCUE LINK hub";
static double coolant = -1;
// Coolant history for the panel graph: 15 min at one sample per 4 s tick, C x100.
#define HISTORY 225
static short history[HISTORY];
static int historyLen, historyPos;
static HWND panel;
static void panelUpdate(void);
// Cached switches, so the 4 s tick never reads the registry. Written through to HKCU when changed.
static BOOL litOn = TRUE, litWant = TRUE, curveOn = TRUE, linkOk;
// Why the fan curve is off (shown in the panel): 0 active, 1 no [fans], 2 not admin, 3 PawnIO or module missing, 4 no known
// fan chip, 5 a fan or sensor named in blackout.ini is not there, 6 the definition is untested and untested=1 is not set.
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
    // [fans] outputs=sio/2,sio/4  source=link  curve=30:40,45:100  (temperature C : duty %). See docs/devices.md.
    WCHAR list[128], *tok, *ctx;
    GetPrivateProfileStringW(L"fans", L"outputs", L"", outSpec, 200, iniPath);
    if (!outSpec[0]) {
        // From before definitions: channels=1,3,4 are Nuvoton fan channels counted from 0.
        GetPrivateProfileStringW(L"fans", L"channels", L"", list, 128, iniPath);
        for (tok = wcstok(list, L", ", &ctx); tok && lstrlenW(outSpec) < 180; tok = wcstok(NULL, L", ", &ctx))
            if (*tok >= L'0' && *tok <= L'6') wsprintfW(outSpec + lstrlenW(outSpec), L"%ssio/%d", outSpec[0] ? L"," : L"", *tok - L'0' + 1);
    }
    outCount = outSpec[0] != 0;
    for (WCHAR *c = outSpec; *c; c++) outCount += *c == L',';
    if (outCount > MAX_OUT) outCount = MAX_OUT;
    GetPrivateProfileStringW(L"fans", L"source", L"link", srcSpec, 64, iniPath);
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
    // Optional names for the panel, in the order of outputs=: names=Top,Side,Bottom
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

static BOOL linkReadMore(BYTE *in) { return hidIo(link, NULL, 0, in, 513); }

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

// ---- Fan hardware ----
// What Blackout knows about a fan chip or a USB controller comes from a definition file (devices.c). This part joins
// those to Windows: Super I/O ports through PawnIO's LpcIO module (PawnIO installed, Blackout run as admin, access
// shared with HWiNFO and LibreHardwareMonitor through the standard ISA bus mutex), and HID reports.
#include "devices.c"

typedef HRESULT(WINAPI *PioOpen)(PHANDLE);
typedef HRESULT(WINAPI *PioLoad)(HANDLE, const UCHAR *, SIZE_T);
typedef HRESULT(WINAPI *PioExec)(HANDLE, PCSTR, const ULONG64 *, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
typedef struct { HANDLE h; OVERLAPPED ov; BOOL reading; BYTE buf[DEV_REPORT]; } HidHost;
typedef struct { char dev[40], item[DEV_NAME]; } FanRef;
static PioExec pioExec;
static HANDLE isaMutex;
static struct { Dev *dev; int fan; } outs[MAX_OUT];
static Dev *srcDev; // NULL = the iCUE LINK hub's coolant sensor
static int srcTemp;
static BOOL fanManual;
// Every definition, while setup or --devices is running. Freed afterwards.
static Dev *defs;
static int defCount;

// What a motherboard fan's registers held before Blackout took it is also noted in a registry key that lasts until
// shutdown (HKLM, volatile). If Blackout is killed while it holds a fan, the next start finds the fan in manual mode
// and, with the note, can still hand it back as the BIOS had it. Without a note, manual mode is the BIOS's own setting.
#define HELD_KEY L"SOFTWARE\\Blackout\\Held"

static BOOL heldRead(int i, BYTE *was) {
    WCHAR name[80];
    DWORD v, n = sizeof(v);
    wsprintfW(name, L"%S/%d", outs[i].dev->file, outs[i].fan + 1);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, HELD_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &n) != ERROR_SUCCESS) return FALSE;
    was[0] = (BYTE)v, was[1] = (BYTE)(v >> 8), was[2] = (BYTE)(v >> 16);
    return TRUE;
}

static void heldWrite(int i, BOOL held) {
    const DevFan *f = &outs[i].dev->fans[outs[i].fan];
    WCHAR name[80];
    HKEY key;
    DWORD v = f->savedMode | f->savedDuty << 8 | f->savedEnable << 16;
    wsprintfW(name, L"%S/%d", outs[i].dev->file, outs[i].fan + 1);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, HELD_KEY, 0, NULL, REG_OPTION_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) return;
    if (held) RegSetValueExW(key, name, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
    else RegDeleteValueW(key, name);
    RegCloseKey(key);
}

static ULONG64 pioCall(HANDLE h, PCSTR fn, ULONG64 a, ULONG64 b, SIZE_T nin) {
    ULONG64 in[2] = {a, b}, out = 0;
    SIZE_T n;
    pioExec(h, fn, in, nin, &out, 1, &n);
    return out;
}

static int hostIn(void *h, int port) { return (int)pioCall(h, "ioctl_pio_inb", port, 0, 1); }
static void hostOut(void *h, int port, int v) { pioCall(h, "ioctl_pio_outb", port, v, 2); }
static void hostSlot(void *h, int slot) { pioCall(h, "ioctl_select_slot", slot, 0, 1); }
static void hostBars(void *h) { pioCall(h, "ioctl_find_bars", 0, 0, 0); }

static BOOL isaLock(void) {
    DWORD r = WaitForSingleObject(isaMutex, 500);
    return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
}

// A PawnIO handle with the LpcIO module loaded, or NULL with fanWhy saying why. One handle drives one config port.
static HANDLE pioOpen(void) {
    WCHAR raw[MAX_PATH], path[MAX_PATH];
    HANDLE h = NULL;
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\PawnIO\\PawnIOLib.dll", path, MAX_PATH);
    fanWhy = 3;
    HMODULE lib = LoadLibraryW(path);
    if (!lib) return NULL;
    PioOpen pOpen = (PioOpen)GetProcAddress(lib, "pawnio_open");
    PioLoad pLoad = (PioLoad)GetProcAddress(lib, "pawnio_load");
    pioExec = (PioExec)GetProcAddress(lib, "pawnio_execute");
    GetPrivateProfileStringW(L"fans", L"module", L"", raw, MAX_PATH, iniPath);
    if (raw[0]) ExpandEnvironmentStringsW(raw, path, MAX_PATH);
    else exeDir(path, L"LpcIO.bin");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(f, NULL), got = 0;
    UCHAR *blob = HeapAlloc(GetProcessHeap(), 0, size);
    BOOL ok = pOpen && pLoad && pioExec && blob && ReadFile(f, blob, size, &got, NULL) && got == size;
    CloseHandle(f);
    ok = ok && pOpen(&h) == S_OK && pLoad(h, blob, size) == S_OK;
    if (blob) HeapFree(GetProcessHeap(), 0, blob);
    if (ok) return h;
    if (h) CloseHandle(h);
    if (!isElevated()) fanWhy = 2;
    return NULL;
}

static void hidDrop(HidHost *s) {
    if (s->h != INVALID_HANDLE_VALUE) {
        CancelIo(s->h);
        CloseHandle(s->h);
        s->h = INVALID_HANDLE_VALUE;
    }
    s->reading = FALSE;
}

static int hostHid(void *host, const unsigned char *out, int outLen, unsigned char *in, int inLen) {
    HidHost *s = host;
    if (s->h == INVALID_HANDLE_VALUE) return 0;
    if (hidIo(s->h, out, outLen, in, inLen)) return 1;
    hidDrop(s); // unplugged or not answering: opened again on the next use
    return 0;
}

// Opens the HID interface a definition describes: its VID, one of its PIDs, the interface text if given, and reports
// large enough for the definition's commands and fields. Report sizes come from Windows, not from the file.
static BOOL hidOpen(Dev *d) {
    HidHost *s = d->host;
    GUID g;
    int cmd = d->set.len > d->rpmCmd.len ? d->set.len : d->rpmCmd.len;
    if (d->tempCmd.len > cmd) cmd = d->tempCmd.len;
    HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return FALSE;
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
    for (DWORD i = 0; s->h == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &g, i, &ifd); i++) {
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; BYTE raw[1024]; } u;
        WCHAR id[24], iface[16];
        BOOL match = FALSE;
        PHIDP_PREPARSED_DATA parsed;
        HIDP_CAPS caps;
        u.d.cbSize = sizeof(u.d);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, &u.d, sizeof(u), NULL, NULL)) continue;
        CharLowerW(u.d.DevicePath);
        for (int p = 0; p < d->pids && !match; p++) {
            wsprintfW(id, L"vid_%04x&pid_%04x", d->vid, d->pid[p]);
            match = wcsstr(u.d.DevicePath, id) != NULL;
        }
        if (match && d->iface[0]) {
            MultiByteToWideChar(CP_ACP, 0, d->iface, -1, iface, 16);
            match = wcsstr(u.d.DevicePath, iface) != NULL;
        }
        if (!match) continue;
        HANDLE h = CreateFileW(u.d.DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (HidD_GetPreparsedData(h, &parsed)) {
            if (HidP_GetCaps(parsed, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength >= d->inNeed && caps.InputReportByteLength <= DEV_REPORT &&
                caps.OutputReportByteLength <= DEV_REPORT && (!cmd || caps.OutputReportByteLength > cmd)) {
                s->h = h;
                d->inLen = caps.InputReportByteLength ? caps.InputReportByteLength - 1 : 0;
                d->outLen = caps.OutputReportByteLength ? caps.OutputReportByteLength - 1 : 0;
            }
            HidD_FreePreparsedData(parsed);
        }
        if (s->h == INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (s->h == INVALID_HANDLE_VALUE) return FALSE;
    // A controller that reports on its own: keep only the newest reports queued, so a 4 s tick never reads old ones.
    if (d->fanIn.listen || d->tempIn.listen) HidD_SetNumInputBuffers(s->h, 2);
    return TRUE;
}

// Takes in the reports a controller has sent on its own since the last call, without waiting (or for up to wait ms
// for the first one). One read is always left pending. FALSE = the device is gone.
static BOOL hidPump(Dev *d, DWORD wait) {
    HidHost *s = d->host;
    DWORD n;
    for (int i = 0; i < 16; i++) {
        if (!s->reading) {
            ResetEvent(s->ov.hEvent);
            if (!ReadFile(s->h, s->buf, d->inLen + 1, NULL, &s->ov) && GetLastError() != ERROR_IO_PENDING) break;
            s->reading = TRUE;
        }
        if (wait) WaitForSingleObject(s->ov.hEvent, wait), wait = 0;
        if (!GetOverlappedResult(s->h, &s->ov, &n, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE) return TRUE;
            break;
        }
        s->reading = FALSE;
        devHidReport(d, s->buf, (int)n);
    }
    if (!s->reading) return TRUE; // 16 reports and still more: enough for now
    hidDrop(s);
    return FALSE;
}

static BOOL hidAttach(Dev *d) {
    HidHost *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HidHost));
    if (!s) return FALSE;
    s->h = INVALID_HANDLE_VALUE;
    s->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    d->host = s;
    return TRUE;
}

static void hidDetach(Dev *d) {
    HidHost *s = d->host;
    hidDrop(s);
    CloseHandle(s->ov.hEvent);
    HeapFree(GetProcessHeap(), 0, s);
    d->host = NULL;
}

// Before a device is used in a tick. A USB controller is opened again if it went away, and its own reports taken in.
static BOOL devReady(Dev *d) {
    HidHost *s = d->host;
    if (d->type == DEV_SIO) return TRUE;
    if (s->h == INVALID_HANDLE_VALUE && !hidOpen(d)) return FALSE;
    return !(d->fanIn.listen || d->tempIn.listen) || hidPump(d, 0);
}

static void defAdd(char *text, const char *name, char *errors, int cap) {
    if (!(defCount % 16)) {
        SIZE_T size = (defCount + 16) * sizeof(Dev);
        Dev *more = defs ? HeapReAlloc(GetProcessHeap(), 0, defs, size) : HeapAlloc(GetProcessHeap(), 0, size);
        if (!more) return;
        defs = more;
    }
    if (devParse(&defs[defCount], text, name)) defCount++;
    else if (errors && lstrlenA(errors) + lstrlenA(name) + lstrlenA(devErr) + 12 < cap) wsprintfA(errors + lstrlenA(errors), "  %s.ini: %s\r\n", name, devErr);
}

// Reads every definition in the devices folder beside the exe, then the built-in one. Files that don't parse are
// skipped, and listed in errors when the caller wants to know.
static void defsLoad(char *errors, int cap) {
    enum { LIMIT = 16384 };
    WCHAR path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    char name[64], *text = HeapAlloc(GetProcessHeap(), 0, LIMIT + 1);
    if (!text) return;
    exeDir(path, L"devices\\*.ini");
    HANDLE find = FindFirstFileW(path, &fd);
    for (BOOL more = find != INVALID_HANDLE_VALUE; more; more = FindNextFileW(find, &fd)) {
        DWORD got = 0;
        int len = lstrlenW(fd.cFileName) - 4;
        if (len < 1 || len > 39 || fd.nFileSizeLow > LIMIT || fd.nFileSizeHigh) continue;
        exeDir(path, L"devices\\");
        lstrcatW(path, fd.cFileName);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        BOOL ok = ReadFile(f, text, LIMIT, &got, NULL);
        CloseHandle(f);
        if (!ok) continue;
        text[got] = 0;
        WideCharToMultiByte(CP_ACP, 0, fd.cFileName, len, name, sizeof(name), NULL, NULL);
        name[len] = 0;
        CharLowerA(name);
        defAdd(text, name, errors, cap);
    }
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
    memcpy(text, devBuiltin, sizeof(devBuiltin));
    defAdd(text, "builtin", NULL, 0);
    HeapFree(GetProcessHeap(), 0, text);
}

// "sio/2" or "corsair-commander-pro/Fan 1" into its two halves.
static BOOL refParse(char *spec, FanRef *r) {
    char *slash = strchr(spec, '/');
    if (!slash) return FALSE;
    *slash = 0;
    return dvText(r->dev, sizeof(r->dev), dvTrim(spec)) && dvText(r->item, sizeof(r->item), dvTrim(slash + 1));
}

// The definition a name in blackout.ini means: "sio" is the fan chip that was found, anything else a file name.
static Dev *defFind(const char *name, int chip0, int chip1) {
    if (dvSame(name, "sio")) return chip0 >= 0 ? &defs[chip0] : chip1 >= 0 ? &defs[chip1] : NULL;
    for (int i = 0; i < defCount; i++)
        if (dvSame(defs[i].file, name)) return &defs[i];
    return NULL;
}

// Works out what [fans] in blackout.ini means on this machine and gets that hardware ready. FALSE, with fanWhy set,
// when the curve can't run. Afterwards only the definitions in use are still in memory.
static BOOL fansInit(void) {
    char spec[200], src[64], *list = spec, *tok;
    FanRef refs[MAX_OUT + 1]; // the outputs, then the source
    Dev *used[MAX_OUT + 1], *kept[MAX_OUT + 1];
    HANDLE pio[2] = {NULL, NULL};
    int n = 0, total, chip[2] = {-1, -1}, id;
    BOOL untested = GetPrivateProfileIntW(L"fans", L"untested", 0, iniPath), needSio = FALSE, second = FALSE, ok = FALSE;
    WideCharToMultiByte(CP_ACP, 0, outSpec, -1, spec, sizeof(spec), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, srcSpec, -1, src, sizeof(src), NULL, NULL);
    fanWhy = 5;
    while (n < MAX_OUT && (tok = dvCut(&list, ',')))
        if (*tok && !refParse(tok, &refs[n++])) return FALSE;
    total = n;
    if (!dvSame(dvTrim(src), "link") && !refParse(src, &refs[total++])) return FALSE;
    outCount = n;
    if (!n) return FALSE;
    defsLoad(NULL, 0);
    for (int i = 0; i < total; i++) {
        Dev *d = defFind(refs[i].dev, -1, -1);
        needSio |= dvSame(refs[i].dev, "sio") || (d && d->type == DEV_SIO);
    }
    if (needSio) {
        if (!(pio[0] = pioOpen())) goto done;
        fanWhy = 4;
        isaMutex = CreateMutexW(NULL, FALSE, L"Global\\Access_ISABUS.HTP.Method");
        if (!isaLock()) goto done;
        chip[0] = sioProbe(pio[0], 0, defs, defCount, &id);
        // The second config port is looked at only when something in blackout.ini is not behind the first.
        for (int i = 0; i < total; i++) {
            Dev *d = defFind(refs[i].dev, chip[0], -1);
            second |= dvSame(refs[i].dev, "sio") ? chip[0] < 0 : d && d->type == DEV_SIO && !d->host;
        }
        if (second && (pio[1] = chip[0] < 0 ? pio[0] : pioOpen())) chip[1] = sioProbe(pio[1], 1, defs, defCount, &id);
        ReleaseMutex(isaMutex);
    }
    for (int i = 0; i < total; i++) {
        Dev *d = used[i] = defFind(refs[i].dev, chip[0], chip[1]);
        BOOL sio = dvSame(refs[i].dev, "sio") || (d && d->type == DEV_SIO);
        fanWhy = sio ? 4 : 5;
        if (!d || (sio && !d->host)) goto done;
        fanWhy = 5;
        // A USB controller that is not there yet (early in logon) is opened by a later tick.
        if (d->type == DEV_HID && !d->host && !hidAttach(d)) goto done;
        if (i < n) {
            outs[i].fan = devFindFan(d, refs[i].item);
            if (outs[i].fan < 0 || !d->fans[outs[i].fan].control) goto done;
            fanWhy = 6;
            if (!d->verified && !untested) goto done;
        } else if ((srcTemp = devFindTemp(d, refs[i].item)) < 0) {
            goto done;
        }
    }
    ok = TRUE;
    for (int i = 0; i < total; i++) {
        kept[i] = NULL;
        for (int k = 0; k < i; k++)
            if (used[k] == used[i]) kept[i] = kept[k];
        if (!kept[i] && (kept[i] = HeapAlloc(GetProcessHeap(), 0, sizeof(Dev)))) *kept[i] = *used[i];
        ok = ok && kept[i];
    }
done:
    for (int i = 0; i < defCount; i++) {
        BOOL inUse = FALSE;
        for (int k = 0; ok && k < total; k++) inUse |= used[k] == &defs[i];
        if (defs[i].type == DEV_HID && defs[i].host && !inUse) hidDetach(&defs[i]);
    }
    for (int s = 0; s < 2; s++) {
        BOOL inUse = FALSE;
        for (int k = 0; ok && k < total; k++) inUse |= used[k]->host == pio[s];
        if (pio[s] && !inUse && (!s || pio[1] != pio[0])) CloseHandle(pio[s]);
    }
    if (defs) HeapFree(GetProcessHeap(), 0, defs);
    defs = NULL, defCount = 0;
    if (!ok) return FALSE;
    for (int i = 0; i < n; i++) outs[i].dev = kept[i];
    if (total > n) {
        srcDev = kept[n];
        MultiByteToWideChar(CP_ACP, 0, srcDev->temps[srcTemp].name, -1, srcLabel, 24);
        MultiByteToWideChar(CP_ACP, 0, srcDev->name, -1, srcFrom, 48);
    }
    fanWhy = 0;
    return TRUE;
}

// The temperature that drives the curve.
static BOOL tempRead(double *t) {
    BOOL ok;
    if (!srcDev) return linkTemp(t);
    if (srcDev->type == DEV_SIO && !isaLock()) return FALSE;
    ok = devReady(srcDev) && devTempRead(srcDev, srcTemp, t) && *t > 0 && *t < 120;
    devEnd(srcDev);
    if (srcDev->type == DEV_SIO) ReleaseMutex(isaMutex);
    return ok;
}

// Chips are left the way other software expects after each run of accesses to one device (devEnd).
static void fansSet(int duty) {
    BOOL all = TRUE, ready = FALSE;
    if (isaMutex && !isaLock()) return;
    for (int i = 0; i < outCount; i++) {
        Dev *d = outs[i].dev;
        if (!i || d != outs[i - 1].dev) ready = devReady(d) && devSafe(d);
        if (ready && d->type == DEV_SIO && !d->fans[outs[i].fan].taken) {
            BYTE was[3];
            devFanSave(d, outs[i].fan, heldRead(i, was) ? was : NULL);
            heldWrite(i, TRUE);
        }
        if (ready && devFanSet(d, outs[i].fan, duty)) fanManual = TRUE;
        else all = FALSE;
        if (i == outCount - 1 || d != outs[i + 1].dev) devEnd(d);
    }
    if (isaMutex) ReleaseMutex(isaMutex);
    if (!all) return; // a fan was not reached: fanDuty stays as it was, so the next tick tries again
    fanDuty = duty;
    setDword(L"FanDuty", duty);
}

// Hand the fans back exactly as found (motherboard headers return to the BIOS curve).
static void fansRelease(void) {
    if (!fanManual || (isaMutex && !isaLock())) return;
    for (int i = 0; i < outCount; i++) {
        devFanRelease(outs[i].dev, outs[i].fan);
        if (outs[i].dev->type == DEV_SIO) heldWrite(i, FALSE);
        if (i == outCount - 1 || outs[i].dev != outs[i + 1].dev) devEnd(outs[i].dev);
    }
    if (isaMutex) ReleaseMutex(isaMutex);
    fanManual = FALSE;
    fanDuty = -1;
    setDword(L"FanDuty", 0);
}

// After sleep the chip is back under BIOS control: what was saved before no longer describes it.
static void fansForget(void) {
    for (int i = 0; i < outCount; i++) outs[i].dev->fans[outs[i].fan].taken = 0;
    fanManual = FALSE;
    fanDuty = -1;
}

// Fan RPM for the panel only; read while it is open.
static void fansReadRpm(void) {
    BOOL ready = FALSE;
    if (isaMutex && !isaLock()) return;
    for (int i = 0; i < outCount; i++) {
        Dev *d = outs[i].dev;
        if (!i || d != outs[i - 1].dev) ready = devReady(d);
        fanRpm[i] = ready ? devFanRpm(d, outs[i].fan) : 0;
        if (i == outCount - 1 || d != outs[i + 1].dev) devEnd(d);
    }
    if (isaMutex) ReleaseMutex(isaMutex);
}

static int reportDev(char *out, int n, Dev *d) {
    double c;
    n += wsprintfA(out + n, "%s (%s%s, %s)\r\n", d->name, d->file, lstrcmpA(d->file, "builtin") ? ".ini" : "", d->verified ? "verified" : "untested");
    for (int i = 0; i < d->fanCount; i++) {
        DevFan *f = &d->fans[i];
        n += wsprintfA(out + n, "  fan %2d   %-22s %5d rpm", i + 1, f->name, devFanRpm(d, i));
        if (d->type == DEV_SIO && f->control) {
            int mode = sioRead(d, f->mode);
            n += wsprintfA(out + n, "   duty %3d%%   mode %02X", sioRead(d, f->duty) * 100 / 255, mode);
        }
        n += wsprintfA(out + n, "   %s\r\n", f->control ? "can be controlled" : "read only");
    }
    for (int i = 0; i < d->tempCount; i++) {
        n += wsprintfA(out + n, "  temp %2d  %-22s ", i + 1, d->temps[i].name);
        if (devTempRead(d, i, &c)) {
            int tenths = (int)(c * 10 + (c < 0 ? -0.5 : 0.5));
            n += wsprintfA(out + n, "%s%d.%d C\r\n", tenths < 0 ? "-" : "", (tenths < 0 ? -tenths : tenths) / 10, (tenths < 0 ? -tenths : tenths) % 10);
        } else {
            n += wsprintfA(out + n, "no reading\r\n");
        }
    }
    if (d->guards) n += wsprintfA(out + n, "  %s\r\n", devSafe(d) ? "guard: control allowed" : "guard: control refused, the chip is not in the state this definition assumes");
    devEnd(d);
    return n;
}

// blackout.exe --devices: what Blackout sees on this machine, written to %TEMP%\blackout-devices.txt and opened.
// Reads only. Safe beside a running Blackout (same mutex).
static int devicesReport(void) {
    enum { REPORT = 65536, ERRORS = 8192 };
    WCHAR path[MAX_PATH];
    DWORD w;
    char *out = HeapAlloc(GetProcessHeap(), 0, REPORT), *errors = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ERRORS);
    int n, id;
    if (!out || !errors) return 1;
    loadConfig();
    defsLoad(errors, ERRORS);
    n = wsprintfA(out, "Blackout device report\r\n%d definitions read from the devices folder\r\n", defCount - 1);
    n += wsprintfA(out + n, "In blackout.ini: outputs=sio/<fan number>,...  and  source=sio/<temp name>  (docs/devices.md)\r\n");
    HANDLE pio = pioOpen();
    if (!pio) n += wsprintfA(out + n, "\r\nMotherboard fan chips: not read. %s\r\n", fanWhy == 2 ? "Run this as admin." : "PawnIO or LpcIO.bin is missing.");
    isaMutex = CreateMutexW(NULL, FALSE, L"Global\\Access_ISABUS.HTP.Method");
    for (int slot = 0; pio && slot < 2 && isaLock(); slot++) {
        int m = sioProbe(pio, slot, defs, defCount, &id);
        n += wsprintfA(out + n, "\r\nConfig port %s: ", slot ? "4E" : "2E");
        if (m >= 0) n += wsprintfA(out + n, "chip %04X at %03X, ", id, defs[m].base), n = reportDev(out, n, &defs[m]);
        else if (id) n += wsprintfA(out + n, "chip %04X has no definition. docs/devices.md shows how to add one.\r\n", id);
        else n += wsprintfA(out + n, "no chip\r\n");
        ReleaseMutex(isaMutex);
    }
    for (int i = 0; i < defCount && n < REPORT - 4096; i++) {
        Dev *d = &defs[i];
        if (d->type != DEV_HID || !hidAttach(d)) continue;
        if (hidOpen(d)) {
            if (d->fanIn.listen || d->tempIn.listen) hidPump(d, 1500);
            n += wsprintfA(out + n, "\r\nUSB %04X: ", d->vid);
            n = reportDev(out, n, d);
        }
        hidDetach(d);
    }
    if (errors[0]) n += wsprintfA(out + n, "\r\nDefinitions that could not be read:\r\n"), lstrcpynA(out + n, errors, REPORT - n), n += lstrlenA(out + n);
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"blackout-devices.txt");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 1;
    WriteFile(f, out, n, &w, NULL);
    CloseHandle(f);
    if (!wcsstr(GetCommandLineW(), L"quiet")) ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    return 0;
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

// Re-applies the curve to the current reading straight away (used after a panel edit).
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
    if (coolant < 0) lstrcpyW(nid.szTip, L"Blackout \x00B7 No temperature reading \x00B7 Fans on the BIOS curve");
    else if (fanDuty < 0) wsprintfW(nid.szTip, L"Blackout \x00B7 %s %d \x00B0" L"C \x00B7 Fans on the BIOS curve", srcLabel, (int)(coolant + 0.5));
    else wsprintfW(nid.szTip, L"Blackout \x00B7 %s %d \x00B0" L"C \x00B7 Fans %d%%", srcLabel, (int)(coolant + 0.5), fanDuty);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Every 4 s: the temperature is averaged over ~30 s, fans go up straight away and come down only once it is
// [fans] hysteresis C cooler. No reading for 20 s hands the fans back to the BIOS.
static void fanTick(void) {
    double t;
    if (tempRead(&t)) {
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
        if (coolant < 0) lstrcpyW(label, L"No temperature reading");
        else if (fanDuty < 0) wsprintfW(label, L"%s %d.%d C, BIOS fans", srcLabel, (int)coolant, (int)(coolant * 10) % 10);
        else wsprintfW(label, L"%s %d.%d C, fans %d%%", srcLabel, (int)coolant, (int)(coolant * 10) % 10, fanDuty);
        AppendMenuW(curve, MF_STRING | MF_GRAYED, 0, label);
        AppendMenuW(curve, MF_STRING | (curveOn ? MF_CHECKED : 0), ID_CURVE, L"Follow the temperature");
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
        case ID_DEVICES: {
            // Write the device report from this (elevated) instance, without opening it. For checking a machine remotely.
            WCHAR cmd[MAX_PATH + 32] = L"\"";
            GetModuleFileNameW(NULL, cmd + 1, MAX_PATH);
            lstrcatW(cmd, L"\" --devices quiet");
            spawn(cmd);
            return 0;
        }
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
        if (wp == PBT_APMRESUMEAUTOMATIC) fansForget();
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
    if (wcsstr(cmd, L"--devices")) return devicesReport();
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
    hasCurve = outCount && curveN && fansInit();
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
