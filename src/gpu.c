// Native NVIDIA overclock profiles. Included by blackout.c (one translation unit, shares its state).
//
// A profile is a clock curve (one frequency offset per voltage point), a power limit and a memory offset. Blackout
// writes them, and the XBAR (crossbar clock) offset, straight to the driver through nvapi64.dll, the driver's own
// library, so no tuning app has to run. The driver keeps them until it resets or the PC restarts, so they are sent
// again after startup and after a driver reset (a System event subscription, nothing is polled).
//
// These are private driver interfaces that the community worked out; docs/gpu.md lists the layouts and sources.
// Rules learned the hard way: read, change, write back (a zeroed buffer is ignored); a memory write zeroes every curve
// offset, so the order is power, memory, XBAR, curve; and writing needs admin. nvapi64.dll is loaded only while
// something is being read or written.

#define MAX_XBAR     8
#define NV_SLOTS     255
#define NV_KEEP      0x7FFFFFFF
#define WM_GPU_RESET (WM_APP + 4)
#define WM_GPU_DONE  (WM_APP + 5)
#define GPU_KEEP     2 // gpuStart job: GPU_KEEP + N keeps what the card is running now as profile N
#define GPU_QUERY    L"*[System[(Provider[@Name='Display'] and EventID=4101) or (Provider[@Name='nvlddmkm'] and (EventID=14 or EventID=153))]]"

typedef void *(*NvQuery)(unsigned);
typedef int (*NvCall)(void *, void *);
// Clock curve tables: info says what each slot is, control holds the offsets. Core points come first.
typedef struct {
    BYTE info[0x182C], ctl[0x2420];
    int slot[NV_SLOTS], count;
} NvCurve;

static void loadConfig(void);
static HMODULE nvLib;
static NvQuery nvq;
static void *nvGpu;
static int gpuProfiles, xbarSteps[MAX_XBAR], xbarCount, xbarDefault;
// For the panel: -1 = nothing sent yet, 0 = sent, anything else = what went wrong (gpuNote says it in words).
static int gpuState = -1, xbarState = -1, xbarSent;
static SYSTEMTIME gpuAt, xbarSentAt;
// For the panel: what it is waiting for (0 nothing, 1 a send, 2 the card being read into a profile), and the last
// profile kept from the card: its number, 0 or what went wrong, and when.
static int gpuWait, gpuKept, gpuKeptState, gpuKeepSlot, gpuSentSlot;
static SYSTEMTIME gpuKeptAt;
static WCHAR gpuNote[48], gpuNames[6][24];
// The card's own curve (MHz at each core point's mV), for the panel's chart. Read once, when the offsets are zero.
static int gpuBase[NV_SLOTS], gpuVolt[NV_SLOTS], gpuPoints;
static volatile LONG gpuBusy;
static HWND gpuWnd;

static BOOL hasGpu(void) {
    return gpuProfiles || xbarCount;
}

// The last send was refused, so something that should be on the card is not.
static BOOL gpuBad(void) {
    return hasGpu() && (gpuState == -2 || (gpuProfiles && gpuState && gpuState != -1) || (xbarCount && xbarState && xbarState != -1));
}

// The panel offers its GPU tab, with an empty first profile, on any PC that has the NVIDIA driver's library.
static BOOL gpuOffered(void) {
    WCHAR path[MAX_PATH + 16];
    if (hasGpu()) return TRUE;
    path[GetSystemDirectoryW(path, MAX_PATH)] = 0;
    lstrcatW(path, L"\\nvapi64.dll");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void *nvAlloc(SIZE_T n) {
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n);
}
static void nvFree(void *p) {
    HeapFree(GetProcessHeap(), 0, p);
}

// The first NVIDIA GPU. nvapi64.dll ships with the driver in System32.
static BOOL nvOpen(void) {
    void *gpus[64];
    unsigned n = 0;
    nvLib = LoadLibraryExW(L"nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nvLib) return FALSE;
    nvq = (NvQuery)GetProcAddress(nvLib, "nvapi_QueryInterface");
    if (nvq && nvq(0x0150E828) && !((int (*)(void))nvq(0x0150E828))() && nvq(0xE5AC921F) &&
        !((int (*)(void **, unsigned *))nvq(0xE5AC921F))(gpus, &n) && n) {
        nvGpu = gpus[0];
        return TRUE;
    }
    FreeLibrary(nvLib);
    nvLib = NULL;
    return FALSE;
}

static void nvClose(void) {
    if (nvq(0xD22BDD7E)) ((int (*)(void))nvq(0xD22BDD7E))();
    FreeLibrary(nvLib);
    nvLib = NULL;
}

static int nv(unsigned id, void *buf) {
    NvCall f = (NvCall)nvq(id);
    return f ? f(nvGpu, buf) : -3;
}

static int *nvDelta(NvCurve *c, int slot) {
    return (int *)(c->ctl + 0x58 + slot * 0x24);
}

static int nvCurveGet(NvCurve *c) {
    ZeroMemory(c, sizeof(*c));
    *(DWORD *)c->info = 0x0001182C;
    int r = nv(0x507B4B59, c->info);
    if (r) return r;
    *(DWORD *)c->ctl = 0x00012420;
    CopyMemory(c->ctl + 4, c->info + 4, 32);
    if ((r = nv(0x23F1B133, c->ctl))) return r;
    for (int i = 0; i < NV_SLOTS; i++) {
        DWORD *point = (DWORD *)(c->info + 0x44 + i * 24);
        BOOL used = (((DWORD *)(c->info + 4))[i / 32] >> (i % 32)) & 1;
        if (used && point[0] == 0 && point[1] == 1) c->slot[c->count++] = i; // a core point, set by voltage
    }
    return 0;
}

// -1000: the profile was saved on a card with a different number of points. -1001: the driver took it but reads back differently.
static int nvCurveSet(const int *delta, int count) {
    NvCurve *c = nvAlloc(sizeof(NvCurve));
    int r = nvCurveGet(c);
    if (!r && c->count != count) r = -1000;
    if (!r) {
        for (int i = 0; i < count; i++) *nvDelta(c, c->slot[i]) = delta[i];
        r = nv(0x0733E009, c->ctl);
    }
    if (!r && !(r = nvCurveGet(c)))
        for (int i = 0; i < count; i++)
            if (*nvDelta(c, c->slot[i]) != delta[i]) r = -1001;
    nvFree(c);
    return r;
}

// Power limit in thousandths of a percent of the card's default, kept inside what the card allows.
static int nvPower(int milli, int *now) {
    DWORD info[46] = {184 | 1 << 16}, st[18] = {72 | 1 << 16};
    int r = nv(0x34206D86, info);
    if (!r) r = nv(0x70916171, st);
    if (r) return r;
    if (now) *now = (int)st[4];
    if (milli == NV_KEEP) return 0;
    st[4] = (DWORD)(milli < (int)info[5] ? (int)info[5] : milli > (int)info[11] ? (int)info[11] : milli);
    return nv(0xAD95F5ED, st);
}

// Memory clock offset in kHz on the top performance state. Writing it zeroes the clock curve.
static int nvMemory(int khz, int *now) {
    DWORD *p = nvAlloc(7416);
    int r;
    p[0] = 7416 | 3 << 16;
    if (now) {
        *now = 0;
        if (!(r = nv(0x6FF81213, p)))
            for (DWORD c = 0; c < p[3] && c < 8; c++)
                if (p[7 + c * 11] == 4) *now = (int)p[10 + c * 11];
    } else {
        p[2] = 1, p[3] = 1, p[7] = 4, p[10] = (DWORD)khz;
        r = nv(0x0F4DAE6B, p);
    }
    nvFree(p);
    return r;
}

// XBAR offset in kHz: clock domain 1 of the domains table (layout checked on RTX 5090, drivers 572 to 621).
static int nvXbar(int khz, int *now) {
    BYTE *b = nvAlloc(0x13000);
    *(DWORD *)b = 0x000261A4;
    *(DWORD *)(b + 8) = 0xFF;
    int r = nv(0xF58938F5, b);
    if (!r && now) *now = *(int *)(b + 0x53C);
    if (!r && khz != NV_KEEP) {
        *(int *)(b + 0x53C) = khz;
        r = nv(0xD14B69CF, b);
    }
    nvFree(b);
    return r;
}

static void nvBaseRead(void) {
    NvCurve *c = nvAlloc(sizeof(NvCurve));
    BYTE *v = nvAlloc(0x1C28);
    *(DWORD *)v = 0x00011C28;
    if (!nvCurveGet(c)) {
        CopyMemory(v + 4, c->info + 4, 32);
        if (!nv(0x21537AD4, v)) {
            for (int i = 0; i < c->count; i++) {
                DWORD *e = (DWORD *)(v + 0x44 + c->slot[i] * 28);
                gpuBase[i] = ((int)e[1] - *nvDelta(c, c->slot[i])) / 1000, gpuVolt[i] = (int)e[2] / 1000;
            }
            gpuPoints = c->count;
        }
    }
    nvFree(v);
    nvFree(c);
}

// ---- Profiles in blackout.ini: [gpu.1] name=  power=  memory=  curve=offset,offset,... (kHz, one per core point) ----

static int gpuLoad(int n, int *power, int *memory, int *delta) {
    WCHAR sec[16], *list = nvAlloc(4096 * sizeof(WCHAR)), *at = list;
    int count = 0;
    wsprintfW(sec, L"gpu.%d", n);
    GetPrivateProfileStringW(sec, L"curve", L"", list, 4096, iniPath);
    while (*at && count < NV_SLOTS) {
        delta[count++] = _wtoi(at);
        while (*at && *at != L',') at++;
        if (*at) at++;
    }
    *power = (int)GetPrivateProfileIntW(sec, L"power", 0, iniPath);
    *memory = (int)GetPrivateProfileIntW(sec, L"memory", 0, iniPath);
    nvFree(list);
    return count;
}

static void gpuConfig(void) {
    WCHAR list[128], sec[16], *tok, *ctx;
    GetPrivateProfileStringW(L"gpu", L"xbar", L"", list, 128, iniPath);
    xbarCount = 0;
    for (tok = wcstok(list, L",", &ctx); tok && xbarCount < MAX_XBAR; tok = wcstok(NULL, L",", &ctx)) {
        int v = _wtoi(tok);
        if (v >= 0 && v <= 1000) xbarSteps[xbarCount++] = v;
    }
    xbarDefault = GetPrivateProfileIntW(L"gpu", L"xbar_default", xbarCount ? xbarSteps[xbarCount - 1] : 0, iniPath);
    for (gpuProfiles = 0; gpuProfiles < 5; gpuProfiles++) {
        wsprintfW(sec, L"gpu.%d", gpuProfiles + 1);
        if (!GetPrivateProfileStringW(sec, L"curve", L"", list, 8, iniPath)) break;
        GetPrivateProfileStringW(sec, L"name", L"", gpuNames[gpuProfiles + 1], 24, iniPath);
    }
}

static int gpuSlot(void) {
    int slot = (int)getDword(L"GpuProfile", 1);
    return slot < 1 || slot > gpuProfiles ? 1 : slot;
}

static void gpuSay(int r, LPCWSTR what) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    gpuState = r, gpuAt = t;
    if (!r) wsprintfW(gpuNote, L"Applied at %02d:%02d", t.wHour, t.wMinute);
    else if (r == -137) lstrcpyW(gpuNote, L"Needs admin rights");
    else if (r == -1000) lstrcpyW(gpuNote, L"Saved for a different card");
    else if (r == -1001) lstrcpyW(gpuNote, L"The driver changed the curve");
    else wsprintfW(gpuNote, L"%s refused (%d)", what, r);
}

static void gpuXbarNow(void) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    xbarSent = (int)getDword(L"Xbar", (DWORD)xbarDefault);
    xbarState = nvXbar(xbarSent * 1000, NULL);
    xbarSentAt = t;
}

// Power, memory, XBAR, curve last. Stops at the first refusal, so a half-applied profile ends on the stock curve
// (the memory write has zeroed it), never on a wrong one.
static int gpuApplyNow(int slot) {
    int *delta = nvAlloc(NV_SLOTS * sizeof(int)), power, memory, count = gpuLoad(slot, &power, &memory, delta), r = 0;
    LPCWSTR what = L"Power limit";
    gpuSentSlot = slot;
    if (power) r = nvPower(power, NULL);
    if (!r) what = L"Memory offset", r = nvMemory(memory, NULL);
    if (!r && !gpuPoints) nvBaseRead(); // the memory write has just zeroed the offsets
    if (!r && xbarCount) gpuXbarNow();
    if (!r) what = L"Curve", r = nvCurveSet(delta, count);
    gpuSay(r, what);
    nvFree(delta);
    return r;
}

// Reads what the card is running now and writes a report for the command line to out. With slot 1 to 5 it is also
// kept as that profile in blackout.ini. Returns 0 when the card was read and, if asked for, the profile was written;
// a driver code, -1000 when the card shows no core points, -1002 when blackout.ini could not be written.
static int gpuRead(int slot, char *out, int *len) {
    WCHAR sec[16], num[16], *list;
    int n, power = 0, memory = 0, xbar = 0, wrote = 1;
    NvCurve *c = nvAlloc(sizeof(NvCurve));
    int r = nvCurveGet(c), pr = nvPower(NV_KEEP, &power), mr = nvMemory(0, &memory), xr = nvXbar(NV_KEEP, &xbar);
    n = wsprintfA(out, "curve %d, core points %d\r\npower %d: %d\r\n", r, c->count, pr, power);
    n += wsprintfA(out + n, "memory %d: %d kHz\r\n", mr, memory);
    n += wsprintfA(out + n, "xbar %d: %d kHz\r\noffsets kHz:", xr, xbar);
    for (int i = 0; i < c->count; i += 9) n += wsprintfA(out + n, " %d", *nvDelta(c, c->slot[i]));
    // A memory offset set by another tool can sit in the table's memory slots instead: keep the larger reading.
    for (int i = 0; i < NV_SLOTS; i++) {
        DWORD *point = (DWORD *)(c->info + 0x44 + i * 24);
        if ((point[0] || point[1] != 1) && *nvDelta(c, i) > memory) memory = *nvDelta(c, i);
    }
    n += wsprintfA(out + n, "\r\n");
    if (!r && !c->count) r = -1000;
    if (!r && slot >= 1 && slot <= 5) {
        list = nvAlloc(4096 * sizeof(WCHAR));
        for (int i = 0, at = 0; i < c->count; i++) at += wsprintfW(list + at, i ? L",%d" : L"%d", *nvDelta(c, c->slot[i]));
        wsprintfW(sec, L"gpu.%d", slot);
        wsprintfW(num, L"%d", power);
        WritePrivateProfileStringW(sec, L"power", num, iniPath);
        wsprintfW(num, L"%d", memory);
        WritePrivateProfileStringW(sec, L"memory", num, iniPath);
        wrote = WritePrivateProfileStringW(sec, L"curve", list, iniPath);
        n += wsprintfA(out + n, "saved profile %d: %s (power %d, memory %d kHz, %d points)\r\n", slot, wrote ? "ok" : "FAILED", power, memory, c->count);
        nvFree(list);
    }
    nvFree(c);
    *len = n;
    return r ? r : wrote ? 0 : -1002;
}

// NVAPI can stall for seconds while the driver recovers, so nothing here runs on the window's thread.
// job: 0 = only XBAR, 1 = the whole profile and XBAR, GPU_KEEP + N = keep what the card is running as profile N.
static DWORD WINAPI gpuThread(void *arg) {
    int job = (int)(INT_PTR)arg, n;
    if (!nvOpen()) {
        if (job >= GPU_KEEP) gpuKeptState = -2;
        else gpuState = -2, lstrcpyW(gpuNote, L"No NVIDIA driver found");
    } else {
        if (job >= GPU_KEEP) {
            char *text = nvAlloc(2048);
            gpuKeptState = gpuRead(job - GPU_KEEP, text, &n);
            if (!gpuKeptState && !gpuPoints) nvBaseRead(); // so the chart can draw it without waiting for a send
            nvFree(text);
        } else if (job && gpuProfiles) {
            gpuApplyNow(gpuSlot());
        } else if (xbarCount) {
            gpuXbarNow();
        }
        nvClose();
    }
    if (job >= GPU_KEEP) gpuKept = job - GPU_KEEP, GetLocalTime(&gpuKeptAt);
    InterlockedExchange(&gpuBusy, 0);
    PostMessageW(gpuWnd, WM_GPU_DONE, (WPARAM)job, 0);
    return 0;
}

// Returns FALSE while an earlier job is still running.
static BOOL gpuStart(HWND hwnd, int job) {
    if (InterlockedCompareExchange(&gpuBusy, 1, 0)) return FALSE;
    gpuWnd = hwnd;
    HANDLE t = CreateThread(NULL, 0, gpuThread, (void *)(INT_PTR)job, 0, NULL);
    if (t) CloseHandle(t), gpuWait = job >= GPU_KEEP ? 2 : 1;
    else InterlockedExchange(&gpuBusy, 0), gpuWait = 0;
    return TRUE;
}

// ---- Driver reset: the driver logs it, Windows tells us. wevtapi.dll is only loaded when [gpu] is set up. ----

static DWORD WINAPI gpuEvent(int action, void *hwnd, HANDLE event) {
    (void)event;
    if (action == 1) PostMessageW((HWND)hwnd, WM_GPU_RESET, 0, 0); // EvtSubscribeActionDeliver
    return 0;
}

static void gpuWatch(HWND hwnd) {
    typedef HANDLE(WINAPI * Subscribe)(HANDLE, HANDLE, LPCWSTR, LPCWSTR, HANDLE, void *, void *, DWORD);
    static BOOL on;
    if (on) return; // once: at startup, or when the first profile is kept from the panel
    on = TRUE;
    HMODULE lib = LoadLibraryExW(L"wevtapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Subscribe sub = lib ? (Subscribe)GetProcAddress(lib, "EvtSubscribe") : NULL;
    if (sub) sub(NULL, NULL, L"System", GPU_QUERY, NULL, hwnd, gpuEvent, 1); // EvtSubscribeToFutureEvents
}

// ---- Command line: --gpu-save N keeps what the card is running now as profile N, --gpu-apply N applies one, ----
// ---- --gpu-status only reads. Each writes %TEMP%\blackout-gpu.txt.                                          ----

static int gpuCommand(LPCWSTR cmd) {
    char out[2048];
    WCHAR path[MAX_PATH];
    DWORD w;
    LPCWSTR at;
    int n = 0, r, slot;
    loadConfig();
    if (!nvOpen()) {
        n = wsprintfA(out, "No NVIDIA driver found\r\n");
    } else if ((at = wcsstr(cmd, L"--gpu-apply"))) {
        slot = _wtoi(at + 11);
        r = slot >= 1 && slot <= gpuProfiles ? gpuApplyNow(slot) : -1;
        n = wsprintfA(out, "apply profile %d: %d (%ls), xbar %d: %d\r\n", slot, r, r == -1 ? L"no such profile" : gpuNote, xbarSent, xbarState);
    } else {
        gpuRead((at = wcsstr(cmd, L"--gpu-save")) ? _wtoi(at + 10) : 0, out, &n);
    }
    if (nvLib) nvClose();
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"blackout-gpu.txt");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 1;
    WriteFile(f, out, n, &w, NULL);
    CloseHandle(f);
    return 0;
}
