// Tray panel: a Windows 11 style flyout drawn with Direct2D/DirectWrite.
// Included by blackout.c (one translation unit, shares its state).
//
// Cost model: the panel exists only while it is open. d2d1.dll and dwrite.dll are loaded on open and freed on close.
// Drawing is done on the CPU (software DC render target) into a 32-bit bitmap that is handed to a layered window.
// For a 372 px panel that opens in about 15 ms and needs about 15 MB while open, where a GPU target took 90 to 400 ms
// and 60 to 70 MB because it loads the graphics driver into the process. Measured with --panel-bench.
//
// Look: docked flush to the taskbar and the screen edge, square on those sides, one rounded corner facing the desktop,
// and filled with the taskbar's own colour (sampled when it opens) so it reads as part of the same surface.
//
// Views: Main (screens off, lighting, live cooling, the GPU profile) and Settings with tabs Fans, Wake, GPU, System.
//
// Type: the big numerals (F_BIG) are for live readings only. Settings are drawn as charts and chips. Red (ACCENT) means
// "on": a switch that is on, the profile that is on the card, the one main action of a view.
// Words: a row has one line, and its title says what the control does without help. No hint lines, no labels over
// cards that explain themselves. Secondary text only for state that changes or for what nothing else can say.
#define COBJMACROS
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#define PSAPI_VERSION 2
#include <psapi.h>

#define PANEL_CLASS L"BlackoutPanel"
#define VERSION     L"1.1.0"
#define PANEL_W     372.0f
#define SET_H       476.0f // every Settings tab has this height, so the tabs never move under the cursor
#define PAD         16.0f
#define INSET       14.0f
#define GAP         12.0f
#define ROW         40.0f // one line of text and one control
#define NOTICE      44.0f // the one row with two lines: something is wrong, and why
#define SECT        18.0f
#define LABEL       22.0f
#define RADIUS      8
#define ACCENT      0xE5484D
#define WARN        0xF0A35B
#define GOOD        0x3FB950
#define WM_RENDER   (WM_APP + 3)

enum { H_NONE, H_SCREENS, H_GEAR, H_BACK, H_TAB_WAKE, H_TAB_SYSTEM, H_RGB, H_SYNC, H_START, H_EXIT, H_HOTKEY, H_WAKE_CLICK, H_WAKE_ESC, H_COUNT };
enum { F_BRAND, F_ROW, F_SMALL, F_BIG, F_UNIT, F_BOLD, F_TINY, F_COUNT };
enum { V_MAIN, V_WAKE, V_SYSTEM };

static const GUID IID_D2DFactory = {0x06152247, 0x6f50, 0x465a, {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
static const GUID IID_DWFactory = {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
typedef HRESULT(WINAPI *D2DCreate)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);
typedef HRESULT(WINAPI *DWCreate)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);

static HMODULE d2dLib, dwLib;
static ID2D1Factory *d2f;
static IDWriteFactory *dwf;
static ID2D1DCRenderTarget *dcrt;
static HDC surfDc; // the panel is drawn into this bitmap, premultiplied BGRA
static HBITMAP surfBmp;
static HGDIOBJ surfOld;
static void *surfBits;
static int surfW, surfH;
static ID2D1RenderTarget *rt;
static ID2D1SolidColorBrush *brush;
static IDWriteTextFormat *fmt[F_COUNT];
static HWND panelOwner;
static float panelScale = 1, panelH;
static D2D1_RECT_F hit[H_COUNT];
static int hover, view = V_MAIN, settingsTab = V_WAKE;
static BOOL dockTop, panelLight, panelTest, panelHidden, dirty;
static DWORD panelClosedAt;
// Preview values for --panel-test / --panel-shot, so screenshots don't depend on the machine's settings.
static struct { LPCWSTR name; DWORD value; } fake[16];
static int fakeCount;

static DWORD pref(LPCWSTR name, DWORD fallback) {
    for (int i = 0; i < fakeCount; i++)
        if (!lstrcmpW(fake[i].name, name)) return fake[i].value;
    return getDword(name, fallback);
}
static void setPref(LPCWSTR name, DWORD v) {
    if (!panelTest) {
        setDword(name, v);
        return;
    }
    for (int i = 0; i < fakeCount; i++)
        if (!lstrcmpW(fake[i].name, name)) {
            fake[i].value = v;
            return;
        }
    if (fakeCount < 16) fake[fakeCount].name = name, fake[fakeCount++].value = v;
}

// ---- Drawing primitives ----

static D2D1_COLOR_F rgba(UINT rgb, float a) {
    D2D1_COLOR_F c = {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a};
    return c;
}
static UINT ink(void) { return panelLight ? 0x1B1B1F : 0xF3F3F3; }
static UINT dim(void) { return panelLight ? 0x5F5F66 : 0xA8A8AD; }
static UINT tone(void) { return panelLight ? 0x000000 : 0xFFFFFF; }
static UINT warn(void) { return panelLight ? 0xB45309 : WARN; }
static UINT panelTint; // 0xRRGGBB taken from the taskbar, 0 = none
static int shade = 100, grey; // percent of the taskbar's brightness, and percent of its colour removed
static UINT baseColor(void) { return panelTint ? panelTint : panelLight ? 0xEDEDED : 0x1C1E20; }

static void paint(UINT rgb, float a) {
    D2D1_COLOR_F c = rgba(rgb, a);
    ID2D1SolidColorBrush_SetColor(brush, &c);
}
static D2D1_RECT_F rc(float x, float y, float w, float h) {
    D2D1_RECT_F r = {x, y, x + w, y + h};
    return r;
}
static D2D1_POINT_2F pt(float x, float y) {
    D2D1_POINT_2F p = {x, y};
    return p;
}
static BOOL inside(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static void fillRound(D2D1_RECT_F r, float rad, UINT rgb, float a) {
    D2D1_ROUNDED_RECT rr = {r, rad, rad};
    paint(rgb, a);
    ID2D1RenderTarget_FillRoundedRectangle(rt, &rr, (ID2D1Brush *)brush);
}
static void strokeRound(D2D1_RECT_F r, float rad, UINT rgb, float a, float w) {
    D2D1_ROUNDED_RECT rr = {r, rad, rad};
    paint(rgb, a);
    ID2D1RenderTarget_DrawRoundedRectangle(rt, &rr, (ID2D1Brush *)brush, w, NULL);
}
static void dot(float x, float y, float r, UINT rgb, float a) {
    D2D1_ELLIPSE e = {{x, y}, r, r};
    paint(rgb, a);
    ID2D1RenderTarget_FillEllipse(rt, &e, (ID2D1Brush *)brush);
}
static void ring(float x, float y, float r, UINT rgb, float a, float w) {
    D2D1_ELLIPSE e = {{x, y}, r, r};
    paint(rgb, a);
    ID2D1RenderTarget_DrawEllipse(rt, &e, (ID2D1Brush *)brush, w, NULL);
}
static void line(float x0, float y0, float x1, float y1, UINT rgb, float a, float w) {
    paint(rgb, a);
    ID2D1RenderTarget_DrawLine(rt, pt(x0, y0), pt(x1, y1), (ID2D1Brush *)brush, w, NULL);
}
static void text(LPCWSTR s, int f, D2D1_RECT_F r, UINT rgb, float a, DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextFormat_SetTextAlignment(fmt[f], align);
    paint(rgb, a);
    ID2D1RenderTarget_DrawText(rt, s, lstrlenW(s), fmt[f], &r, (ID2D1Brush *)brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}
static float measure(LPCWSTR s, int f) {
    IDWriteTextLayout *l;
    DWRITE_TEXT_METRICS m = {0};
    IDWriteTextFormat_SetTextAlignment(fmt[f], DWRITE_TEXT_ALIGNMENT_LEADING);
    if (IDWriteFactory_CreateTextLayout(dwf, s, lstrlenW(s), fmt[f], 1000, 100, &l) != S_OK) return 0;
    IDWriteTextLayout_GetMetrics(l, &m);
    IDWriteTextLayout_Release(l);
    return m.widthIncludingTrailingWhitespace;
}
// Open arc, clockwise from (x0, y0) to (x1, y1), taking the long way round when large is set.
static void arc(float x0, float y0, float x1, float y1, float r, BOOL large, UINT rgb, float w) {
    ID2D1PathGeometry *g;
    ID2D1GeometrySink *sink;
    if (ID2D1Factory_CreatePathGeometry(d2f, &g) != S_OK) return;
    if (ID2D1PathGeometry_Open(g, &sink) == S_OK) {
        D2D1_ARC_SEGMENT seg = {{x1, y1}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL};
        ID2D1GeometrySink_BeginFigure(sink, pt(x0, y0), D2D1_FIGURE_BEGIN_HOLLOW);
        ID2D1GeometrySink_AddArc(sink, &seg);
        ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_OPEN);
        ID2D1GeometrySink_Close(sink);
        ID2D1GeometrySink_Release(sink);
        paint(rgb, 1);
        ID2D1RenderTarget_DrawGeometry(rt, (ID2D1Geometry *)g, (ID2D1Brush *)brush, w, NULL);
    }
    ID2D1PathGeometry_Release(g);
}

// ---- Icons ----

// Crescent as a real shape (disc minus disc), so it sits cleanly on the translucent background.
static void moon(float x, float y, float r) {
    D2D1_ELLIPSE outer = {{x, y}, r, r}, cut = {{x + r * 0.55f, y - r * 0.1f}, r * 0.8f, r * 0.8f};
    ID2D1EllipseGeometry *a, *b;
    ID2D1PathGeometry *g;
    ID2D1GeometrySink *sink;
    if (ID2D1Factory_CreateEllipseGeometry(d2f, &outer, &a) != S_OK) return;
    if (ID2D1Factory_CreateEllipseGeometry(d2f, &cut, &b) == S_OK) {
        if (ID2D1Factory_CreatePathGeometry(d2f, &g) == S_OK) {
            if (ID2D1PathGeometry_Open(g, &sink) == S_OK) {
                ID2D1EllipseGeometry_CombineWithGeometry(a, (ID2D1Geometry *)b, D2D1_COMBINE_MODE_EXCLUDE, NULL, 0.05f, (ID2D1SimplifiedGeometrySink *)sink);
                ID2D1GeometrySink_Close(sink);
                ID2D1GeometrySink_Release(sink);
                paint(ink(), 1);
                ID2D1RenderTarget_FillGeometry(rt, (ID2D1Geometry *)g, (ID2D1Brush *)brush, NULL);
            }
            ID2D1PathGeometry_Release(g);
        }
        ID2D1EllipseGeometry_Release(b);
    }
    ID2D1EllipseGeometry_Release(a);
}
static void powerIcon(float cx, float cy, UINT rgb) {
    arc(cx + 3.3f, cy - 4.4f, cx - 3.3f, cy - 4.4f, 5.5f, TRUE, rgb, 2);
    line(cx, cy - 7.5f, cx, cy - 1, rgb, 1, 2);
}
static void gear(float cx, float cy, UINT rgb) {
    static const float c8[8] = {1, 0.7071f, 0, -0.7071f, -1, -0.7071f, 0, 0.7071f};
    for (int i = 0; i < 8; i++) {
        float c = c8[i], s = c8[(i + 6) % 8];
        line(cx + c * 5.5f, cy + s * 5.5f, cx + c * 8.5f, cy + s * 8.5f, rgb, 1, 2.6f);
    }
    ring(cx, cy, 5.5f, rgb, 1, 2);
    dot(cx, cy, 2, rgb, 1);
}
static void chevronLeft(float cx, float cy, UINT rgb) {
    line(cx + 3, cy - 6, cx - 3, cy, rgb, 1, 2);
    line(cx - 3, cy, cx + 3, cy + 6, rgb, 1, 2);
}
static void mouseIcon(float cx, float cy, UINT rgb, float a) {
    strokeRound(rc(cx - 4.5f, cy - 7, 9, 14), 4.5f, rgb, a, 1.3f);
    line(cx, cy - 4.5f, cx, cy - 1.5f, rgb, a, 1.3f);
}
static void keyboardIcon(float cx, float cy, UINT rgb, float a) {
    strokeRound(rc(cx - 8, cy - 5.5f, 16, 11), 2.5f, rgb, a, 1.3f);
    for (int i = -1; i <= 1; i++) dot(cx + i * 4, cy - 1.5f, 0.9f, rgb, a);
    line(cx - 3.5f, cy + 2, cx + 3.5f, cy + 2, rgb, a, 1.3f);
}

// ---- Controls ----

static void toggle(float x, float y, BOOL on, BOOL hot, BOOL enabled) {
    D2D1_RECT_F r = rc(x, y, 40, 20);
    if (!enabled) {
        strokeRound(r, 10, dim(), 0.35f, 1);
        dot(x + (on ? 30 : 10), y + 10, 5, dim(), 0.35f);
    } else if (on) {
        fillRound(r, 10, ACCENT, hot ? 0.85f : 1);
        dot(x + 30, y + 10, 6, 0xFFFFFF, 1);
    } else {
        fillRound(r, 10, tone(), hot ? 0.10f : 0.04f);
        strokeRound(r, 10, dim(), 0.9f, 1);
        dot(x + 10, y + 10, 5, dim(), 1);
    }
}
static void card(float x, float y, float w, float h) {
    fillRound(rc(x, y, w, h), 8, tone(), 0.045f);
    strokeRound(rc(x, y, w, h), 8, tone(), 0.07f, 1);
}
static void divider(float x, float y, float w) { line(x, y, x + w, y, tone(), 0.07f, 1); }
static void section(LPCWSTR name, float x, float *y, float w) {
    text(name, F_TINY, rc(x + 2, *y + 2, w, 16), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    *y += LABEL;
}
static void hoverFill(int id, D2D1_RECT_F r) {
    if (hover == id) fillRound(rc(r.left + 3, r.top + 3, r.right - r.left - 6, r.bottom - r.top - 6), 6, tone(), 0.05f);
}
// A title on the left, a switch on the right. The whole row is the click target. The title has to say what the switch
// does on its own: rows have one line. indent sets a row under the switch it depends on.
static void toggleRow(LPCWSTR label, float x, float y, float w, float indent, int id, BOOL on, BOOL enabled) {
    float a = enabled ? 1 : 0.4f;
    if (enabled) {
        hit[id] = rc(x, y, w, ROW);
        hoverFill(id, hit[id]);
    }
    text(label, F_ROW, rc(x + INSET + indent, y + 10, w - 80 - indent, 22), ink(), a, DWRITE_TEXT_ALIGNMENT_LEADING);
    toggle(x + w - INSET - 40, y + 10, on, hover == id, enabled);
}

// ---- Layout ----

static float mainHeight(void) { return PAD + 32 + GAP + 2 * ROW + PAD; }
static float viewHeight(void) { return view == V_MAIN ? mainHeight() : SET_H; }

// ---- Views ----

static void paintHeader(float x, float y, float w) {
    if (view == V_MAIN) {
        moon(x + 9, y + 16, 9);
        text(L"Blackout", F_BRAND, rc(x + 26, y + 5, 150, 22), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        hit[H_SCREENS] = rc(x + w - 128, y, 128, 32);
        fillRound(hit[H_SCREENS], 16, ACCENT, hover == H_SCREENS ? 0.85f : 1);
        powerIcon(x + w - 110, y + 17, 0xFFFFFF);
        text(L"Screens off", F_BOLD, rc(x + w - 98, y + 7, 92, 18), 0xFFFFFF, 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        hit[H_GEAR] = rc(x + w - 128 - 8 - 32, y, 32, 32);
        if (hover == H_GEAR) dot(hit[H_GEAR].left + 16, y + 16, 16, tone(), 0.10f);
        gear(hit[H_GEAR].left + 16, y + 16, hover == H_GEAR ? ink() : dim());
    } else {
        hit[H_BACK] = rc(x - 4, y, 32, 32);
        if (hover == H_BACK) dot(x + 12, y + 16, 16, tone(), 0.10f);
        chevronLeft(x + 12, y + 16, ink());
    }
}

static void paintTabs(float x, float y, float w) {
    static const LPCWSTR names[2] = {L"Wake", L"System"};
    float sw = (w - 6) / 2;
    fillRound(rc(x, y, w, 32), 8, tone(), 0.06f);
    for (int i = 0; i < 2; i++) {
        int id = H_TAB_WAKE + i;
        BOOL on = view == V_WAKE + i;
        D2D1_RECT_F r = rc(x + 3 + i * sw, y + 3, sw, 26);
        hit[id] = rc(r.left, y, sw, 32);
        if (on) fillRound(r, 6, 0xFFFFFF, panelLight ? 0.9f : 0.13f);
        else if (hover == id) fillRound(r, 6, tone(), 0.06f);
        text(names[i], on ? F_BOLD : F_ROW, rc(r.left, y + (on ? 7 : 6), sw, 20), on || hover == id ? ink() : dim(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

static void paintMain(float x, float y, float w) {
    card(x, y, w, 2 * ROW);
    toggleRow(L"RGB lighting", x, y, w, 0, H_RGB, panelTest ? pref(L"RgbOn", 1) : rgbShown(), TRUE);
    divider(x, y + ROW, w);
    toggleRow(L"RGB off with the screens", x, y + ROW, w, 0, H_SYNC, pref(L"Sync", 1), TRUE);
}

static void paintWake(float x, float y, float w) {
    WCHAR s[96];
    float ix = x + INSET, iw = w - 2 * INSET;
    BOOL hk = pref(L"HotkeyMode", 0);
    // The title is the start of a sentence that the rows under it finish: wake the screens only with a left-click,
    // with Esc, and (always, so it is a word and not a switch) with Ctrl+Alt+Del.
    card(x, y, w, 4 * ROW);
    toggleRow(L"Wake the screens only with", x, y, w, 0, H_HOTKEY, hk, TRUE);
    divider(x, y + ROW, w);
    toggleRow(L"Left-click", x, y + ROW, w, 16, H_WAKE_CLICK, pref(L"WakeClick", 1), hk);
    divider(x, y + 2 * ROW, w);
    toggleRow(L"Esc", x, y + 2 * ROW, w, 16, H_WAKE_ESC, pref(L"WakeEsc", 1), hk);
    divider(x, y + 3 * ROW, w);
    text(L"Ctrl+Alt+Del", F_ROW, rc(ix + 16, y + 3 * ROW + 10, iw - 100, 22), ink(), hk ? 1 : 0.4f, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(L"always", F_SMALL, rc(ix + iw - 80, y + 3 * ROW + 11, 80, 18), dim(), hk ? 1 : 0.4f, DWRITE_TEXT_ALIGNMENT_TRAILING);
    y += 4 * ROW + SECT;

    section(L"WHAT WOKE THE SCREENS", x, &y, w);
    float h = panelH - PAD - y;
    card(x, y, w, h);
    if (!wakeCount) {
        text(L"Nothing yet", F_ROW, rc(ix, y + h / 2 - 11, iw, 22), ink(), 0.7f, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    float rh = (h - 12) / MAX_WAKES;
    for (int i = 0; i < wakeCount; i++) {
        float ry = y + 6 + i * rh;
        if (i) divider(ix, ry, iw);
        wsprintfW(s, L"%02d:%02d", wakes[i].at.wHour, wakes[i].at.wMinute);
        text(s, F_BOLD, rc(ix, ry + rh / 2 - 9, 44, 18), ink(), i ? 0.7f : 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        // The cause as a picture: what kind of input, the device's name, and the bracketed remark set apart on the
        // right ("not blocked" is the one that matters, so it must never be the part that gets cut off).
        WCHAR *name = s, *tag;
        float cy = ry + rh / 2, tw = 0;
        lstrcpyW(s, wakes[i].cause);
        if ((tag = wcsstr(s, L" ("))) {
            *tag = 0, tag += 2;
            if (tag[0] && tag[lstrlenW(tag) - 1] == L')') tag[lstrlenW(tag) - 1] = 0;
            BOOL slip = tag[0] == L'n'; // "not blocked": input got past Hotkey mode
            tw = measure(tag, F_TINY) + 8;
            text(tag, F_TINY, rc(ix + iw - tw, cy - 7, tw, 14), slip ? warn() : dim(), 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        if (s[0] == L'M' || s[0] == L'L') mouseIcon(ix + 56, cy, i ? dim() : ink(), 1);
        else if (s[0] == L'K' || s[0] == L'E') keyboardIcon(ix + 56, cy, i ? dim() : ink(), 1);
        else ring(ix + 56, cy, 4, dim(), 0.7f, 1.3f);
        if (!wcsncmp(s, L"Mouse: ", 7)) name += 7;
        else if (!wcsncmp(s, L"Keyboard: ", 10)) name += 10;
        text(name, F_SMALL, rc(ix + 72, cy - 8, iw - 72 - tw, 18), i ? dim() : ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

// The value takes all the room the label leaves, and ends in an ellipsis if even that is not enough.
static void statusRow(LPCWSTR label, LPCWSTR value, int state, float x, float y, float w) {
    float lw = measure(label, F_SMALL) + 16 + 12;
    if (state >= 0) dot(x + INSET + 4, y + 14, 3.5f, state == 2 ? GOOD : state == 1 ? warn() : dim(), state ? 1 : 0.5f);
    text(label, F_SMALL, rc(x + INSET + 16, y + 6, 120, 18), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(value, F_SMALL, rc(x + INSET + lw, y + 6, w - 2 * INSET - lw, 18), state == 1 ? warn() : dim(), 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

static void paintSystem(float x, float y, float w) {
    WCHAR s[96];
    float ix = x + INSET, iw = w - 2 * INSET;
    card(x, y, w, 2 * ROW);
    toggleRow(L"Start with Windows", x, y, w, 0, H_START, panelTest ? pref(L"Start", 1) : startupEnabled(), TRUE);
    divider(x, y + ROW, w);
    hit[H_EXIT] = rc(x, y + ROW, w, ROW);
    hoverFill(H_EXIT, hit[H_EXIT]);
    text(L"Exit Blackout", F_ROW, rc(ix, y + ROW + 10, iw, 22), ACCENT, 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    y += 2 * ROW + GAP;

    float rh = (panelH - PAD - y - 8) / 5;
    card(x, y, w, panelH - PAD - y);
    y += 4 + (rh - 28) / 2;
    wsprintfW(s, L"%d LEDs", linkLeds);
    statusRow(L"Pump lighting", !linkLeds ? L"Not set up" : linkOk ? s : L"Hub not answering", !linkLeds ? 0 : linkOk ? 2 : 1, x, y, w);
    statusRow(L"OpenRGB", hasOpenRgb ? L"Found" : openrgb[0] ? L"Not found at the path in blackout.ini" : L"Not set up", hasOpenRgb ? 2 : openrgb[0] ? 1 : 0, x, y + rh, w);
    statusRow(L"RAM lighting", hasRam ? L"FURY module" : L"Not installed", hasRam ? 2 : 0, x, y + 2 * rh, w);
    statusRow(L"Monitor power", ddcIds[0] ? ddcIds : L"Not set up", ddcIds[0] ? 2 : 0, x, y + 3 * rh, w);
    statusRow(L"Blackout", VERSION, -1, x, y + 4 * rh, w);
}

static void paintPanel(void) {
    float x = PAD, w = PANEL_W - 2 * PAD, y = PAD;
    D2D1_COLOR_F clear = {0, 0, 0, 0};
    ID2D1PathGeometry *g;
    ID2D1GeometrySink *sink;
    float r = RADIUS, W = PANEL_W, H = panelH;
    ID2D1RenderTarget_Clear(rt, &clear);
    // Background: one opaque shape, square where it meets the taskbar and the screen edge, rounded at the corner that
    // faces the desktop. The window is layered, so the curve is anti-aliased against whatever is behind it.
    if (ID2D1Factory_CreatePathGeometry(d2f, &g) == S_OK) {
        if (ID2D1PathGeometry_Open(g, &sink) == S_OK) {
            D2D1_ARC_SEGMENT corner = {{dockTop ? 0 : r, dockTop ? H - r : 0}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL};
            if (dockTop) {
                ID2D1GeometrySink_BeginFigure(sink, pt(0, 0), D2D1_FIGURE_BEGIN_FILLED);
                ID2D1GeometrySink_AddLine(sink, pt(W, 0));
                ID2D1GeometrySink_AddLine(sink, pt(W, H));
                ID2D1GeometrySink_AddLine(sink, pt(r, H));
            } else {
                ID2D1GeometrySink_BeginFigure(sink, pt(W, 0), D2D1_FIGURE_BEGIN_FILLED);
                ID2D1GeometrySink_AddLine(sink, pt(W, H));
                ID2D1GeometrySink_AddLine(sink, pt(0, H));
                ID2D1GeometrySink_AddLine(sink, pt(0, r));
            }
            ID2D1GeometrySink_AddArc(sink, &corner);
            ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
            ID2D1GeometrySink_Close(sink);
            ID2D1GeometrySink_Release(sink);
            paint(baseColor(), 1);
            ID2D1RenderTarget_FillGeometry(rt, (ID2D1Geometry *)g, (ID2D1Brush *)brush, NULL);
        }
        ID2D1PathGeometry_Release(g);
    }
    // Hairline along the two free edges and round the corner, like the taskbar's own edge.
    if (ID2D1Factory_CreatePathGeometry(d2f, &g) == S_OK) {
        if (ID2D1PathGeometry_Open(g, &sink) == S_OK) {
            float edge = dockTop ? H - 0.5f : 0.5f;
            D2D1_ARC_SEGMENT corner = {{0.5f, dockTop ? H - r : r}, {r - 0.5f, r - 0.5f}, 0,
                                       dockTop ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL};
            ID2D1GeometrySink_BeginFigure(sink, pt(W, edge), D2D1_FIGURE_BEGIN_HOLLOW);
            ID2D1GeometrySink_AddLine(sink, pt(r, edge));
            ID2D1GeometrySink_AddArc(sink, &corner);
            ID2D1GeometrySink_AddLine(sink, pt(0.5f, dockTop ? 0 : H));
            ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_OPEN);
            ID2D1GeometrySink_Close(sink);
            ID2D1GeometrySink_Release(sink);
            paint(tone(), panelLight ? 0.14f : 0.12f);
            ID2D1RenderTarget_DrawGeometry(rt, (ID2D1Geometry *)g, (ID2D1Brush *)brush, 1, NULL);
        }
        ID2D1PathGeometry_Release(g);
    }
    memset(hit, 0, sizeof(hit));
    paintHeader(x, y, w);
    y += 32;
    if (view == V_MAIN) {
        paintMain(x, y + GAP, w);
        return;
    }
    // Settings has no title row: the tabs say where you are, and the back arrow sits beside them.
    paintTabs(x + 36, y - 32, w - 36);
    y += GAP;
    if (view == V_WAKE) paintWake(x, y, w);
    else paintSystem(x, y, w);
}

// ---- Resources and rendering ----

static void panelFree(void) {
    for (int i = 0; i < F_COUNT; i++)
        if (fmt[i]) IDWriteTextFormat_Release(fmt[i]), fmt[i] = NULL;
    if (brush) ID2D1SolidColorBrush_Release(brush), brush = NULL;
    if (dcrt) ID2D1DCRenderTarget_Release(dcrt), dcrt = NULL;
    rt = NULL;
    if (surfDc) {
        SelectObject(surfDc, surfOld);
        DeleteObject(surfBmp);
        DeleteDC(surfDc);
        surfDc = NULL, surfBmp = NULL, surfBits = NULL, surfW = surfH = 0;
    }
    if (dwf) IDWriteFactory_Release(dwf), dwf = NULL;
    if (d2f) ID2D1Factory_Release(d2f), d2f = NULL;
    if (dwLib) FreeLibrary(dwLib), dwLib = NULL;
    if (d2dLib) FreeLibrary(d2dLib), d2dLib = NULL;
}

static BOOL panelLoad(void) {
    static const struct { LPCWSTR face; DWRITE_FONT_WEIGHT weight; float size; } spec[F_COUNT] = {
        {L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 15},
        {L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 13.5f},
        {L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 11.5f},
        {L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 30},
        {L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 15},
        {L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13},
        {L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 10.5f},
    };
    d2dLib = LoadLibraryW(L"d2d1.dll");
    dwLib = LoadLibraryW(L"dwrite.dll");
    D2DCreate mkD2d = d2dLib ? (D2DCreate)GetProcAddress(d2dLib, "D2D1CreateFactory") : NULL;
    DWCreate mkDw = dwLib ? (DWCreate)GetProcAddress(dwLib, "DWriteCreateFactory") : NULL;
    BOOL ok = mkD2d && mkDw && mkD2d(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_D2DFactory, NULL, (void **)&d2f) == S_OK &&
              mkDw(DWRITE_FACTORY_TYPE_SHARED, &IID_DWFactory, (IUnknown **)&dwf) == S_OK;
    for (int i = 0; ok && i < F_COUNT; i++) {
        ok = IDWriteFactory_CreateTextFormat(dwf, spec[i].face, NULL, spec[i].weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             spec[i].size, L"en-gb", &fmt[i]) == S_OK;
        if (ok) {
            // One line, and text that does not fit ends in an ellipsis instead of being cut through a letter.
            DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            IDWriteInlineObject *dots;
            IDWriteTextFormat_SetWordWrapping(fmt[i], DWRITE_WORD_WRAPPING_NO_WRAP);
            if (IDWriteFactory_CreateEllipsisTrimmingSign(dwf, fmt[i], &dots) == S_OK) {
                IDWriteTextFormat_SetTrimming(fmt[i], &trim, dots);
                IDWriteInlineObject_Release(dots);
            }
        }
    }
    if (!ok) panelFree();
    return ok;
}

// The bitmap the panel is drawn into, remade when the view changes height.
static BOOL panelSurface(int w, int h) {
    if (surfDc && w == surfW && h == surfH) return TRUE;
    if (surfDc) {
        SelectObject(surfDc, surfOld);
        DeleteObject(surfBmp);
        DeleteDC(surfDc);
        surfDc = NULL;
    }
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, (DWORD)(w * h * 4), 0, 0, 0, 0}, {{0}}};
    surfDc = CreateCompatibleDC(NULL);
    surfBmp = surfDc ? CreateDIBSection(surfDc, &bi, DIB_RGB_COLORS, &surfBits, NULL, 0) : NULL;
    if (!surfBmp) {
        if (surfDc) DeleteDC(surfDc);
        surfDc = NULL;
        return FALSE;
    }
    surfOld = SelectObject(surfDc, surfBmp);
    surfW = w, surfH = h;
    return TRUE;
}

// Draws the current view on the CPU and, when there is a window, hands the bitmap to it. pos moves the window too.
static BOOL panelRender(POINT *pos) {
    D2D1_RENDER_TARGET_PROPERTIES p = {D2D1_RENDER_TARGET_TYPE_SOFTWARE, {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
                                       96 * panelScale, 96 * panelScale, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT};
    D2D1_COLOR_F white = {1, 1, 1, 1};
    int w = (int)(PANEL_W * panelScale + 0.5f), h = (int)(panelH * panelScale + 0.5f);
    RECT r = {0, 0, w, h};
    if (!panelSurface(w, h)) return FALSE;
    if (!rt) {
        if (ID2D1Factory_CreateDCRenderTarget(d2f, &p, &dcrt) != S_OK) return FALSE;
        rt = (ID2D1RenderTarget *)dcrt;
        ID2D1RenderTarget_SetTextAntialiasMode(rt, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (ID2D1RenderTarget_CreateSolidColorBrush(rt, &white, NULL, &brush) != S_OK) return FALSE;
    }
    ID2D1DCRenderTarget_BindDC(dcrt, surfDc, &r);
    ID2D1RenderTarget_BeginDraw(rt);
    paintPanel();
    if (ID2D1RenderTarget_EndDraw(rt, NULL, NULL) != S_OK) {
        ID2D1SolidColorBrush_Release(brush), brush = NULL;
        ID2D1DCRenderTarget_Release(dcrt), dcrt = NULL, rt = NULL;
        return FALSE;
    }
    if (panel) {
        POINT src = {0, 0};
        SIZE size = {w, h};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        GdiFlush();
        UpdateLayeredWindow(panel, NULL, pos, &size, surfDc, &src, 0, &blend, ULW_ALPHA);
    }
    return TRUE;
}

// Ask for a redraw. Several requests before the next message collapse into one frame.
static void panelDirty(void) {
    if (panel && !dirty) dirty = TRUE, PostMessageW(panel, WM_RENDER, 0, 0);
}

// ---- Interaction ----

static int hitTest(LPARAM lp) {
    float mx = (short)LOWORD(lp) / panelScale, my = (short)HIWORD(lp) / panelScale;
    for (int i = 1; i < H_COUNT; i++)
        if (inside(hit[i], mx, my)) return i;
    return H_NONE;
}

// Switch view. The window only changes size between Main and Settings, and only when their heights differ.
static void panelGo(int to) {
    RECT r;
    view = to, hover = H_NONE;
    if (to != V_MAIN) settingsTab = to;
    GetWindowRect(panel, &r);
    panelH = viewHeight();
    POINT pos = {r.left, dockTop ? r.top : r.bottom - (int)(panelH * panelScale + 0.5f)};
    panelRender(&pos);
}

static void panelClick(int id) {
    switch (id) {
    case H_NONE: return;
    case H_SCREENS:
        DestroyWindow(panel);
        if (!panelTest) SetTimer(panelOwner, TIMER_OFF, 400, NULL);
        return;
    case H_GEAR: panelGo(settingsTab); return;
    case H_BACK: panelGo(V_MAIN); return;
    case H_TAB_WAKE:
    case H_TAB_SYSTEM: panelGo(V_WAKE + id - H_TAB_WAKE); return;
    case H_RGB:
        if (panelTest) setPref(L"RgbOn", !pref(L"RgbOn", 1));
        else rgbToggle(panelOwner);
        break;
    case H_SYNC: setPref(L"Sync", !pref(L"Sync", 1)); break;
    case H_START:
        if (panelTest) setPref(L"Start", !pref(L"Start", 1));
        else toggleStartup();
        break;
    case H_HOTKEY: setPref(L"HotkeyMode", !pref(L"HotkeyMode", 0)); break;
    case H_WAKE_CLICK:
    case H_WAKE_ESC: {
        LPCWSTR key = id == H_WAKE_CLICK ? L"WakeClick" : L"WakeEsc", other = id == H_WAKE_CLICK ? L"WakeEsc" : L"WakeClick";
        // Keep at least one wake input, so the screens can always be woken.
        if (!pref(key, 1) || pref(other, 1)) setPref(key, !pref(key, 1));
        break;
    }
    case H_EXIT:
        DestroyWindow(panel);
        if (!panelTest) DestroyWindow(panelOwner);
        return;
    }
    panelDirty();
}

static LRESULT CALLBACK panelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps); // a layered window keeps its own picture; nothing to draw here
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_RENDER:
        dirty = FALSE;
        panelRender(NULL);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_NCCALCSIZE: if (wp) return 0; break;
    case WM_NCHITTEST: return HTCLIENT;
    case WM_MOUSEMOVE: {
        int h = hitTest(lp);
        if (h != hover) hover = h, panelDirty();
        SetCursor(LoadCursorW(NULL, h ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hover) hover = H_NONE, panelDirty();
        return 0;
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK: // the second click of a fast double click counts as a click too
        panelClick(hitTest(lp));
        return 0;
    case WM_SETCURSOR: return TRUE;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (view != V_MAIN) panelGo(V_MAIN);
            else DestroyWindow(hwnd);
        } else if (view != V_MAIN && (wp == VK_LEFT || wp == VK_RIGHT)) {
            panelGo(view == V_WAKE ? V_SYSTEM : V_WAKE);
        }
        return 0;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE) DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        panelFree();
        panel = NULL;
        hover = H_NONE, view = V_MAIN, dirty = FALSE;
        panelClosedAt = GetTickCount();
        if (panelTest && !panelHidden) PostQuitMessage(0);
        trim();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void panelUpdate(void) { panelDirty(); }

// Opens the panel docked to the taskbar. Returns FALSE (caller falls back to the plain menu) if Direct2D is unavailable.
static BOOL panelShow(HWND owner) {
    if (panel) {
        DestroyWindow(panel);
        return TRUE;
    }
    // A tray click on an open panel first deactivates (closes) it; don't reopen on the same click.
    if (GetTickCount() - panelClosedAt < 300) return TRUE;
    if (!panelLoad()) return FALSE;
    static BOOL registered;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = panelProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = PANEL_CLASS;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        registered = RegisterClassW(&wc) != 0;
    }
    panelOwner = owner;
    view = V_MAIN;
    if (!panelTest) panelLight = getDwordAt(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", 0);
    // The monitor that holds the tray icon; dock to its taskbar and right edge.
    NOTIFYICONIDENTIFIER id = {sizeof(id), nid.hWnd, nid.uID, {0}};
    RECT icon;
    POINT anchor;
    HWND shell = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!panelTest && Shell_NotifyIconGetRect(&id, &icon) == S_OK) anchor.x = (icon.left + icon.right) / 2, anchor.y = icon.top;
    else if (shell && GetWindowRect(shell, &icon)) anchor.x = icon.right - 1, anchor.y = icon.top; // the main taskbar
    else GetCursorPos(&anchor);
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    typedef HRESULT(WINAPI * GetDpi)(HMONITOR, int, UINT *, UINT *);
    GetDpi getDpi = shcore ? (GetDpi)GetProcAddress(shcore, "GetDpiForMonitor") : NULL;
    if (getDpi) getDpi(mon, 0, &dpiX, &dpiY);
    if (shcore) FreeLibrary(shcore);
    // Dock against the taskbar's own rectangle on this monitor. The work area is not used unless there is no taskbar
    // here: Windows sometimes reports it as the whole screen after a display change, which put the panel over the taskbar.
    RECT wa = mi.rcWork, bar;
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL), next = NULL;
    if (tray && MonitorFromWindow(tray, MONITOR_DEFAULTTONULL) != mon) tray = NULL;
    while (!tray && (next = FindWindowExW(NULL, next, L"Shell_SecondaryTrayWnd", NULL)))
        if (MonitorFromWindow(next, MONITOR_DEFAULTTONULL) == mon) tray = next;
    BOOL docked = tray && IsWindowVisible(tray) && GetWindowRect(tray, &bar);
    if (docked) {
        wa = mi.rcMonitor;
        if (bar.right - bar.left < bar.bottom - bar.top) { // vertical taskbar (Windows 10)
            if (bar.left <= wa.left) wa.left = bar.right;
            else wa.right = bar.left;
        } else if (bar.top <= wa.top) {
            wa.top = bar.bottom;
        } else {
            wa.bottom = bar.top;
        }
    }
    // Take the panel's colour from the taskbar it docks to, unchanged, so it reads as part of the same surface on
    // any wallpaper, accent colour or transparency setting. One row of taskbar pixels, per-channel median.
    DWORD tint = 0;
    panelTint = 0;
    if (docked) {
        int w = bar.right - bar.left > 512 ? 512 : bar.right - bar.left, hist[3][256] = {{0}};
        DWORD row[512];
        BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -1, 1, 32, BI_RGB, 0, 0, 0, 0, 0}, {{0}}};
        HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, 1);
        HGDIOBJ old = SelectObject(mem, bmp);
        BOOL ok = BitBlt(mem, 0, 0, w, 1, screen, bar.right - w, bar.top <= mi.rcMonitor.top ? bar.bottom - 3 : bar.top + 2, SRCCOPY);
        SelectObject(mem, old);
        if (ok && GetDIBits(mem, bmp, 0, 1, row, &bi, DIB_RGB_COLORS)) {
            for (int i = 0; i < w; i++)
                for (int c = 0; c < 3; c++) hist[c][(row[i] >> (16 - 8 * c)) & 255]++;
            int med[3], luma;
            for (int c = 0; c < 3; c++) {
                int seen = 0;
                med[c] = 0;
                while (med[c] < 255 && (seen += hist[c][med[c]]) * 2 < w) med[c]++;
            }
            luma = (med[0] * 3 + med[1] * 6 + med[2]) / 10;
            for (int c = 0; c < 3; c++) {
                int v = (med[c] * (100 - grey) + luma * grey) / 100 * shade / 100;
                tint |= (DWORD)(v > 255 ? 255 : v) << (8 * c); // AABBGGRR: c = 0 is red
            }
            panelTint = (tint & 255) << 16 | (tint & 0xFF00) | (tint >> 16 & 255);
        }
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
    }
    panelScale = dpiX / 96.0f;
    // Small or heavily scaled screens: shrink the whole panel so the tallest view still fits.
    float tallest = mainHeight() > SET_H ? mainHeight() : SET_H;
    if (tallest * panelScale > wa.bottom - wa.top) panelScale = (wa.bottom - wa.top) / tallest;
    panelH = viewHeight();
    int pw = (int)(PANEL_W * panelScale + 0.5f), ph = (int)(panelH * panelScale + 0.5f);
    dockTop = wa.top > mi.rcMonitor.top; // taskbar at the top
    int px = wa.right - pw, py = dockTop ? wa.top : wa.bottom - ph;
    panel = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | (panelHidden ? WS_EX_NOACTIVATE : 0), PANEL_CLASS, L"Blackout", WS_POPUP,
                            px, py, pw, ph, owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!panel) {
        panelFree();
        return FALSE;
    }
    // No DWM show or hide animation: the panel appears and disappears instantly, like other tray menus.
    BOOL noAnim = TRUE;
    DwmSetWindowAttribute(panel, 3, &noAnim, sizeof(noAnim));
    // The first frame is in place before the window is shown, so nothing half-drawn is ever visible.
    POINT pos = {px, py};
    if (!panelRender(&pos)) {
        DestroyWindow(panel);
        return FALSE;
    }
    if (panelHidden) return TRUE; // benchmark: never shown
    ShowWindow(panel, SW_SHOW);
    SetForegroundWindow(panel);
    return TRUE;
}

// ---- Developer modes (no hardware access, no single-instance lock) ----
//   --panel-test  [flags]              open the panel with preview data; exits on close
//   --panel-shot  [flags] out=<file>   draw one view off-screen to a .bmp, no window at all (README images, reviews)
//   --panel-bench                      open and close five times, never shown; log time and memory to %TEMP%
// Flags: wake system | light top | x2 scale=<percent> | hotkey wakes longwakes rgboff longstatus | hover=<id>

static BOOL flag(LPCWSTR cmd, LPCWSTR name) {
    WCHAR key[32] = L" ";
    lstrcatW(key, name);
    LPCWSTR at = wcsstr(cmd, key);
    return at && (at[lstrlenW(key)] == L' ' || !at[lstrlenW(key)]);
}

static BOOL panelShot(LPCWSTR path) {
    DWORD wrote;
    if (!panelLoad()) return FALSE;
    panelH = viewHeight();
    BOOL ok = panelRender(NULL);
    if (ok) {
        BITMAPINFOHEADER info = {sizeof(info), surfW, -surfH, 1, 32, BI_RGB, (DWORD)(surfW * surfH * 4), 0, 0, 0, 0};
        BITMAPFILEHEADER head = {0x4D42, sizeof(head) + sizeof(info) + surfW * surfH * 4, 0, 0, sizeof(head) + sizeof(info)};
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        ok = f != INVALID_HANDLE_VALUE;
        if (ok) {
            GdiFlush();
            WriteFile(f, &head, sizeof(head), &wrote, NULL);
            WriteFile(f, &info, sizeof(info), &wrote, NULL);
            WriteFile(f, surfBits, surfW * surfH * 4, &wrote, NULL);
            CloseHandle(f);
        }
    }
    panelFree();
    return ok;
}

static void benchLine(HANDLE f, LPCSTR stage, int ms) {
    PROCESS_MEMORY_COUNTERS_EX m = {sizeof(m)};
    char out[320];
    DWORD handles = 0, w;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof(m));
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    int n = wsprintfA(out, "%-16s private %5u KB  working set %5u KB  handles %3u  open %3d ms  loaded:%s%s%s%s\r\n", stage, (UINT)(m.PrivateUsage / 1024),
                      (UINT)(m.WorkingSetSize / 1024), handles, ms, GetModuleHandleW(L"d2d1.dll") ? " d2d1" : "", GetModuleHandleW(L"dwrite.dll") ? " dwrite" : "",
                      GetModuleHandleW(L"d3d10warp.dll") ? " warp" : "", GetModuleHandleW(L"nvwgf2umx.dll") || GetModuleHandleW(L"nvldumdx.dll") ? " gpu-driver" : "");
    WriteFile(f, out, n, &w, NULL);
}

static void panelBench(void) {
    WCHAR path[MAX_PATH];
    LARGE_INTEGER q, a, b;
    MSG m;
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"blackout-bench.txt");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    QueryPerformanceFrequency(&q);
    panelHidden = TRUE;
    benchLine(f, "start", 0);
    for (int i = 0; i < 5; i++) {
        char stage[32];
        QueryPerformanceCounter(&a);
        panelShow(NULL);
        QueryPerformanceCounter(&b);
        wsprintfA(stage, "open %d", i + 1);
        benchLine(f, stage, (int)((b.QuadPart - a.QuadPart) * 1000 / q.QuadPart));
        if (panel) DestroyWindow(panel);
        panelClosedAt = 0;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        wsprintfA(stage, "closed %d", i + 1);
        benchLine(f, stage, 0);
    }
    Sleep(3000);
    benchLine(f, "3 s later", 0);
    CloseHandle(f);
}

static int panelDev(LPWSTR cmd) {
    WCHAR fury[MAX_PATH];
    LPCWSTR out = wcsstr(cmd, L"out="), at;
    MSG m;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    loadConfig();
    panelTest = TRUE;
    panelLight = flag(cmd, L"light");
    exeDir(fury, L"furyled.exe");
    hasOpenRgb = openrgb[0] && GetFileAttributesW(openrgb) != INVALID_FILE_ATTRIBUTES;
    hasRam = GetFileAttributesW(fury) != INVALID_FILE_ATTRIBUTES;
    linkOk = !flag(cmd, L"longstatus");
    if (flag(cmd, L"longstatus")) {
        lstrcpyW(ddcIds, L"BNQ7F2F,DELA0EC,GSM5B7F,AUS27B1");
        lstrcpyW(openrgb, L"C:\\missing\\OpenRGB.exe"), linkLeds = 22;
    }
    // Every switch is faked, so previews and screenshots never depend on (or change) this machine's settings.
    setPref(L"RgbOn", !flag(cmd, L"rgboff")), setPref(L"Sync", 1), setPref(L"Start", 1);
    setPref(L"HotkeyMode", flag(cmd, L"hotkey")), setPref(L"WakeClick", 1), setPref(L"WakeEsc", 1);
    if (flag(cmd, L"wakes")) {
        static const LPCWSTR why[3] = {L"Left-click", L"Mouse: PRO X SUPERLIGHT 2c", L"No input: Windows or an app"};
        for (wakeCount = 0; wakeCount < 3; wakeCount++) {
            GetLocalTime(&wakes[wakeCount].at);
            wakes[wakeCount].at.wHour = (WORD)(9 - wakeCount * 3), wakes[wakeCount].at.wMinute = (WORD)(13 + wakeCount * 17);
            lstrcpyW(wakes[wakeCount].cause, why[wakeCount]);
        }
    }
    if (flag(cmd, L"longwakes")) {
        // The longest causes wakeRecord() can write: a 40 character product name, with and without "(not blocked)".
        static const LPCWSTR why[MAX_WAKES] = {L"Keyboard: Razer BlackWidow V4 Pro 75% Wireless Ke (not blocked)", L"Mouse: Logitech G502 X PLUS LIGHTSPEED Wireless (not blocked)",
                                               L"Input device: USB Composite Device Receiver 2.4G", L"Esc", L"No input: Windows or an app"};
        for (wakeCount = 0; wakeCount < MAX_WAKES; wakeCount++) {
            GetLocalTime(&wakes[wakeCount].at);
            wakes[wakeCount].at.wHour = (WORD)(23 - wakeCount * 5), wakes[wakeCount].at.wMinute = (WORD)(48 - wakeCount * 9);
            lstrcpyW(wakes[wakeCount].cause, why[wakeCount]);
        }
    }
    dockTop = flag(cmd, L"top");
    if ((at = wcsstr(cmd, L"hover="))) hover = _wtoi(at + 6);
    int to = flag(cmd, L"wake") ? V_WAKE : flag(cmd, L"system") ? V_SYSTEM : V_MAIN;
    if (wcsstr(cmd, L"--panel-bench")) {
        panelBench();
        return 0;
    }
    if (wcsstr(cmd, L"--panel-shot")) {
        panelScale = flag(cmd, L"x2") ? 2 : (at = wcsstr(cmd, L"scale=")) ? _wtoi(at + 6) / 100.0f : 1;
        view = to;
        return out && panelShot(out + 4) ? 0 : 1;
    }
    if (!panelShow(NULL)) return 1;
    if (to != V_MAIN) panelGo(to);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        DispatchMessageW(&m);
    }
    return 0;
}
