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
#define COBJMACROS
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#define PSAPI_VERSION 2
#include <psapi.h>

#define PANEL_CLASS L"BlackoutPanel"
#define VERSION     L"1.1.0"
#define PANEL_W     372.0f
#define SET_H       520.0f // every Settings tab has this height, so the tabs never move under the cursor
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
    H_MAX_DN, H_MAX_UP, H_HYS_DN, H_HYS_UP, H_RESET, H_HOTKEY, H_WAKE_CLICK, H_WAKE_ESC, H_NOTICE, H_OVR_DN, H_OVR_UP, H_OVR,
    H_TAB_GPU, H_PROF1, H_PROF2, H_PROF3, H_PROF4, H_PROF5, H_XBAR_DN, H_XBAR_UP, H_GPU_APPLY, H_GPU_KEEP, H_GPU, H_GPU_NAME, H_COUNT
};
enum { F_BRAND, F_ROW, F_SMALL, F_BIG, F_UNIT, F_BOLD, F_TINY, F_COUNT };
enum { V_MAIN, V_FANS, V_WAKE, V_SYSTEM, V_GPU };

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
static int hover, dragPoint = -1, view = V_MAIN, settingsTab = V_FANS, tabs[4], tabCount;
static BOOL dockTop, panelLight, panelTest, panelHidden, dirty, gpuTab;
static DWORD panelClosedAt;
static D2D1_RECT_F chart;
static const float tMin = 25, tMax = 50;
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
static UINT knob(void) { return panelLight ? 0x1B1B1F : 0xFFFFFF; } // markers that sit on the background
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
// Marks a row that opens something.
static void pencil(float cx, float cy, UINT rgb, float a) {
    line(cx - 2.5f, cy + 2.5f, cx + 3.5f, cy - 3.5f, rgb, a, 2.4f);
    line(cx - 5, cy + 5, cx - 3.4f, cy + 3.4f, rgb, a, 1.2f);
}
static void chevronRight(float cx, float cy) {
    line(cx - 2, cy - 4.5f, cx + 2.5f, cy, dim(), 0.8f, 1.5f);
    line(cx + 2.5f, cy, cx - 2, cy + 4.5f, dim(), 0.8f, 1.5f);
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
// A stepper's two buttons as one control, 28 px high: [ - | + ]. The value is never between them; it is shown with its
// label, and this sits at the end of the row or under the chip. Each half is a target of at least 36 x 28.
static void stepper(int down, int up, float x, float y, float w, BOOL canDown, BOOL canUp, float a) {
    float half = w / 2;
    fillRound(rc(x, y, w, 28), 6, tone(), 0.07f * a);
    for (int i = 0; i < 2; i++) {
        int id = i ? up : down;
        BOOL can = i ? canUp : canDown;
        float hx = x + i * half, cx = hx + half / 2;
        if (can) hit[id] = rc(hx, y, half, 28);
        if (can && hover == id) fillRound(rc(hx + 2, y + 2, half - 4, 24), 4, tone(), 0.10f * a);
        line(cx - 5, y + 14, cx + 5, y + 14, ink(), (can ? 1 : 0.3f) * a, 1.5f);
        if (i) line(cx, y + 9, cx, y + 19, ink(), (can ? 1 : 0.3f) * a, 1.5f);
    }
    line(x + half, y + 7, x + half, y + 21, tone(), 0.16f * a, 1);
}
// A button with a word on it, 28 px high. kind: 0 plain, 1 the main action of the view, 2 asks to confirm,
// 3 quiet (small and dim: for what is rarely needed).
static void button(int id, LPCWSTR label, float x, float y, float w, int kind, BOOL can) {
    D2D1_RECT_F r = rc(x, y, w, 28);
    BOOL hot = can && hover == id;
    if (can) hit[id] = r;
    if (kind == 1 && can) fillRound(r, 6, ACCENT, hot ? 0.85f : 1);
    else if (kind == 2) fillRound(r, 6, warn(), hot ? 0.3f : 0.2f);
    else fillRound(r, 6, tone(), !can ? 0.03f : hot ? 0.12f : kind == 3 ? 0.04f : 0.06f);
    text(label, kind == 3 ? F_SMALL : F_BOLD, rc(x, y + (kind == 3 ? 6 : 5), w, 18), kind == 1 && can ? 0xFFFFFF : kind == 2 ? warn() : kind == 3 && !hot ? dim() : ink(),
         can ? 1 : 0.35f, DWRITE_TEXT_ALIGNMENT_CENTER);
}
// A reading: the number with its unit small beside it, and what it is underneath. Fan speeds on Main, a profile's
// numbers on the GPU tab. Numbers that are the point of a chip or a stepper row are set in F_BRAND, a size above
// their labels, so the eye lands on the value first.
static void chip(LPCWSTR value, LPCWSTR unit, LPCWSTR label, float x, float y, float w) {
    float vw = measure(value, F_BRAND), uw = unit ? measure(unit, F_SMALL) + 3 : 0, at = x + (w - vw - uw) / 2;
    fillRound(rc(x, y, w, 44), 6, tone(), 0.06f);
    if (vw + uw > w - 8) {
        text(value, F_BRAND, rc(x + 4, y + 3, w - 8, 20), ink(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        text(value, F_BRAND, rc(at, y + 3, vw + 2, 20), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (unit) text(unit, F_SMALL, rc(at + vw + 3, y + 7, uw, 16), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    text(label, F_SMALL, rc(x + 4, y + 23, w - 8, 16), dim(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
}
// A setting that is stepped: label and hint on the left, then the value with its unit, then [ - | + ] at the end of
// the row. One row holds one control. a dims the row.
static void stepperRow(LPCWSTR label, LPCWSTR hint, LPCWSTR value, LPCWSTR unit, float x, float y, float w, int down, int up, BOOL canDown, BOOL canUp, float a) {
    float px = x + w - INSET - 72, uw = measure(unit, F_SMALL), vw = measure(value, F_BRAND) + 2, vx = px - 12 - uw - 3 - vw;
    float lw = vx - 10 - x - INSET;
    text(label, F_ROW, rc(x + INSET, y + (hint ? 6 : 12), lw, 22), ink(), a, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (hint) text(hint, F_TINY, rc(x + INSET, y + 26, lw, 14), dim(), a, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(value, F_BRAND, rc(vx, y + 11, vw, 20), ink(), a, DWRITE_TEXT_ALIGNMENT_TRAILING);
    text(unit, F_SMALL, rc(px - 12 - uw, y + 15, uw + 2, 16), dim(), a, DWRITE_TEXT_ALIGNMENT_LEADING);
    stepper(down, up, px, y + 8, 72, canDown, canUp, a);
}

// ---- Layout ----

static float chartX(float t) { return chart.left + (t - tMin) / (tMax - tMin) * (chart.right - chart.left); }
static float chartY(float d) { return chart.bottom - d / 100 * (chart.bottom - chart.top); }
static int capped(int d) { return d > fanMax ? fanMax : d; }

static float mainHeight(void) {
    float h = PAD + 32 + SECT + LABEL + 2 * ROW;
    if (hasCurve) h += SECT + LABEL + INSET + 56 + GAP + 48 + GAP + 44 + INSET + 3 * ROW;
    else if (fanWhy >= 2) h += GAP + ROW; // notice: the fan curve is set up but can't run
    if (hasGpu()) h += SECT + LABEL + ROW;
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
                                  L"No known fan chip found: run blackout.exe --devices", L"Fan or sensor not found: check blackout.ini",
                                  L"Untested hardware: set untested=1 to use it"};
    return why[fanWhy < 0 || fanWhy > 6 ? 0 : fanWhy];
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

// The Fans tab is only offered when a fan curve is configured (working or not), the GPU tab when [gpu] sets it up
// or the PC has an NVIDIA driver (so the first profile can be made here).
static void tabsBuild(void) {
    BOOL found = FALSE;
    tabCount = 0;
    if (fanWhy != 1) tabs[tabCount++] = V_FANS;
    tabs[tabCount++] = V_WAKE;
    if (gpuTab) tabs[tabCount++] = V_GPU;
    tabs[tabCount++] = V_SYSTEM;
    for (int i = 0; i < tabCount; i++) found |= tabs[i] == settingsTab;
    if (!found) settingsTab = tabs[0];
}

static void paintTabs(float x, float y, float w) {
    static const LPCWSTR names[4] = {L"Fans", L"Wake", L"System", L"GPU"};
    float sw = (w - 6) / tabCount;
    fillRound(rc(x, y, w, 32), 8, tone(), 0.06f);
    for (int i = 0; i < tabCount; i++) {
        int id = tabs[i] == V_GPU ? H_TAB_GPU : H_TAB_FANS + tabs[i] - V_FANS;
        BOOL on = view == tabs[i];
        D2D1_RECT_F r = rc(x + 3 + i * sw, y + 3, sw, 26);
        hit[id] = rc(r.left, y, sw, 32);
        if (on) fillRound(r, 6, 0xFFFFFF, panelLight ? 0.9f : 0.13f);
        else if (hover == id) fillRound(r, 6, tone(), 0.06f);
        text(names[tabs[i] - V_FANS], on ? F_BOLD : F_ROW, rc(r.left, y + (on ? 7 : 6), sw, 20), on || hover == id ? ink() : dim(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

// What the card has been told. 2 = applied, 1 = refused or not possible (why says it), 0 = nothing sent yet.
static int gpuStatus(LPWSTR title, LPWSTR why) {
    why[0] = 0;
    if (!(panelTest ? pref(L"Admin", 1) : isElevated())) {
        lstrcpyW(title, L"Needs admin rights"), lstrcpyW(why, L"Run Blackout as admin to send this");
    } else if (gpuState == -2 || (gpuProfiles && gpuState && gpuState != -1)) {
        lstrcpyW(title, L"Not applied"), lstrcpyW(why, gpuNote);
    } else if (xbarCount && xbarState && xbarState != -1) {
        lstrcpyW(title, L"Not applied"), wsprintfW(why, L"XBAR refused (%d)", xbarState);
    } else if (gpuProfiles ? gpuState == -1 : xbarState == -1) {
        lstrcpyW(title, L"Not sent yet");
        return 0;
    } else {
        if (gpuProfiles) lstrcpyW(title, gpuNote);
        else wsprintfW(title, L"XBAR applied at %02d:%02d", xbarSentAt.wHour, xbarSentAt.wMinute);
        return 2;
    }
    return 1;
}

// The profile that is sent to the card (1 to gpuProfiles), 0 when there is none.
static int gpuActive(void) {
    int slot = (int)pref(L"GpuProfile", 1);
    return !gpuProfiles ? 0 : slot < 1 || slot > gpuProfiles ? 1 : slot;
}

// Main: which profile is on the card and whether the driver took it. From memory only; a click opens the GPU tab.
static void paintMainGpu(float x, float y, float w) {
    WCHAR s[64], why[64];
    float ix = x + INSET, right = x + w - INSET - 16;
    int slot = gpuActive(), mhz = (int)pref(L"Xbar", (DWORD)xbarDefault);
    if (!hasGpu()) return;
    section(L"GPU", x, &y, w);
    card(x, y, w, ROW);
    hit[H_GPU] = rc(x, y, w, ROW);
    hoverFill(H_GPU, hit[H_GPU]);
    int state = gpuWait ? 0 : gpuStatus(s, why);
    dot(ix + 4, y + 22, 3.5f, state == 2 ? GOOD : state == 1 ? warn() : dim(), state ? 1 : 0.5f);
    text(gpuWait ? L"Sending..." : state == 1 && why[0] ? why : s, F_TINY, rc(ix + 16, y + 26, right - ix - 16 - 70, 14), state == 1 ? warn() : dim(), 1,
         DWRITE_TEXT_ALIGNMENT_LEADING);
    if (slot && gpuNames[slot][0]) lstrcpyW(s, gpuNames[slot]);
    else if (slot) wsprintfW(s, L"Profile %d", slot);
    else lstrcpyW(s, L"XBAR offset");
    text(s, F_ROW, rc(ix + 16, y + 6, right - ix - 16 - (slot && xbarCount ? 70 : 0), 22), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (xbarCount) {
        wsprintfW(s, mhz ? L"+%d MHz" : L"%d MHz", mhz);
        text(s, F_SMALL, rc(right - 80, y + (slot ? 8 : 13), 80, 18), ink(), 0.85f, DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (slot) text(L"XBAR", F_TINY, rc(right - 80, y + 26, 80, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    chevronRight(x + w - INSET - 4, y + 22);
}

static void paintMain(float x, float y, float w) {
    WCHAR s[96];
    float ix = x + INSET, iw = w - 2 * INSET;
    section(L"LIGHTING", x, &y, w);
    card(x, y, w, 2 * ROW);
    toggleRow(L"RGB", NULL, x, y, w, H_RGB, panelTest ? pref(L"RgbOn", 1) : rgbShown(), TRUE);
    divider(x, y + ROW, w);
    toggleRow(L"Turn off with screens", NULL, x, y + ROW, w, H_SYNC, pref(L"Sync", 1), TRUE);
    y += 2 * ROW;
    if (!hasCurve) {
        if (fanWhy >= 2) {
            // Set up but not running: say so where it will be seen. Click for details.
            y += GAP;
            card(x, y, w, ROW);
            hit[H_NOTICE] = rc(x, y, w, ROW);
            hoverFill(H_NOTICE, hit[H_NOTICE]);
            dot(ix + 4, y + 22, 3.5f, warn(), 1);
            text(L"Fan curve is off", F_ROW, rc(ix + 16, y + 6, iw - 36, 22), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
            text(fanWhyText(), F_TINY, rc(ix + 16, y + 26, iw - 36, 14), warn(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
            chevronRight(x + w - INSET - 4, y + 22);
            y += ROW;
        }
        paintMainGpu(x, y + SECT, w);
        return;
    }
    y += SECT;

    section(L"COOLING", x, &y, w);
    card(x, y, w, INSET + 56 + GAP + 48 + GAP + 44 + INSET + 3 * ROW);
    float cy = y + INSET;
    // Two big readings on the left, the graph's range on the right.
    if (coolant < 0) lstrcpyW(s, L"--");
    else wsprintfW(s, L"%d.%d", (int)coolant, (int)(coolant * 10 + 0.5) % 10);
    text(s, F_BIG, rc(ix, cy - 4, 120, 40), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(L"\x00B0" L"C", F_UNIT, rc(ix + 2 + measure(s, F_BIG), cy + 10, 30, 22), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(srcLabel, F_SMALL, rc(ix, cy + 38, 120, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (fanDuty < 0) lstrcpyW(s, L"BIOS");
    else wsprintfW(s, L"%d", fanDuty);
    text(s, F_BIG, rc(ix + 118, cy - 4, 110, 40), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (fanDuty >= 0) text(L"%", F_UNIT, rc(ix + 120 + measure(s, F_BIG), cy + 10, 24, 22), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    int ov = ovState();
    text(ov == 1 ? L"Fans (override)" : ov == 2 ? (srcDev ? L"Override paused: too hot" : L"Override paused: water hot")
         : fanDuty >= 0 && fanMax < 100 && fanDuty >= fanMax ? L"Fans (at max)" : L"Fans",
         F_SMALL, rc(ix + 118, cy + 38, iw - 118, 18), ov == 2 ? warn() : dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
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
        text(srcDev ? L"No temperature reading. Fans are on the BIOS curve." : L"No water reading. Fans are on the BIOS curve.", F_SMALL, rc(ix, cy + 15, iw, 18), warn(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
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
    int chips = outCount < 3 ? outCount : 3;
    float cw = chips ? (iw - (chips - 1) * 8) / chips : iw;
    for (int i = 0; i < chips; i++) {
        WCHAR name[32], rpm[16];
        if (fanRpm[i] >= 1000 && fanRpm[i] < 10000) wsprintfW(rpm, L"%d,%03d", fanRpm[i] / 1000, fanRpm[i] % 1000);
        else if (fanRpm[i] > 0 && fanRpm[i] < 1000) wsprintfW(rpm, L"%d", fanRpm[i]);
        else lstrcpyW(rpm, L"-");
        chainName(i, name);
        chip(rpm, rpm[0] == L'-' ? NULL : L"rpm", name, ix + i * (cw + 8), cy, cw);
    }
    cy += 44 + INSET;
    divider(x, cy, w);
    toggleRow(srcDev ? L"Follow the temperature" : L"Follow water temperature", NULL, x, cy, w, H_CURVE, curveOn, TRUE);
    // Override: a fixed speed instead of the curve. Works while the curve is on; the speed can be set before switching on.
    cy += ROW;
    divider(x, cy, w);
    toggleRow(L"Override", ov == 2 ? L"" : L"A fixed speed instead of the curve", x, cy, w, H_OVR, ovOn, curveOn);
    if (ov == 2) text(L"Paused while hot", F_TINY, rc(ix, cy + 26, iw - 80, 14), warn(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    // Its speed on a row of its own: one row, one control.
    cy += ROW;
    divider(x, cy, w);
    wsprintfW(s, L"%d", ovDuty);
    stepperRow(L"Override speed", NULL, s, L"%", x, cy, w, H_OVR_DN, H_OVR_UP, curveOn && ovDuty > 20, curveOn && ovDuty < 100, !curveOn ? 0.4f : ovOn ? 1 : 0.6f);
    paintMainGpu(x, cy + ROW + SECT, w);
}

static void paintFans(float x, float y, float w) {
    WCHAR s[96];
    float ix = x + INSET, iw = w - 2 * INSET;
    BOOL off = !hasCurve;
    if (off) {
        // Why it is not running, then the curve that is set up, drawn grey: it is there, it is just not driving anything.
        card(x, y, w, 62);
        dot(ix + 4, y + 23, 3.5f, warn(), 1);
        text(L"The fan curve is off", F_ROW, rc(ix + 16, y + 12, iw - 16, 22), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        text(fanWhyText(), F_SMALL, rc(ix + 16, y + 34, iw - 16, 18), warn(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        y += 62 + GAP;
    }
    // One card to the bottom of the tab; the chart takes what the rows below it leave.
    float ch = panelH - PAD - y - (INSET + 6 + 18 + GAP + 18 + INSET + (off ? 0 : 3 * ROW));
    card(x, y, w, panelH - PAD - y);
    float cy = y + INSET;
    chart = rc(ix + 34, cy + 6, iw - 34 - 8, ch);
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
    geometry(p, n, FALSE, 0, off ? dim() : ACCENT, off ? 0.7f : 1, 2.5f);
    cy = chart.bottom + 18 + GAP;
    if (off) {
        text(L"The curve that is set up. Help: README, Fan curve.", F_SMALL, rc(ix, cy, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        return;
    }
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
    text(curveN < MAX_CURVE ? L"Drag a point. Double-click adds one, right-click removes it." : L"Drag a point. Right-click removes it.", F_SMALL,
         rc(ix, cy, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    cy += 18 + INSET;
    divider(x, cy, w);
    wsprintfW(s, L"%d", fanMax);
    stepperRow(L"Max fan speed", L"Upper limit for every fan", s, L"%", x, cy, w, H_MAX_DN, H_MAX_UP, fanMax > 30, fanMax < 100, 1);
    cy += ROW;
    divider(x, cy, w);
    wsprintfW(s, L"%d", fanHyst);
    stepperRow(L"Slow down after", srcDev ? L"It must be this much cooler first" : L"Water must be this much cooler first", s, L"\x00B0" L"C", x, cy, w, H_HYS_DN, H_HYS_UP, fanHyst > 0, fanHyst < 5, 1);
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
    float h = panelH - PAD - y;
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
    dot(x + INSET + 4, y + 14, 3.5f, state == 2 ? GOOD : state == 1 ? warn() : dim(), state ? 1 : 0.5f);
    text(label, F_SMALL, rc(x + INSET + 16, y + 6, 120, 18), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(value, F_SMALL, rc(x + INSET + lw, y + 6, w - 2 * INSET - lw, 18), state == 1 ? warn() : dim(), 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

static void paintSystem(float x, float y, float w) {
    WCHAR s[96], why[64];
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
    card(x, y, w, 8 * 28 + 8);
    y += 4;
    // The cause only: the remedy after the colon is on the Fans tab and the Main view, which have the room.
    lstrcpynW(s, fanWhyText(), 96);
    if (wcschr(s, L':')) *wcschr(s, L':') = 0;
    if (!fanWhy) wsprintfW(s, L"Active on %d fan%s", outCount, outCount == 1 ? L"" : L"s");
    statusRow(L"Fan curve", fanWhy == 1 ? L"Not set up" : s, fanWhy == 0 ? 2 : fanWhy == 1 ? 0 : 1, x, y, w);
    if (coolant >= 0) wsprintfW(s, L"%d.%d \x00B0" L"C from %s", (int)coolant, (int)(coolant * 10 + 0.5) % 10, srcFrom);
    statusRow(srcDev ? L"Temperature" : L"Water sensor", coolant >= 0 ? s : hasCurve ? L"No reading" : L"Not used", coolant >= 0 ? 2 : hasCurve ? 1 : 0, x, y + 28, w);
    wsprintfW(s, L"Hub connected, %d LEDs", linkLeds);
    statusRow(L"Pump lighting", !linkLeds ? L"Not set up" : linkOk ? s : L"Hub not answering", !linkLeds ? 0 : linkOk ? 2 : 1, x, y + 56, w);
    statusRow(L"OpenRGB", hasOpenRgb ? L"Found" : openrgb[0] ? L"Not found at the path in blackout.ini" : L"Not set up", hasOpenRgb ? 2 : openrgb[0] ? 1 : 0, x,
              y + 84, w);
    statusRow(L"RAM lighting", hasRam ? L"FURY module found" : L"Not installed", hasRam ? 2 : 0, x, y + 112, w);
    statusRow(L"Monitor power", ddcIds[0] ? ddcIds : L"Not set up", ddcIds[0] ? 2 : 0, x, y + 140, w);
    BOOL admin = panelTest ? pref(L"Admin", 1) : isElevated();
    statusRow(L"Admin rights", admin ? L"Yes" : L"No, so the fan curve can't run", admin ? 2 : outCount ? 1 : 0, x, y + 168, w);
    int gpu = hasGpu() ? gpuStatus(s, why) : 0;
    statusRow(L"GPU profile", !hasGpu() ? L"Not set up" : gpu == 1 && why[0] ? why : s, gpu, x, y + 196, w);
    text(L"Blackout " VERSION L"  \x00B7  github.com/Xipzer/blackout-tray", F_TINY, rc(x + 2, panelH - PAD - 14, w, 14), dim(), 0.8f,
         DWRITE_TEXT_ALIGNMENT_LEADING);
}

// GPU tab: one profile drawn as a picture. Profile pills, then a card with the clock curve over the card's own curve,
// its numbers as chips, and what the driver said last. Nothing is read from the card because the panel is open
// (driver reads cost frame time): the curve is the saved one, the status is what the driver answered when it was sent.
// A pill only shows a profile. It is sent when Apply is pressed, so looking is safe and a slip costs nothing.
static int viewSlot = -1, viewPower, viewMemory, viewCount, viewDelta[NV_SLOTS];
static int fakePower, fakeMemory = -1, fakeMismatch; // --panel-shot: power= mem= mismatch
static int gpuShown;   // the profile on show, 0 = the active one
static BOOL keepArmed; // "Read from card" was pressed on a profile that exists: the next press replaces it
static BOOL naming;    // the profile's name is being typed (click the name; Enter keeps it, Esc drops it)
static WCHAR nameBuf[24];

static void gpuView(int slot) {
    if (slot == viewSlot) return;
    viewSlot = slot;
    if (!panelTest) {
        viewCount = gpuLoad(slot, &viewPower, &viewMemory, viewDelta);
        return;
    }
    // Preview: profile 1 follows the card's curve 480 MHz up, flattens at 3,090 and has a raised top; profile 2 is
    // capped at 3,075, below the card's own top end; the others are plain offsets.
    viewPower = fakePower ? fakePower : slot == 1 ? 121739 : 104000, viewMemory = fakeMemory != -1 ? fakeMemory : slot == 1 ? 3000000 : 2000000;
    viewCount = gpuPoints - fakeMismatch;
    for (int i = 0; i < gpuPoints; i++) {
        int top = gpuVolt[i] <= 1100 ? 3090 : gpuVolt[i] >= 1150 ? 3440 : 3090 + (gpuVolt[i] - 1100) * 7;
        int want = slot == 1 ? top : slot == 2 ? 3075 : 9999, up = slot == 1 ? 480 : slot == 2 ? 400 : 420 - slot * 60;
        viewDelta[i] = (gpuBase[i] + up > want ? want - gpuBase[i] : up) * 1000;
    }
}

static void thousands(LPWSTR out, int v) {
    if (v >= 1000) wsprintfW(out, L"%d,%03d", v / 1000, v % 1000);
    else wsprintfW(out, L"%d", v);
}

// The profile on show: the active one until a pill is clicked. One past the last is the empty profile behind "+".
static int gpuOnShow(void) {
    int slots = gpuProfiles + (gpuProfiles < 5);
    return gpuShown >= 1 && gpuShown <= slots ? gpuShown : gpuProfiles ? gpuActive() : 1;
}

static void paintGpu(float x, float y, float w) {
    WCHAR s[96], title[64], why[64];
    float ix = x + INSET, iw = w - 2 * INSET, bottom = panelH - PAD;
    int active = gpuActive(), shown = gpuOnShow(), mhz = (int)pref(L"Xbar", (DWORD)xbarDefault), at = 0;
    int state = gpuStatus(title, why);
    BOOL empty = shown > gpuProfiles, live = !empty && shown == active && state == 2 && !gpuWait;
    BOOL admin = panelTest ? pref(L"Admin", 1) : isElevated(), again = live || (!gpuProfiles && xbarCount && state == 2 && !gpuWait);
    for (int i = 0; i < xbarCount; i++)
        if (xbarSteps[i] == mhz) at = i;
    if (!empty) gpuView(shown);

    // Profiles. Red is the one on the card, the lighter pill is the one on show, "+" is the next empty one.
    // A profile without a name gets a small pill like "+", so the names get the room.
    if (gpuProfiles) {
        int named = 0;
        for (int i = 1; i <= gpuProfiles; i++) named += gpuNames[i][0] != 0;
        float plus = gpuProfiles < 5 ? 36 : 0, px = x + 3;
        float pw = named ? (w - 6 - plus - (gpuProfiles - named) * 36) / named : (w - 6 - plus) / gpuProfiles;
        fillRound(rc(x, y, w, 32), 8, tone(), 0.06f);
        for (int i = 1; i <= gpuProfiles + (plus > 0); i++) {
            int id = H_PROF1 + i - 1;
            D2D1_RECT_F r = rc(px, y + 3, i > gpuProfiles || (named && !gpuNames[i][0]) ? 36 : pw, 26);
            BOOL on = i == active && state == 2 && !gpuWait;
            float cx = (r.left + r.right) / 2;
            px = r.right;
            hit[id] = rc(r.left, y, r.right - r.left, 32);
            if (on) fillRound(r, 6, ACCENT, 0.92f);
            else if (i == shown) fillRound(r, 6, 0xFFFFFF, panelLight ? 0.9f : 0.13f);
            else if (hover == id) fillRound(r, 6, tone(), 0.06f);
            if (i == active && !on) strokeRound(rc(r.left + 0.5f, r.top + 0.5f, r.right - r.left - 1, 25), 6, ACCENT, 0.9f, 1);
            UINT c = on ? 0xFFFFFF : i == shown || hover == id ? ink() : dim();
            if (i > gpuProfiles) {
                line(cx - 5, y + 16, cx + 5, y + 16, c, 1, 1.5f);
                line(cx, y + 11, cx, y + 21, c, 1, 1.5f);
            } else {
                wsprintfW(s, L"%d", i);
                text(gpuNames[i][0] ? gpuNames[i] : s, F_SMALL, rc(r.left + 6, y + 8, r.right - r.left - 12, 18), c, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
        y += 32 + GAP;
    }

    // One card to the bottom of the tab: title, curve, chips, status.
    card(x, y, w, bottom - y);
    if (naming) lstrcpyW(s, nameBuf);
    else if (!empty && gpuNames[shown][0]) lstrcpyW(s, gpuNames[shown]);
    else wsprintfW(s, L"Profile %d", shown);
    float nw = measure(s, F_BOLD);
    if (nw > iw - 140) nw = iw - 140;
    if (naming) {
        // The name as a field: what has been typed, and a caret.
        fillRound(rc(ix - 8, y + 8, iw - 106, 28), 6, tone(), 0.08f);
        strokeRound(rc(ix - 7.5f, y + 8.5f, iw - 107, 27), 6, ACCENT, 0.9f, 1);
        line(ix + nw + 1.5f, y + 14, ix + nw + 1.5f, y + 30, ink(), 1, 1);
    } else if (!empty && !gpuWait) {
        hit[H_GPU_NAME] = rc(ix - 8, y + 8, nw + 34, 28);
        if (hover == H_GPU_NAME) fillRound(hit[H_GPU_NAME], 6, tone(), 0.06f);
        pencil(ix + nw + 13, y + 22, dim(), hover == H_GPU_NAME ? 1 : 0.6f);
    }
    text(s, F_BOLD, rc(ix, y + 13, iw - 140, 18), ink(), empty ? 0.7f : 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    // The way in: keep what the card is running now (set by any tuning app) as this profile.
    if (keepArmed && !empty) button(H_GPU_KEEP, L"Replace it?", x + w - INSET - 106, y + 8, 106, 2, !gpuWait);
    else button(H_GPU_KEEP, L"Read from card", x + w - INSET - 106, y + 8, 106, empty ? 1 : 3, !gpuWait);

    // Under the chart: three equal chips (power limit, memory offset, XBAR offset), and under a chip that can be
    // stepped its [ - | + ]. Today that is XBAR, with a note beside it on why only this one has buttons; the other
    // two take the same control under their chips when they become adjustable.
    int chips = (empty ? 0 : 2) + (xbarCount > 0);
    float cw = xbarCount ? (iw - 16) / 3 : (iw - 8) / 2, chipsY = bottom - ROW - INSET - 44 - (xbarCount ? 34 : 0);
    D2D1_RECT_F g = rc(ix + 30, y + 46, iw - 30 - 14, (chips ? chipsY - GAP : bottom - ROW - INSET) - 18 - (y + 46));

    // The curve: MHz over volts, this profile over the card's own curve. The scale follows what is drawn.
    const float v0 = 800, v1 = 1200;
    BOOL drawn = !empty && gpuPoints && viewCount == gpuPoints;
    int lo = 1000, hi = 3500, peak = 0, peakAt = -1;
    if (gpuPoints) lo = 99999, hi = 0;
    for (int i = 0; i < gpuPoints; i++) {
        int f = gpuBase[i] + (drawn ? viewDelta[i] / 1000 : 0);
        if (drawn && f > peak) peak = f, peakAt = i;
        if (gpuVolt[i] < v0 || gpuVolt[i] > v1) continue;
        if (gpuBase[i] < lo) lo = gpuBase[i];
        if ((f > gpuBase[i] ? f : gpuBase[i]) > hi) hi = f > gpuBase[i] ? f : gpuBase[i];
    }
    if (lo > hi) lo = 1000, hi = 3500;
    float f0 = (float)(lo / 500 * 500), f1 = (float)((hi + 499) / 500 * 500);
    if (f1 - f0 < 1000) f1 = f0 + 1000;
#define GX(v) (g.left + ((v) - v0) / (v1 - v0) * (g.right - g.left))
#define GY(f) (g.bottom - ((f) < f0 ? 0 : (f) > f1 ? 1 : ((f) - f0) / (f1 - f0)) * (g.bottom - g.top))
    for (int f = (int)f0; f <= (int)f1; f += 500) {
        line(g.left, GY((float)f), g.right, GY((float)f), tone(), 0.07f, 1);
        if (f % 1000 || f == (int)f0) continue;
        thousands(s, f);
        text(s, F_TINY, rc(ix - 6, GY((float)f) - 7, 32, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    for (int v = 800; v <= 1200; v += 100) {
        line(GX((float)v), g.top, GX((float)v), g.bottom, tone(), 0.07f, 1);
        if (v % 200) continue;
        wsprintfW(s, L"%d.%d V", v / 1000, v % 1000 / 100);
        text(s, F_TINY, rc(GX((float)v) - 24, g.bottom + 4, 48, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (gpuPoints) {
        D2D1_POINT_2F stock[NV_SLOTS], tuned[NV_SLOTS];
        int n = 0;
        for (int i = 0; i < gpuPoints; i++) {
            if (gpuVolt[i] < v0 || gpuVolt[i] > v1) continue;
            stock[n] = pt(GX((float)gpuVolt[i]), GY((float)gpuBase[i]));
            tuned[n++] = pt(GX((float)gpuVolt[i]), GY((float)(gpuBase[i] + viewDelta[i] / 1000)));
        }
        if (drawn) geometry(tuned, n, TRUE, g.bottom, live ? ACCENT : tone(), live ? 0.13f : 0.05f, 0);
        geometry(stock, n, FALSE, 0, tone(), 0.3f, 1.5f);
        if (drawn) geometry(tuned, n, FALSE, 0, live ? ACCENT : ink(), live ? 1 : 0.8f, 2.5f);
    }
    if (drawn && peakAt >= 0) {
        // The top of the curve and the lowest voltage that reaches it, written beside its marker.
        WCHAR volt[24];
        float px = GX((float)gpuVolt[peakAt]), py = GY((float)peak);
        thousands(s, peak);
        lstrcatW(s, L" MHz");
        wsprintfW(volt, L"at %d.%03d V", gpuVolt[peakAt] / 1000, gpuVolt[peakAt] % 1000);
        float lw = measure(s, F_BOLD), vw = measure(volt, F_SMALL);
        if (vw > lw) lw = vw;
        BOOL inPlot = gpuVolt[peakAt] >= v0 && gpuVolt[peakAt] <= v1;
        if (!inPlot) px = g.right;
        float lx = px + 8 + lw <= g.right ? px + 8 : px - 8 - lw;
        if (inPlot) {
            dashed(px, py, px, g.bottom, tone(), 0.4f);
            dot(px, py, 3.5f, knob(), 1);
        }
        text(s, F_BOLD, rc(lx, g.bottom - 38, lw + 2, 18), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        text(volt, F_SMALL, rc(lx, g.bottom - 21, lw + 2, 16), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    } else {
        // Under the card's own curve when that is drawn (it rises to the right), in the middle when the plot is bare.
        D2D1_RECT_F m = rc(g.left, gpuPoints ? g.bottom - 44 : (g.top + g.bottom) / 2 - 18, g.right - g.left - (gpuPoints ? 10 : 0), 18);
        DWRITE_TEXT_ALIGNMENT side = gpuPoints ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_CENTER;
        text(empty ? L"Nothing saved here yet" : gpuPoints ? L"Saved for a different card" : L"No picture of the curve yet", F_SMALL, m, ink(), 0.7f, side);
        m.top += 18, m.bottom += 16;
        text(empty ? L"Tune the card in any app, then read it in" : gpuPoints ? L"Read from card replaces it with this card's" : L"It is drawn once the profile has been sent",
             F_TINY, m, dim(), 1, side);
    }
#undef GX
#undef GY

    if (!empty) {
        if (viewPower) wsprintfW(s, L"%d", viewPower / 1000);
        else lstrcpyW(s, L"-");
        chip(s, viewPower ? L"%" : NULL, L"Power limit", ix, chipsY, cw);
        wsprintfW(s, viewMemory > 0 ? L"+%d" : L"%d", viewMemory / 1000);
        chip(s, L"MHz", L"Memory offset", ix + cw + 8, chipsY, cw);
    }
    if (xbarCount) {
        // XBAR belongs to the card, not to a profile, and is sent a moment after the last step.
        float cx = ix + 2 * (cw + 8);
        wsprintfW(s, mhz ? L"+%d" : L"%d", mhz);
        chip(s, L"MHz", L"XBAR offset", cx, chipsY, cw);
        stepper(H_XBAR_DN, H_XBAR_UP, cx, chipsY + 50, cw, at > 0 && !gpuWait, at < xbarCount - 1 && !gpuWait, 1);
        if (gpuProfiles) text(L"XBAR is kept across profiles", F_TINY, rc(ix + 2, chipsY + 57, 2 * cw, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // What the driver said last time, and the button that sends this profile.
    float ay = bottom - ROW;
    divider(x, ay, w);
    BOOL sendable = !empty || (!gpuProfiles && xbarCount);
    if (gpuWait) {
        lstrcpyW(title, gpuWait == 2 ? L"Reading the card..." : L"Sending..."), why[0] = 0, state = 0;
    } else if (naming) {
        lstrcpyW(title, L"Naming the profile"), lstrcpyW(why, L"Enter keeps the name, Esc drops it"), state = 0;
    } else if (keepArmed && !empty) {
        lstrcpyW(title, L"Replace this profile?"), lstrcpyW(why, L"With what the card is running now"), state = 1;
    } else if (gpuKept == shown && gpuKeptState) {
        lstrcpyW(title, L"Card not read"), state = 1;
        if (gpuKeptState == -2) lstrcpyW(why, L"No NVIDIA driver found");
        else if (gpuKeptState == -1002) lstrcpyW(why, L"blackout.ini could not be written");
        else if (gpuKeptState == -1000) lstrcpyW(why, L"The card shows no curve points");
        else wsprintfW(why, L"The driver answered %d", gpuKeptState);
    } else if (!sendable) {
        lstrcpyW(title, L"Empty profile"), why[0] = 0, state = 0;
    } else if (!empty && shown != active && admin) {
        lstrcpyW(title, L"Not on the card"), why[0] = 0, state = 0;
    }
    dot(ix + 4, ay + 22, 3.5f, state == 2 ? GOOD : state == 1 ? warn() : dim(), state ? 1 : 0.5f);
    text(title, F_ROW, rc(ix + 16, ay + (why[0] ? 6 : 12), iw - 16 - 88, 22), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (why[0]) text(why, F_TINY, rc(ix + 16, ay + 26, iw - 16 - 88, 14), state == 1 ? warn() : dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    button(H_GPU_APPLY, again ? L"Re-apply" : L"Apply", x + w - INSET - 80, ay + 8, 80, !again, sendable && admin && !gpuWait);
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
    else if (view == V_GPU) paintGpu(x, y, w);
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
    view = to, hover = H_NONE, keepArmed = FALSE, naming = FALSE;
    if (to != V_MAIN) settingsTab = to;
    GetWindowRect(panel, &r);
    panelH = viewHeight();
    POINT pos = {r.left, dockTop ? r.top : r.bottom - (int)(panelH * panelScale + 0.5f)};
    panelRender(&pos);
}

// Ends typing a profile's name. keep writes it to [gpu.N] name= in blackout.ini.
static void nameDone(BOOL keep) {
    WCHAR sec[16];
    int slot = gpuOnShow();
    if (!naming) return;
    naming = FALSE;
    if (keep && slot <= gpuProfiles) {
        lstrcpyW(gpuNames[slot], nameBuf);
        wsprintfW(sec, L"gpu.%d", slot);
        if (!panelTest) WritePrivateProfileStringW(sec, L"name", nameBuf, iniPath), tipShown = -2, tipUpdate();
    }
    panelDirty();
}

static void panelClick(int id) {
    nameDone(TRUE); // a click elsewhere keeps what was typed
    if (keepArmed && id != H_GPU_KEEP) keepArmed = FALSE, panelDirty(); // any other click takes the question back
    if (id) gpuKept = 0;
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
    case H_TAB_GPU:
    case H_GPU: panelGo(V_GPU); return;
    case H_PROF1:
    case H_PROF2:
    case H_PROF3:
    case H_PROF4:
    case H_PROF5: gpuShown = id - H_PROF1 + 1; break; // only shows it: nothing is sent until Apply
    case H_GPU_NAME: naming = TRUE, lstrcpyW(nameBuf, gpuNames[gpuOnShow()]); break;
    case H_XBAR_DN:
    case H_XBAR_UP: {
        int at = 0, mhz = (int)pref(L"Xbar", (DWORD)xbarDefault);
        for (int i = 0; i < xbarCount; i++)
            if (xbarSteps[i] == mhz) at = i;
        at += id == H_XBAR_UP ? 1 : -1;
        setPref(L"Xbar", xbarSteps[at < 0 ? 0 : at >= xbarCount ? xbarCount - 1 : at]);
        // Sent a moment after the last step, so stepping through values does not write each one to the card.
        if (!panelTest) gpuWait = 1, SetTimer(panelOwner, TIMER_XBAR, 700, NULL);
        break;
    }
    case H_GPU_APPLY:
        if (gpuOnShow() <= gpuProfiles) setPref(L"GpuProfile", gpuOnShow());
        if (panelTest) gpuSay(0, L""), xbarState = 0;
        else gpuWait = 1, SetTimer(panelOwner, TIMER_GPU, 100, NULL);
        break;
    case H_GPU_KEEP:
        // Reads the card once, on the worker thread, and writes the profile to blackout.ini. A profile that exists
        // is only replaced on a second press.
        gpuKeepSlot = gpuOnShow();
        if (gpuKeepSlot <= gpuProfiles && !keepArmed) {
            keepArmed = TRUE;
        } else if (panelTest) {
            if (gpuKeepSlot > gpuProfiles) gpuProfiles = gpuKeepSlot, gpuNames[gpuKeepSlot][0] = 0;
            keepArmed = FALSE, viewSlot = -1, setPref(L"GpuProfile", gpuKeepSlot), gpuSay(0, L""), xbarState = 0;
        } else {
            keepArmed = FALSE, gpuWait = 2, SetTimer(panelOwner, TIMER_KEEP, 50, NULL);
        }
        break;
    case H_RGB:
        if (panelTest) setPref(L"RgbOn", !pref(L"RgbOn", 1));
        else rgbToggle(panelOwner);
        break;
    case H_SYNC: setPref(L"Sync", !pref(L"Sync", 1)); break;
    case H_CURVE:
        if (panelTest) curveOn = !curveOn, fanDuty = curveOn ? curveAt(coolant) : -1;
        else SendMessageW(panelOwner, WM_COMMAND, ID_CURVE, 0);
        break;
    case H_OVR:
        if (panelTest) ovOn = !ovOn, fanDuty = coolant < 0 ? -1 : fanTarget(TRUE);
        else SendMessageW(panelOwner, WM_COMMAND, ID_OVERRIDE, 0);
        break;
    case H_OVR_DN:
    case H_OVR_UP:
        ovDuty += id == H_OVR_UP ? 5 : -5;
        ovDuty = ovDuty < 20 ? 20 : ovDuty > 100 ? 100 : ovDuty;
        setPref(L"OverrideDuty", ovDuty);
        if (panelTest) fanDuty = coolant < 0 ? -1 : fanTarget(TRUE);
        else if (ovOn) curveApplyNow(), tipUpdate();
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
    case WM_CHAR:
        if (!naming) return 0;
        if (wp == L'\r') nameDone(TRUE);
        else if (wp == L'\b' && nameBuf[0]) nameBuf[lstrlenW(nameBuf) - 1] = 0;
        else if (wp >= 32 && wp != 127 && lstrlenW(nameBuf) < 23) nameBuf[lstrlenW(nameBuf) + 1] = 0, nameBuf[lstrlenW(nameBuf)] = (WCHAR)wp;
        panelDirty();
        return 0;
    case WM_KEYDOWN:
        if (naming) {
            if (wp == VK_ESCAPE) nameDone(FALSE);
            return 0; // the keys belong to the name: no tab switching, and Esc only leaves the field
        }
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
        hover = H_NONE, dragPoint = -1, view = V_MAIN, dirty = FALSE, gpuShown = 0, keepArmed = FALSE, gpuKept = 0, naming = FALSE;
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
    if (!panelTest) gpuTab = gpuOffered();
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
// Flags: fans wake system gpu | light top | x2 | nofans notadmin noread curveoff atcap nohist override ovhot | hotkey wakes edited rgboff
//        hover=<id> drag=<point> shade=<percent> grey=<percent>
// Extremes: temp=<C x10> rpm=<n> max=<percent> hyst=<C> ovduty=<percent> names longnames board fanwhy=<2..6> longstatus longwakes
//           profiles=<0..5> slot=<n> longname power=<percent> mem=<MHz> xbar=<MHz> noxbar unsent nobase mismatch
//           gpustate=<code> what=<0..2> xbarstate=<code> view=<n> busy reading armed keptfail naming | withgpu scale=<percent>

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
    hasCurve = outCount && curveN && !flag(cmd, L"nofans") && !flag(cmd, L"notadmin") && !wcsstr(cmd, L"fanwhy=");
    fanWhy = hasCurve ? 0 : flag(cmd, L"notadmin") ? 2 : (at = wcsstr(cmd, L"fanwhy=")) ? _wtoi(at + 7) : 1;
    if ((at = wcsstr(cmd, L"max="))) fanMax = _wtoi(at + 4);
    if (flag(cmd, L"names")) lstrcpyW(fanNames, L"Top,Side,Bottom");
    if (flag(cmd, L"longnames")) lstrcpyW(fanNames, L"Radiator top exhaust,Side intake,Bottom radiator intake");
    if (flag(cmd, L"board")) srcDev = (Dev *)(INT_PTR)1, lstrcpyW(srcLabel, L"AUXTIN2"), lstrcpyW(srcFrom, L"Nuvoton NCT6796D-R"); // never read
    if (flag(cmd, L"longstatus")) {
        lstrcpyW(srcFrom, L"Aquacomputer Farbwerk 360"), lstrcpyW(ddcIds, L"BNQ7F2F,DELA0EC,GSM5B7F,AUS27B1");
        lstrcpyW(openrgb, L"C:\\missing\\OpenRGB.exe"), linkLeds = 22;
    }
    // Every switch is faked, so previews and screenshots never depend on (or change) this machine's settings.
    setPref(L"RgbOn", !flag(cmd, L"rgboff")), setPref(L"Sync", 1), setPref(L"Start", 1), setPref(L"Admin", !flag(cmd, L"notadmin"));
    setPref(L"HotkeyMode", flag(cmd, L"hotkey")), setPref(L"WakeClick", 1), setPref(L"WakeEsc", 1), setPref(L"Edited", flag(cmd, L"edited"));
    coolant = flag(cmd, L"noread") ? -1 : (at = wcsstr(cmd, L"temp=")) ? _wtoi(at + 5) / 10.0 : wcsstr(cmd, L"--panel-shot") ? 28.6 : getDword(L"Coolant", 286) / 10.0;
    curveOn = !flag(cmd, L"curveoff");
    ovOn = flag(cmd, L"override") || flag(cmd, L"ovhot"), ovDuty = (at = wcsstr(cmd, L"ovduty=")) ? _wtoi(at + 7) : 60;
    if ((at = wcsstr(cmd, L"hyst="))) fanHyst = _wtoi(at + 5);
    if (hasCurve && coolant >= 0 && flag(cmd, L"ovhot")) {
        double top = 0;
        curvePeak(&top);
        coolant = top + 1.5; // past the top of the curve: the override pauses
    }
    fanDuty = !curveOn || coolant < 0 ? -1 : flag(cmd, L"atcap") ? fanMax : fanTarget(TRUE);
    if (hasCurve && coolant >= 0 && !flag(cmd, L"nohist")) {
        // Preview data: a smooth warm-up that ends at the current reading.
        for (int i = 0; i < HISTORY; i++) {
            float t = (float)i / (HISTORY - 1);
            history[i] = (short)(coolant * 100 - 160 * (1 - t) * (1 - t) + 12 * t * (1 - t) * __builtin_sinf(t * 18));
        }
        historyLen = HISTORY, historyPos = 0;
    }
    for (int i = 0; i < outCount && i < 3; i++) fanRpm[i] = (at = wcsstr(cmd, L"rpm=")) ? _wtoi(at + 4) - i * 7 : fanDuty < 0 ? 2990 + i * 6 : fanDuty * 34 + i * 17;
    if (flag(cmd, L"wakes")) {
        static const LPCWSTR why[3] = {L"Left-click (Hotkey mode)", L"Mouse: PRO X SUPERLIGHT 2c", L"No input seen (Windows or an app)"};
        for (wakeCount = 0; wakeCount < 3; wakeCount++) {
            GetLocalTime(&wakes[wakeCount].at);
            wakes[wakeCount].at.wHour = (WORD)(9 - wakeCount * 3), wakes[wakeCount].at.wMinute = (WORD)(13 + wakeCount * 17);
            lstrcpyW(wakes[wakeCount].cause, why[wakeCount]);
        }
    }
    if (flag(cmd, L"longwakes")) {
        // The longest causes wakeRecord() can write: a 40 character product name, with and without "(not blocked)".
        static const LPCWSTR why[MAX_WAKES] = {L"Keyboard: Razer BlackWidow V4 Pro 75% Wireless Ke (not blocked)", L"Mouse: Logitech G502 X PLUS LIGHTSPEED Wireless (not blocked)",
                                               L"Input device: USB Composite Device Receiver 2.4G", L"Esc (Hotkey mode)", L"No input seen (Windows or an app)"};
        for (wakeCount = 0; wakeCount < MAX_WAKES; wakeCount++) {
            GetLocalTime(&wakes[wakeCount].at);
            wakes[wakeCount].at.wHour = (WORD)(23 - wakeCount * 5), wakes[wakeCount].at.wMinute = (WORD)(48 - wakeCount * 9);
            lstrcpyW(wakes[wakeCount].cause, why[wakeCount]);
        }
    }
    dockTop = flag(cmd, L"top");
    if ((at = wcsstr(cmd, L"shade="))) shade = _wtoi(at + 6); // try other backgrounds without rebuilding
    if ((at = wcsstr(cmd, L"grey="))) grey = _wtoi(at + 5);
    if ((at = wcsstr(cmd, L"hover="))) hover = _wtoi(at + 6);
    if ((at = wcsstr(cmd, L"drag="))) dragPoint = _wtoi(at + 5);
    if ((gpuTab = flag(cmd, L"gpu") || flag(cmd, L"withgpu"))) {
        // Preview: five profiles on a card shaped like a real one, four XBAR steps, everything sent at 09:15.
        static const int kv[7] = {450, 800, 850, 950, 1000, 1100, 1240}, kf[7] = {180, 700, 1320, 2535, 2715, 2947, 3172};
        for (gpuPoints = 0; gpuPoints < 127; gpuPoints++) {
            int v = 450 + gpuPoints * 790 / 126, k = 0;
            while (k < 5 && v > kv[k + 1]) k++;
            gpuVolt[gpuPoints] = v, gpuBase[gpuPoints] = kf[k] + (kf[k + 1] - kf[k]) * (v - kv[k]) / (kv[k + 1] - kv[k]);
        }
        xbarCount = 4, xbarSteps[0] = 0, xbarSteps[1] = 300, xbarSteps[2] = 400, xbarSteps[3] = 500, xbarDefault = 400, gpuProfiles = 5;
        for (int i = 1; i <= 5; i++) gpuNames[i][0] = 0;
        lstrcpyW(gpuNames[1], L"Maximum"), lstrcpyW(gpuNames[2], L"Reliable");
        if (flag(cmd, L"fivenames"))
            lstrcpyW(gpuNames[1], L"Maximum"), lstrcpyW(gpuNames[2], L"Reliable"), lstrcpyW(gpuNames[3], L"Fallback"), lstrcpyW(gpuNames[4], L"Compute"), lstrcpyW(gpuNames[5], L"Stock");
        if (flag(cmd, L"longname")) lstrcpyW(gpuNames[1], L"Maximum, top 3,440 MHz"), lstrcpyW(gpuNames[2], L"Reliable, cap 3,075 MHz"), lstrcpyW(gpuNames[3], L"Quiet, cap 2,900 MHz");
        if ((at = wcsstr(cmd, L"profiles="))) gpuProfiles = _wtoi(at + 9);
        if (flag(cmd, L"noxbar")) xbarCount = 0;
        if ((at = wcsstr(cmd, L"power="))) fakePower = _wtoi(at + 6) * 1000 + 739;
        if ((at = wcsstr(cmd, L"mem="))) fakeMemory = _wtoi(at + 4) * 1000;
        fakeMismatch = flag(cmd, L"mismatch");
        if (flag(cmd, L"nobase")) gpuPoints = 0;
        setPref(L"Xbar", (at = wcsstr(cmd, L" xbar=")) ? _wtoi(at + 6) : 400), setPref(L"GpuProfile", (at = wcsstr(cmd, L"slot=")) ? _wtoi(at + 5) : 1);
        gpuState = xbarState = 0, xbarSent = 400, xbarSentAt.wHour = 9, xbarSentAt.wMinute = 15, lstrcpyW(gpuNote, L"Applied at 09:15");
        // Every answer gpuSay() and gpuThread() can leave behind.
        if ((at = wcsstr(cmd, L"gpustate="))) {
            static const LPCWSTR what[3] = {L"Power limit", L"Memory offset", L"Curve"};
            LPCWSTR w = wcsstr(cmd, L"what=");
            gpuSay(_wtoi(at + 9), what[w ? _wtoi(w + 5) % 3 : 0]);
            if (gpuState == -2) lstrcpyW(gpuNote, L"No NVIDIA driver found");
        }
        if ((at = wcsstr(cmd, L"xbarstate="))) xbarState = _wtoi(at + 10);
        if (flag(cmd, L"unsent")) gpuState = xbarState = -1;
        if ((at = wcsstr(cmd, L"view="))) gpuShown = _wtoi(at + 5);
        gpuWait = flag(cmd, L"busy") ? 1 : flag(cmd, L"reading") ? 2 : 0, keepArmed = flag(cmd, L"armed");
        if (flag(cmd, L"keptfail")) gpuKept = gpuOnShow(), gpuKeptState = -1002;
        if (flag(cmd, L"naming")) naming = TRUE, lstrcpyW(nameBuf, L"Quiet, cap 2,9");
    } else {
        gpuProfiles = xbarCount = 0; // screenshots of the other tabs don't depend on this machine's blackout.ini
    }
    tabsBuild();
    int to = flag(cmd, L"fans") ? V_FANS : flag(cmd, L"wake") ? V_WAKE : flag(cmd, L"system") ? V_SYSTEM : flag(cmd, L"gpu") ? V_GPU : V_MAIN;
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
        if (naming) TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
