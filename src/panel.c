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
// Views: Main (screens off, lighting, live cooling) and Settings with three tabs (Fans, Wake, System).
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
#define CHART_H     142.0f
#define PAD         16.0f
#define INSET       14.0f
#define GAP         12.0f
#define ROW         44.0f
#define SECT        18.0f
#define LABEL       22.0f
#define RADIUS      8
#define ACCENT      0xE5484D
#define WARN        0xF0A35B
#define GOOD        0x3FB950
#define WM_RENDER   (WM_APP + 3)

enum {
    H_NONE, H_SCREENS, H_GEAR, H_BACK, H_TAB_FANS, H_TAB_WAKE, H_TAB_SYSTEM, H_RGB, H_SYNC, H_CURVE, H_START, H_EXIT,
    H_MAX_DN, H_MAX_UP, H_HYS_DN, H_HYS_UP, H_RESET, H_HOTKEY, H_WAKE_CLICK, H_WAKE_ESC, H_NOTICE, H_COUNT
};
enum { F_BRAND, F_ROW, F_SMALL, F_BIG, F_UNIT, F_BOLD, F_TINY, F_COUNT };
enum { V_MAIN, V_FANS, V_WAKE, V_SYSTEM };

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
static int hover, dragPoint = -1, view = V_MAIN, settingsTab = V_FANS, tabs[3], tabCount;
static BOOL dockTop, panelLight, panelTest, panelHidden, dirty;
static DWORD panelClosedAt;
static D2D1_RECT_F chart;
static const float tMin = 25, tMax = 50;
// Preview values for --panel-test / --panel-shot, so screenshots don't depend on the machine's settings.
static struct { LPCWSTR name; DWORD value; } fake[12];
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
    if (fakeCount < 12) fake[fakeCount].name = name, fake[fakeCount++].value = v;
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
static UINT knob(void) { return panelLight ? 0x1B1B1F : 0xFFFFFF; } // markers that sit on the background
static UINT panelTint; // 0xRRGGBB taken from the taskbar, 0 = none
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
static void dashed(float x0, float y0, float x1, float y1, UINT rgb, float a) {
    float len = x1 > x0 ? x1 - x0 : y1 - y0;
    for (float s = 0; s < len; s += 6) {
        float e = s + 3 > len ? len : s + 3;
        if (x1 > x0) line(x0 + s, y0, x0 + e, y0, rgb, a, 1);
        else line(x0, y0 + s, x0, y0 + e, rgb, a, 1);
    }
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
// Polyline, or with fill a closed area down to the baseline.
static void geometry(D2D1_POINT_2F *p, int n, BOOL fill, float base, UINT rgb, float a, float w) {
    ID2D1PathGeometry *g;
    ID2D1GeometrySink *sink;
    if (n < 2 || ID2D1Factory_CreatePathGeometry(d2f, &g) != S_OK) return;
    if (ID2D1PathGeometry_Open(g, &sink) == S_OK) {
        ID2D1GeometrySink_BeginFigure(sink, fill ? pt(p[0].x, base) : p[0], fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = fill ? 0 : 1; i < n; i++) ID2D1GeometrySink_AddLine(sink, p[i]);
        if (fill) ID2D1GeometrySink_AddLine(sink, pt(p[n - 1].x, base));
        ID2D1GeometrySink_EndFigure(sink, fill ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        ID2D1GeometrySink_Close(sink);
        ID2D1GeometrySink_Release(sink);
        paint(rgb, a);
        if (fill) ID2D1RenderTarget_FillGeometry(rt, (ID2D1Geometry *)g, (ID2D1Brush *)brush, NULL);
        else ID2D1RenderTarget_DrawGeometry(rt, (ID2D1Geometry *)g, (ID2D1Brush *)brush, w, NULL);
    }
    ID2D1PathGeometry_Release(g);
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
// Label (and optional hint) on the left, switch on the right. The whole row is the click target.
static void toggleRow(LPCWSTR label, LPCWSTR hint, float x, float y, float w, int id, BOOL on, BOOL enabled) {
    float a = enabled ? 1 : 0.4f;
    if (enabled) {
        hit[id] = rc(x, y, w, ROW);
        hoverFill(id, hit[id]);
    }
    text(label, F_ROW, rc(x + INSET, y + (hint ? 6 : 12), w - 80, 22), ink(), a, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (hint) text(hint, F_TINY, rc(x + INSET, y + 26, w - 80, 14), dim(), a, DWRITE_TEXT_ALIGNMENT_LEADING);
    toggle(x + w - INSET - 40, y + 12, on, hover == id, enabled);
}
// Label and hint on the left, [-] value [+] on the right.
static void stepperRow(LPCWSTR label, LPCWSTR hint, LPCWSTR value, float x, float y, float w, int down, int up, BOOL canDown, BOOL canUp) {
    float bx = x + w - INSET - 28;
    text(label, F_ROW, rc(x + INSET, y + 6, w - 160, 22), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(hint, F_TINY, rc(x + INSET, y + 26, w - 160, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    for (int i = 0; i < 2; i++) {
        int id = i ? up : down;
        BOOL can = i ? canUp : canDown;
        D2D1_RECT_F r = rc(i ? bx : bx - 28 - 52, y + 8, 28, 28);
        float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        if (can) hit[id] = r;
        fillRound(r, 6, tone(), !can ? 0.03f : hover == id ? 0.12f : 0.06f);
        line(cx - 5, cy, cx + 5, cy, ink(), can ? 1 : 0.3f, 1.5f);
        if (i) line(cx, cy - 5, cx, cy + 5, ink(), can ? 1 : 0.3f, 1.5f);
    }
    text(value, F_BOLD, rc(bx - 52, y + 13, 52, 18), ink(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---- Layout ----

static float chartX(float t) { return chart.left + (t - tMin) / (tMax - tMin) * (chart.right - chart.left); }
static float chartY(float d) { return chart.bottom - d / 100 * (chart.bottom - chart.top); }
static int capped(int d) { return d > fanMax ? fanMax : d; }

static float mainHeight(void) {
    float h = PAD + 32 + SECT + LABEL + 2 * ROW;
    if (hasCurve) h += SECT + LABEL + INSET + 56 + GAP + 48 + GAP + 44 + INSET + ROW;
    else if (fanWhy >= 2) h += GAP + ROW; // notice: the fan curve is set up but can't run
    return h + PAD;
}
static float viewHeight(void) { return view == V_MAIN ? mainHeight() : SET_H; }

static void chainName(int i, LPWSTR out) {
    WCHAR list[96], *tok, *ctx;
    int n = 0;
    lstrcpyW(list, fanNames);
    for (tok = wcstok(list, L",", &ctx); tok; tok = wcstok(NULL, L",", &ctx), n++)
        if (n == i) {
            while (*tok == L' ') tok++;
            lstrcpynW(out, tok, 24);
            return;
        }
    wsprintfW(out, L"Chain %d", i + 1);
}

// True when the panel has changed the curve, max speed or margin away from blackout.ini.
static BOOL curveEdited(void) {
    if (panelTest) return pref(L"Edited", 0);
    return RegGetValueW(HKEY_CURRENT_USER, APP_KEY, L"Curve", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS ||
           RegGetValueW(HKEY_CURRENT_USER, APP_KEY, L"FanMax", RRF_RT_REG_DWORD, NULL, NULL, NULL) == ERROR_SUCCESS ||
           RegGetValueW(HKEY_CURRENT_USER, APP_KEY, L"Hysteresis", RRF_RT_REG_DWORD, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static LPCWSTR fanWhyText(void) {
    static const LPCWSTR why[] = {L"", L"Not set up: add [fans] to blackout.ini", L"Needs Blackout to run as admin", L"PawnIO or LpcIO.bin is missing",
                                  L"No supported fan chip found"};
    return why[fanWhy < 0 || fanWhy > 4 ? 0 : fanWhy];
}

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
        text(L"Settings", F_BRAND, rc(x + 34, y + 5, 150, 22), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

// The Fans tab is only offered when a fan curve is configured (working or not).
static void tabsBuild(void) {
    BOOL found = FALSE;
    tabCount = 0;
    if (fanWhy != 1) tabs[tabCount++] = V_FANS;
    tabs[tabCount++] = V_WAKE;
    tabs[tabCount++] = V_SYSTEM;
    for (int i = 0; i < tabCount; i++) found |= tabs[i] == settingsTab;
    if (!found) settingsTab = tabs[0];
}

static void paintTabs(float x, float y, float w) {
    static const LPCWSTR names[3] = {L"Fans", L"Wake", L"System"};
    float sw = (w - 6) / tabCount;
    fillRound(rc(x, y, w, 32), 8, tone(), 0.06f);
    for (int i = 0; i < tabCount; i++) {
        int id = H_TAB_FANS + tabs[i] - V_FANS;
        BOOL on = view == tabs[i];
        D2D1_RECT_F r = rc(x + 3 + i * sw, y + 3, sw, 26);
        hit[id] = rc(r.left, y, sw, 32);
        if (on) fillRound(r, 6, 0xFFFFFF, panelLight ? 0.9f : 0.13f);
        else if (hover == id) fillRound(r, 6, tone(), 0.06f);
        text(names[tabs[i] - V_FANS], on ? F_BOLD : F_ROW, rc(r.left, y + (on ? 7 : 6), sw, 20), on || hover == id ? ink() : dim(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

static void paintMain(float x, float y, float w) {
    WCHAR s[96];
    float ix = x + INSET, iw = w - 2 * INSET;
    section(L"LIGHTING", x, &y, w);
    card(x, y, w, 2 * ROW);
    toggleRow(L"RGB", NULL, x, y, w, H_RGB, panelTest ? pref(L"RgbOn", 1) : rgbShown(), TRUE);
    divider(x, y + ROW, w);
    toggleRow(L"Turn off with screens", NULL, x, y + ROW, w, H_SYNC, pref(L"Sync", 1), TRUE);
    if (!hasCurve) {
        if (fanWhy < 2) return;
        // Set up but not running: say so where it will be seen. Click for details.
        y += 2 * ROW + GAP;
        card(x, y, w, ROW);
        hit[H_NOTICE] = rc(x, y, w, ROW);
        hoverFill(H_NOTICE, hit[H_NOTICE]);
        dot(ix + 4, y + 22, 3.5f, warn(), 1);
        text(L"Fan curve is off", F_ROW, rc(ix + 16, y + 6, iw - 16, 22), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        text(fanWhyText(), F_TINY, rc(ix + 16, y + 26, iw - 16, 14), warn(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        return;
    }
    y += 2 * ROW + SECT;

    section(L"COOLING", x, &y, w);
    card(x, y, w, INSET + 56 + GAP + 48 + GAP + 44 + INSET + ROW);
    float cy = y + INSET;
    // Two big readings on the left, the graph's range on the right.
    if (coolant < 0) lstrcpyW(s, L"--");
    else wsprintfW(s, L"%d.%d", (int)coolant, (int)(coolant * 10 + 0.5) % 10);
    text(s, F_BIG, rc(ix, cy - 4, 120, 40), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(L"\x00B0" L"C", F_UNIT, rc(ix + 2 + measure(s, F_BIG), cy + 10, 30, 22), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(L"Water", F_SMALL, rc(ix, cy + 38, 80, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (fanDuty < 0) lstrcpyW(s, L"BIOS");
    else wsprintfW(s, L"%d", fanDuty);
    text(s, F_BIG, rc(ix + 118, cy - 4, 110, 40), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (fanDuty >= 0) text(L"%", F_UNIT, rc(ix + 120 + measure(s, F_BIG), cy + 10, 24, 22), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(fanDuty >= 0 && fanMax < 100 && fanDuty >= fanMax ? L"Fans (at max)" : L"Fans", F_SMALL, rc(ix + 118, cy + 38, 110, 18), dim(), 1,
         DWRITE_TEXT_ALIGNMENT_LEADING);
    int lo = 99999, hi = -99999;
    for (int i = 0; i < historyLen; i++) {
        int v = history[(historyPos - historyLen + i + HISTORY) % HISTORY];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (historyLen >= 8) {
        int l10 = (lo + 5) / 10, h10 = (hi + 5) / 10;
        text(L"Last 15 min", F_SMALL, rc(ix, cy + 2, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (h10 - l10 < 1) wsprintfW(s, L"steady %d.%d \x00B0" L"C", l10 / 10, l10 % 10);
        else wsprintfW(s, L"%d.%d to %d.%d \x00B0" L"C", l10 / 10, l10 % 10, h10 / 10, h10 % 10);
        text(s, F_SMALL, rc(ix, cy + 22, iw, 18), ink(), 0.85f, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    cy += 56 + GAP;
    // Coolant graph: fills from the right as history builds; at least a 1 C span so sensor noise stays flat.
    if (coolant < 0) {
        text(L"No water reading. Fans are on the BIOS curve.", F_SMALL, rc(ix, cy + 15, iw, 18), warn(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (historyLen >= 8) {
        D2D1_POINT_2F p[HISTORY];
        float top = cy + 4, bot = cy + 44;
        float span = hi - lo < 100 ? 100 : (float)(hi - lo), mid = (hi + lo) / 2.0f;
        for (int i = 0; i < historyLen; i++) {
            int v = history[(historyPos - historyLen + i + HISTORY) % HISTORY];
            p[i].x = ix + iw * (HISTORY - historyLen + i) / (HISTORY - 1);
            p[i].y = (top + bot) / 2 - (v - mid) / span * (bot - top);
        }
        geometry(p, historyLen, TRUE, bot + 4, ACCENT, 0.14f, 0);
        geometry(p, historyLen, FALSE, 0, ACCENT, 1, 2);
        dot(p[historyLen - 1].x, p[historyLen - 1].y, 3.5f, knob(), 1);
    } else {
        text(L"The graph fills in over the next 15 minutes", F_SMALL, rc(ix, cy + 15, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    divider(ix, cy + 48, iw);
    cy += 48 + GAP;
    int chips = fanChCount < 3 ? fanChCount : 3;
    float cw = chips ? (iw - (chips - 1) * 8) / chips : iw;
    for (int i = 0; i < chips; i++) {
        WCHAR name[32];
        float cxp = ix + i * (cw + 8);
        fillRound(rc(cxp, cy, cw, 44), 6, tone(), 0.06f);
        if (fanRpm[i] >= 1000 && fanRpm[i] < 10000) wsprintfW(s, L"%d,%03d", fanRpm[i] / 1000, fanRpm[i] % 1000);
        else if (fanRpm[i] > 0 && fanRpm[i] < 1000) wsprintfW(s, L"%d", fanRpm[i]);
        else lstrcpyW(s, L"-");
        text(s, F_BOLD, rc(cxp, cy + 5, cw, 18), ink(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        chainName(i, name);
        wsprintfW(s, L"%s rpm", name);
        text(s, F_SMALL, rc(cxp, cy + 23, cw, 16), dim(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    cy += 44 + INSET;
    divider(x, cy, w);
    toggleRow(L"Follow water temperature", NULL, x, cy, w, H_CURVE, curveOn, TRUE);
}

static void paintFans(float x, float y, float w) {
    WCHAR s[96];
    float ix = x + INSET, iw = w - 2 * INSET;
    if (!hasCurve) {
        card(x, y, w, 118);
        text(L"The fan curve is off", F_ROW, rc(ix, y + 14, iw, 22), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        text(fanWhyText(), F_SMALL, rc(ix, y + 38, iw, 18), warn(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        text(L"It runs motherboard fans from the water temperature.", F_SMALL, rc(ix, y + 66, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        text(L"Setup: README, section Coolant fan curve.", F_SMALL, rc(ix, y + 86, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        return;
    }
    card(x, y, w, INSET + 6 + CHART_H + 18 + GAP + 18 + INSET + 3 * ROW);
    float cy = y + INSET;
    chart = rc(ix + 34, cy + 6, iw - 34 - 8, CHART_H);
    for (int d = 0; d <= 100; d += 25) line(chart.left, chartY((float)d), chart.right, chartY((float)d), tone(), 0.07f, 1);
    for (int t = 25; t <= 50; t += 5) {
        line(chartX((float)t), chart.top, chartX((float)t), chart.bottom, tone(), 0.07f, 1);
        wsprintfW(s, L"%d\x00B0", t);
        text(s, F_TINY, rc(chartX((float)t) - 16, chart.bottom + 4, 32, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    for (int d = 0; d <= 100; d += 50) {
        wsprintfW(s, L"%d%%", d);
        text(s, F_TINY, rc(ix, chartY((float)d) - 7, 28, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (fanMax < 100) {
        dashed(chart.left, chartY((float)fanMax), chart.right, chartY((float)fanMax), warn(), 0.8f);
        wsprintfW(s, L"Max %d%%", fanMax);
        text(s, F_TINY, rc(chart.left + 4, chartY((float)fanMax) - 15, 80, 14), warn(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    D2D1_POINT_2F p[MAX_CURVE + 2];
    int n = 0;
    p[n++] = pt(chart.left, chartY((float)curveAt(tMin)));
    for (int i = 0; i < curveN; i++) p[n++] = pt(chartX((float)curveT[i]), chartY((float)capped(curveD[i])));
    p[n++] = pt(chart.right, chartY((float)curveAt(tMax)));
    geometry(p, n, FALSE, 0, ACCENT, 1, 2.5f);
    // Where the water is now.
    if (coolant >= tMin && coolant <= tMax) {
        dashed(chartX((float)coolant), chart.top, chartX((float)coolant), chart.bottom, tone(), 0.4f);
        dot(chartX((float)coolant), chartY((float)curveAt(coolant)), 3.5f, knob(), 1);
    }
    for (int i = 0; i < curveN; i++) {
        D2D1_POINT_2F hp = p[i + 1];
        if (dragPoint == i) dot(hp.x, hp.y, 11, ACCENT, 0.18f);
        dot(hp.x, hp.y, 6, baseColor(), 1);
        ring(hp.x, hp.y, 6, dragPoint == i ? ACCENT : knob(), 1, 2);
    }
    // While dragging, a value tag follows the handle.
    if (dragPoint >= 0) {
        D2D1_POINT_2F hp = p[dragPoint + 1];
        wsprintfW(s, L"%d \x00B0" L"C   %d%%", curveT[dragPoint], capped(curveD[dragPoint]));
        float tw = measure(s, F_SMALL) + 18, tx = hp.x - tw / 2, ty = hp.y - 34 < chart.top - 8 ? hp.y + 12 : hp.y - 34;
        if (tx < chart.left) tx = chart.left;
        if (tx + tw > chart.right) tx = chart.right - tw;
        fillRound(rc(tx, ty, tw, 22), 11, panelLight ? 0x1B1B1F : 0xF3F3F3, 0.96f);
        text(s, F_SMALL, rc(tx, ty + 3, tw, 18), panelLight ? 0xF3F3F3 : 0x1B1B1F, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    cy = chart.bottom + 18 + GAP;
    text(curveN < MAX_CURVE ? L"Drag a point. Double-click adds one, right-click removes it." : L"Drag a point. Right-click removes it.", F_SMALL,
         rc(ix, cy, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    cy += 18 + INSET;
    divider(x, cy, w);
    wsprintfW(s, L"%d%%", fanMax);
    stepperRow(L"Max fan speed", L"Upper limit for every fan", s, x, cy, w, H_MAX_DN, H_MAX_UP, fanMax > 30, fanMax < 100);
    cy += ROW;
    divider(x, cy, w);
    wsprintfW(s, L"%d \x00B0" L"C", fanHyst);
    stepperRow(L"Slow down after", L"Water must be this much cooler first", s, x, cy, w, H_HYS_DN, H_HYS_UP, fanHyst > 0, fanHyst < 5);
    cy += ROW;
    divider(x, cy, w);
    BOOL edited = curveEdited();
    if (edited) {
        hit[H_RESET] = rc(x, cy, w, ROW);
        hoverFill(H_RESET, hit[H_RESET]);
    }
    text(L"Reset to blackout.ini", F_ROW, rc(ix, cy + 6, iw, 22), ink(), edited ? 1 : 0.4f, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(edited ? L"Drops the changes made here" : L"Nothing has been changed here", F_TINY, rc(ix, cy + 26, iw, 14), dim(), edited ? 1 : 0.4f,
         DWRITE_TEXT_ALIGNMENT_LEADING);
}

static void paintWake(float x, float y, float w) {
    WCHAR s[96];
    float ix = x + INSET, iw = w - 2 * INSET;
    BOOL hk = pref(L"HotkeyMode", 0);
    card(x, y, w, 3 * ROW);
    toggleRow(L"Hotkey mode", L"With the screens off, only the inputs below wake them", x, y, w, H_HOTKEY, hk, TRUE);
    divider(x, y + ROW, w);
    toggleRow(L"Left-click", L"Mouse movement and other buttons are ignored", x, y + ROW, w, H_WAKE_CLICK, pref(L"WakeClick", 1), hk);
    divider(x, y + 2 * ROW, w);
    toggleRow(L"Esc", L"Other keys are ignored", x, y + 2 * ROW, w, H_WAKE_ESC, pref(L"WakeEsc", 1), hk);
    y += 3 * ROW + 10;
    text(L"Ctrl+Alt+Del always works. Ignored input never reaches an app.", F_SMALL, rc(x + 2, y, w, 16), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    y += 16 + 14;

    section(L"WHAT WOKE THE SCREENS", x, &y, w);
    float h = SET_H - PAD - y;
    card(x, y, w, h);
    if (!wakeCount) {
        text(L"Nothing yet", F_ROW, rc(ix, y + h / 2 - 22, iw, 22), ink(), 0.7f, DWRITE_TEXT_ALIGNMENT_CENTER);
        text(L"Each wake is listed here with the device behind it", F_SMALL, rc(ix, y + h / 2 + 2, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    float rh = (h - 12) / MAX_WAKES;
    for (int i = 0; i < wakeCount; i++) {
        float ry = y + 6 + i * rh;
        if (i) divider(ix, ry, iw);
        wsprintfW(s, L"%02d:%02d", wakes[i].at.wHour, wakes[i].at.wMinute);
        text(s, F_BOLD, rc(ix, ry + rh / 2 - 9, 44, 18), ink(), i ? 0.7f : 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        text(wakes[i].cause, F_SMALL, rc(ix + 48, ry + rh / 2 - 8, iw - 48, 18), i ? dim() : ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

static void statusRow(LPCWSTR label, LPCWSTR value, int state, float x, float y, float w) {
    dot(x + INSET + 4, y + 14, 3.5f, state == 2 ? GOOD : state == 1 ? warn() : dim(), state ? 1 : 0.5f);
    text(label, F_SMALL, rc(x + INSET + 16, y + 6, 120, 18), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(value, F_SMALL, rc(x + INSET + 120, y + 6, w - 2 * INSET - 120, 18), state == 1 ? warn() : dim(), 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

static void paintSystem(float x, float y, float w) {
    WCHAR s[96];
    float ix = x + INSET, iw = w - 2 * INSET;
    card(x, y, w, 2 * ROW);
    toggleRow(L"Start with Windows", NULL, x, y, w, H_START, panelTest ? pref(L"Start", 1) : startupEnabled(), TRUE);
    divider(x, y + ROW, w);
    hit[H_EXIT] = rc(x, y + ROW, w, ROW);
    hoverFill(H_EXIT, hit[H_EXIT]);
    text(L"Exit Blackout", F_ROW, rc(ix, y + ROW + 12, iw, 22), ACCENT, 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    y += 2 * ROW + SECT;

    // What Blackout found on this machine: 2 = working, 1 = needs attention, 0 = not set up.
    section(L"STATUS", x, &y, w);
    card(x, y, w, 7 * 28 + 8);
    y += 4;
    if (!fanWhy) wsprintfW(s, L"Active on %d fan header%s", fanChCount, fanChCount == 1 ? L"" : L"s");
    statusRow(L"Fan curve", !fanWhy ? s : fanWhy == 1 ? L"Not set up" : fanWhyText(), fanWhy == 0 ? 2 : fanWhy == 1 ? 0 : 1, x, y, w);
    if (coolant >= 0) wsprintfW(s, L"%d.%d \x00B0" L"C from the iCUE LINK hub", (int)coolant, (int)(coolant * 10 + 0.5) % 10);
    statusRow(L"Water sensor", coolant >= 0 ? s : hasCurve ? L"No reading" : L"Not used", coolant >= 0 ? 2 : hasCurve ? 1 : 0, x, y + 28, w);
    wsprintfW(s, L"Hub connected, %d LEDs", linkLeds);
    statusRow(L"Pump lighting", !linkLeds ? L"Not set up" : linkOk ? s : L"Hub not answering", !linkLeds ? 0 : linkOk ? 2 : 1, x, y + 56, w);
    statusRow(L"OpenRGB", hasOpenRgb ? L"Found" : openrgb[0] ? L"Not found at the path in blackout.ini" : L"Not set up", hasOpenRgb ? 2 : openrgb[0] ? 1 : 0, x,
              y + 84, w);
    statusRow(L"RAM lighting", hasRam ? L"FURY module found" : L"Not installed", hasRam ? 2 : 0, x, y + 112, w);
    statusRow(L"Monitor power", ddcIds[0] ? ddcIds : L"Not set up", ddcIds[0] ? 2 : 0, x, y + 140, w);
    BOOL admin = panelTest ? pref(L"Admin", 1) : isElevated();
    statusRow(L"Admin rights", admin ? L"Yes" : L"No, so the fan curve can't run", admin ? 2 : fanChCount ? 1 : 0, x, y + 168, w);
    text(L"Blackout " VERSION L"  \x00B7  github.com/Xipzer/blackout-tray", F_TINY, rc(x + 2, SET_H - PAD - 14, w, 14), dim(), 0.8f,
         DWRITE_TEXT_ALIGNMENT_LEADING);
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
        paintMain(x, y + SECT, w);
        return;
    }
    paintTabs(x, y + GAP, w);
    y += GAP + 32 + GAP;
    if (view == V_FANS) paintFans(x, y, w);
    else if (view == V_WAKE) paintWake(x, y, w);
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
        if (ok) IDWriteTextFormat_SetWordWrapping(fmt[i], DWRITE_WORD_WRAPPING_NO_WRAP);
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

static int curvePointAt(LPARAM lp) {
    float mx = (short)LOWORD(lp) / panelScale, my = (short)HIWORD(lp) / panelScale;
    if (!hasCurve || view != V_FANS) return -1;
    for (int i = 0; i < curveN; i++) {
        float dx = mx - chartX((float)curveT[i]), dy = my - chartY((float)capped(curveD[i]));
        if (dx * dx + dy * dy < 12 * 12) return i;
    }
    return -1;
}

static void saveCurve(void) {
    WCHAR s[128] = L"", part[16];
    for (int i = 0; i < curveN; i++) {
        wsprintfW(part, i ? L",%d:%d" : L"%d:%d", curveT[i], curveD[i]);
        lstrcatW(s, part);
    }
    if (panelTest) {
        setPref(L"Edited", 1);
        return;
    }
    RegSetKeyValueW(HKEY_CURRENT_USER, APP_KEY, L"Curve", REG_SZ, s, (lstrlenW(s) + 1) * sizeof(WCHAR));
    curveApplyNow();
}

// Double-click on the chart: add a point there, keeping the points in temperature order and at least 1 C apart.
static void curveAdd(LPARAM lp) {
    float mx = (short)LOWORD(lp) / panelScale, my = (short)HIWORD(lp) / panelScale;
    if (!hasCurve || view != V_FANS || curveN >= MAX_CURVE || !inside(chart, mx, my) || curvePointAt(lp) >= 0) return;
    int t = (int)(tMin + (mx - chart.left) / (chart.right - chart.left) * (tMax - tMin) + 0.5f);
    int d = (int)((chart.bottom - my) / (chart.bottom - chart.top) * 100 + 0.5f), at = 0;
    while (at < curveN && curveT[at] < t) at++;
    if ((at < curveN && curveT[at] == t) || (at > 0 && curveT[at - 1] == t)) return;
    for (int i = curveN; i > at; i--) curveT[i] = curveT[i - 1], curveD[i] = curveD[i - 1];
    curveT[at] = t;
    curveD[at] = d < 20 ? 20 : d > fanMax ? fanMax : d;
    curveN++;
    saveCurve();
}

// Right-click on a point: remove it. Two points always stay.
static void curveRemove(LPARAM lp) {
    int at = curvePointAt(lp);
    if (at < 0 || curveN <= 2) return;
    for (int i = at; i < curveN - 1; i++) curveT[i] = curveT[i + 1], curveD[i] = curveD[i + 1];
    curveN--;
    saveCurve();
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
    case H_NOTICE: panelGo(V_FANS); return;
    case H_BACK: panelGo(V_MAIN); return;
    case H_TAB_FANS:
    case H_TAB_WAKE:
    case H_TAB_SYSTEM: panelGo(V_FANS + id - H_TAB_FANS); return;
    case H_RGB:
        if (panelTest) setPref(L"RgbOn", !pref(L"RgbOn", 1));
        else rgbToggle(panelOwner);
        break;
    case H_SYNC: setPref(L"Sync", !pref(L"Sync", 1)); break;
    case H_CURVE:
        if (panelTest) curveOn = !curveOn, fanDuty = curveOn ? curveAt(coolant) : -1;
        else SendMessageW(panelOwner, WM_COMMAND, ID_CURVE, 0);
        break;
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
    case H_MAX_DN:
    case H_MAX_UP:
        fanMax += id == H_MAX_UP ? 5 : -5;
        fanMax = fanMax < 30 ? 30 : fanMax > 100 ? 100 : fanMax;
        setPref(L"FanMax", fanMax);
        if (panelTest) setPref(L"Edited", 1);
        else curveApplyNow();
        break;
    case H_HYS_DN:
    case H_HYS_UP:
        fanHyst += id == H_HYS_UP ? 1 : -1;
        fanHyst = fanHyst < 0 ? 0 : fanHyst > 5 ? 5 : fanHyst;
        setPref(L"Hysteresis", fanHyst);
        if (panelTest) setPref(L"Edited", 1);
        break;
    case H_RESET: {
        WCHAR list[128];
        if (panelTest) {
            setPref(L"Edited", 0);
        } else {
            RegDeleteKeyValueW(HKEY_CURRENT_USER, APP_KEY, L"Curve");
            RegDeleteKeyValueW(HKEY_CURRENT_USER, APP_KEY, L"FanMax");
            RegDeleteKeyValueW(HKEY_CURRENT_USER, APP_KEY, L"Hysteresis");
        }
        GetPrivateProfileStringW(L"fans", L"curve", L"30:40,45:100", list, 128, iniPath);
        parseCurve(list);
        fanMax = GetPrivateProfileIntW(L"fans", L"max", 100, iniPath);
        if (fanMax < 20 || fanMax > 100) fanMax = 100;
        fanHyst = GetPrivateProfileIntW(L"fans", L"hysteresis", 2, iniPath);
        if (fanHyst < 0 || fanHyst > 10) fanHyst = 2;
        if (!panelTest) curveApplyNow();
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
        if (dragPoint >= 0) {
            float mx = (short)LOWORD(lp) / panelScale, my = (short)HIWORD(lp) / panelScale;
            int t = (int)(tMin + (mx - chart.left) / (chart.right - chart.left) * (tMax - tMin) + 0.5f);
            int d = (int)((chart.bottom - my) / (chart.bottom - chart.top) * 100 + 0.5f);
            int tlo = dragPoint ? curveT[dragPoint - 1] + 1 : (int)tMin, thi = dragPoint < curveN - 1 ? curveT[dragPoint + 1] - 1 : (int)tMax;
            t = t < tlo ? tlo : t > thi ? thi : t;
            d = d < 20 ? 20 : d > fanMax ? fanMax : d;
            if (t != curveT[dragPoint] || d != curveD[dragPoint]) {
                curveT[dragPoint] = t, curveD[dragPoint] = d;
                panelDirty();
            }
            return 0;
        }
        int h = hitTest(lp);
        if (h != hover) hover = h, panelDirty();
        SetCursor(LoadCursorW(NULL, h ? IDC_HAND : curvePointAt(lp) >= 0 ? IDC_SIZEALL : IDC_ARROW));
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hover) hover = H_NONE, panelDirty();
        return 0;
    case WM_LBUTTONDOWN:
        dragPoint = curvePointAt(lp);
        if (dragPoint >= 0) SetCapture(hwnd), panelDirty();
        return 0;
    case WM_LBUTTONUP:
        if (dragPoint >= 0) {
            dragPoint = -1;
            ReleaseCapture();
            saveCurve();
            panelDirty();
        } else {
            panelClick(hitTest(lp));
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        // The second click of a fast double click on a switch or button counts as a click too.
        if (hitTest(lp)) panelClick(hitTest(lp));
        else curveAdd(lp), panelDirty();
        return 0;
    case WM_RBUTTONUP:
        curveRemove(lp);
        panelDirty();
        return 0;
    case WM_SETCURSOR: return TRUE;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (view != V_MAIN) panelGo(V_MAIN);
            else DestroyWindow(hwnd);
        } else if (view != V_MAIN && (wp == VK_LEFT || wp == VK_RIGHT)) {
            int at = 0;
            while (at < tabCount && tabs[at] != view) at++;
            panelGo(tabs[(at + (wp == VK_RIGHT ? 1 : tabCount - 1)) % tabCount]);
        }
        return 0;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE && dragPoint < 0) DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        panelFree();
        panel = NULL;
        hover = H_NONE, dragPoint = -1, view = V_MAIN, dirty = FALSE;
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
    tabsBuild();
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
    // Take the panel's colour from the taskbar it docks to, lifted a little, so it reads as part of the same surface on
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
            for (int c = 0; c < 3; c++) {
                int seen = 0, v = 0;
                while (v < 255 && (seen += hist[c][v]) * 2 < w) v++;
                v = v * 122 / 100 + 2; // the lift: tray flyouts sit a shade above the taskbar
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
    if (hasCurve && !panelTest) fansReadRpm();
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
// Flags: fans wake system | light | x2 | nofans notadmin noread curveoff atcap nohist | hotkey wakes edited rgboff
//        hover=<id> drag=<point>

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
    linkOk = TRUE;
    hasCurve = fanChCount && curveN && !flag(cmd, L"nofans") && !flag(cmd, L"notadmin");
    fanWhy = hasCurve ? 0 : flag(cmd, L"notadmin") ? 2 : 1;
    // Every switch is faked, so previews and screenshots never depend on (or change) this machine's settings.
    setPref(L"RgbOn", !flag(cmd, L"rgboff")), setPref(L"Sync", 1), setPref(L"Start", 1), setPref(L"Admin", !flag(cmd, L"notadmin"));
    setPref(L"HotkeyMode", flag(cmd, L"hotkey")), setPref(L"WakeClick", 1), setPref(L"WakeEsc", 1), setPref(L"Edited", flag(cmd, L"edited"));
    coolant = flag(cmd, L"noread") ? -1 : wcsstr(cmd, L"--panel-shot") ? 28.6 : getDword(L"Coolant", 286) / 10.0;
    curveOn = !flag(cmd, L"curveoff");
    fanDuty = !curveOn || coolant < 0 ? -1 : flag(cmd, L"atcap") ? fanMax : curveAt(coolant);
    if (hasCurve && coolant >= 0 && !flag(cmd, L"nohist")) {
        // Preview data: a smooth warm-up that ends at the current reading.
        for (int i = 0; i < HISTORY; i++) {
            float t = (float)i / (HISTORY - 1);
            history[i] = (short)(coolant * 100 - 160 * (1 - t) * (1 - t) + 12 * t * (1 - t) * __builtin_sinf(t * 18));
        }
        historyLen = HISTORY, historyPos = 0;
    }
    for (int i = 0; i < fanChCount && i < 3; i++) fanRpm[i] = fanDuty < 0 ? 2990 + i * 6 : fanDuty * 34 + i * 17;
    if (flag(cmd, L"wakes")) {
        static const LPCWSTR why[3] = {L"Left-click (Hotkey mode)", L"Mouse: PRO X SUPERLIGHT 2c", L"No input seen (Windows or an app)"};
        for (wakeCount = 0; wakeCount < 3; wakeCount++) {
            GetLocalTime(&wakes[wakeCount].at);
            wakes[wakeCount].at.wHour = (WORD)(9 - wakeCount * 3), wakes[wakeCount].at.wMinute = (WORD)(13 + wakeCount * 17);
            lstrcpyW(wakes[wakeCount].cause, why[wakeCount]);
        }
    }
    if ((at = wcsstr(cmd, L"hover="))) hover = _wtoi(at + 6);
    if ((at = wcsstr(cmd, L"drag="))) dragPoint = _wtoi(at + 5);
    tabsBuild();
    int to = flag(cmd, L"fans") ? V_FANS : flag(cmd, L"wake") ? V_WAKE : flag(cmd, L"system") ? V_SYSTEM : V_MAIN;
    if (wcsstr(cmd, L"--panel-bench")) {
        panelBench();
        return 0;
    }
    if (wcsstr(cmd, L"--panel-shot")) {
        panelScale = flag(cmd, L"x2") ? 2 : 1;
        view = to;
        return out && panelShot(out + 4) ? 0 : 1;
    }
    if (!panelShow(NULL)) return 1;
    if (to != V_MAIN) panelGo(to);
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}
