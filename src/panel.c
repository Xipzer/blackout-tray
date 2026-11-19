// Tray panel: a Windows 11 style flyout drawn with Direct2D/DirectWrite.
// Included by blackout.c (one translation unit, shares its state).
//
// Cost model: the panel exists only while it is open. d2d1.dll and dwrite.dll are loaded on open and freed on close.
// Drawing is done on the CPU (software DC render target) into a 32-bit bitmap that is handed to a layered window.
// A GPU target took 90 to 400 ms and 60 to 70 MB because it loads the graphics driver into the process. Measured
// with --panel-bench.
//
// Look: docked flush to the taskbar and the screen edge, square on those sides, one rounded corner facing the desktop,
// and filled with the taskbar's own colour (sampled when it opens) so it reads as part of the same surface.
//
// One view, no pages and no tabs. From the top: Screens off, the lighting card (only when lighting is set up), the
// wake rules with the log of what woke the screens (only once something has), Start with Windows, and at the foot
// the parts Blackout drives on this PC (only those that are set up), the version and Exit. A PC with nothing set up
// gets the button, the wake rules and Start with Windows, and no row that says what is missing.
//
// Red (ACCENT) means "on". A row has one line, and its title says what the control does without help. No hint lines.
#define COBJMACROS
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#define PSAPI_VERSION 2
#include <psapi.h>

#define PANEL_CLASS L"BlackoutPanel"
#define VERSION     L"2.0.0"
#define PANEL_W     352.0f
#define PAD         16.0f
#define INSET       14.0f
#define GAP         12.0f
#define ROW         40.0f // one line of text and one control
#define LABEL       24.0f // the heading of the wake log
#define LOG         24.0f // one wake in the log
#define LINE        22.0f // one line of parts
#define RADIUS      8
#define ACCENT      0xE5484D
#define WARN        0xF0A35B
#define GOOD        0x3FB950
#define WM_RENDER   (WM_APP + 3)

enum { H_NONE, H_SCREENS, H_RGB, H_SYNC, H_HOTKEY, H_WAKE_CLICK, H_WAKE_ESC, H_START, H_EXIT, H_COUNT };
enum { F_BRAND, F_ROW, F_SMALL, F_BOLD, F_TINY, F_COUNT };

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
static int hover;
// startOn is read once when the panel opens. panelTest: a developer mode, where no switch reaches the PC.
static BOOL dockTop, panelLight, panelTest, panelHidden, dirty, startOn;
static DWORD panelClosedAt;
static UINT panelTint; // 0xRRGGBB taken from the taskbar, 0 = none

// ---- Drawing primitives ----

static D2D1_COLOR_F rgba(UINT rgb, float a) {
    D2D1_COLOR_F c = {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a};
    return c;
}
static UINT ink(void) { return panelLight ? 0x1B1B1F : 0xF3F3F3; }
static UINT dim(void) { return panelLight ? 0x5F5F66 : 0xA8A8AD; }
static UINT tone(void) { return panelLight ? 0x000000 : 0xFFFFFF; }
static UINT warn(void) { return panelLight ? 0xB45309 : WARN; }
static UINT good(void) { return panelLight ? 0x1A7F37 : GOOD; }
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
// Every format is one line and ends in an ellipsis when the text does not fit its rectangle (panelLoad).
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

// ---- Icons ----

// Crescent as a real shape (disc minus disc), so it sits cleanly on the background.
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
// The power symbol: an open ring, clockwise the long way round from one side of the gap to the other, and a stroke.
static void powerIcon(float cx, float cy, UINT rgb) {
    ID2D1PathGeometry *g;
    ID2D1GeometrySink *sink;
    if (ID2D1Factory_CreatePathGeometry(d2f, &g) != S_OK) return;
    if (ID2D1PathGeometry_Open(g, &sink) == S_OK) {
        D2D1_ARC_SEGMENT seg = {{cx - 3.3f, cy - 4.4f}, {5.5f, 5.5f}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE};
        ID2D1GeometrySink_BeginFigure(sink, pt(cx + 3.3f, cy - 4.4f), D2D1_FIGURE_BEGIN_HOLLOW);
        ID2D1GeometrySink_AddArc(sink, &seg);
        ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_OPEN);
        ID2D1GeometrySink_Close(sink);
        ID2D1GeometrySink_Release(sink);
        paint(rgb, 1);
        ID2D1RenderTarget_DrawGeometry(rt, (ID2D1Geometry *)g, (ID2D1Brush *)brush, 2, NULL);
    }
    ID2D1PathGeometry_Release(g);
    line(cx, cy - 7.5f, cx, cy - 1, rgb, 1, 2);
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

static void card(float x, float y, float w, float h) {
    fillRound(rc(x, y, w, h), 8, tone(), 0.045f);
    strokeRound(rc(x, y, w, h), 8, tone(), 0.07f, 1);
}
static void divider(float x, float y, float w) { line(x, y, x + w, y, tone(), 0.07f, 1); }
// A title on the left, a switch on the right. The whole row is the click target. The title has to say what the switch
// does on its own: rows have one line.
static void toggleRow(LPCWSTR label, float x, float y, float w, int id, BOOL on) {
    D2D1_RECT_F r = rc(x + w - INSET - 40, y + 10, 40, 20);
    hit[id] = rc(x, y, w, ROW);
    if (hover == id) fillRound(rc(x + 3, y + 3, w - 6, ROW - 6), 6, tone(), 0.05f);
    text(label, F_ROW, rc(x + INSET, y + 10, w - 80, 22), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (on) {
        fillRound(r, 10, ACCENT, hover == id ? 0.85f : 1);
        dot(r.left + 30, y + 20, 6, 0xFFFFFF, 1);
    } else {
        fillRound(r, 10, tone(), hover == id ? 0.10f : 0.04f);
        strokeRound(r, 10, dim(), 0.9f, 1);
        dot(r.left + 10, y + 20, 5, dim(), 1);
    }
}
// One of the inputs that may wake the screens, as a pill with its picture, 28 px high. Chosen is a red outline and a
// red tint, not solid red: that is kept for Screens off, the one action here. While the rule above it is off the pill
// is grey and not a target, but still shows the choice. Returns its width.
static float pill(int id, LPCWSTR label, float x, float y, BOOL on, BOOL live) {
    float tw = measure(label, F_SMALL), w = 36 + tw + 14, a = live ? 1 : 0.45f;
    D2D1_RECT_F r = rc(x, y, w, 28), edge = rc(x + 0.5f, y + 0.5f, w - 1, 27);
    UINT c = on ? ink() : dim();
    if (live) hit[id] = r;
    if (on && live) {
        fillRound(r, 14, ACCENT, hover == id ? 0.30f : 0.20f);
        strokeRound(edge, 13.5f, ACCENT, 1, 1);
    } else {
        fillRound(r, 14, tone(), on || hover == id ? 0.10f : 0.04f);
        if (!on) strokeRound(edge, 13.5f, dim(), 0.9f * a, 1);
    }
    if (id == H_WAKE_CLICK) mouseIcon(x + 19, y + 14, c, a);
    else keyboardIcon(x + 20, y + 14, c, a);
    text(label, F_SMALL, rc(x + 36, y + 6, tw + 2, 16), c, a, DWRITE_TEXT_ALIGNMENT_LEADING);
    return w;
}

// ---- Layout ----

// What Blackout drives on this PC. Only parts that are set up are listed, so with nothing set up nothing is shown.
// A part has one way to fail, and then its value says which.
typedef struct { LPCWSTR label, value; BOOL bad; } Part;
static int partsList(Part *p) {
    static WCHAR leds[16];
    int n = 0;
    if (openrgb[0]) p[n++] = (Part){L"OpenRGB", hasOpenRgb ? L"" : L"not found at the path in blackout.ini", !hasOpenRgb};
    if (linkLeds) {
        wsprintfW(leds, L"%d LEDs", linkLeds);
        p[n++] = (Part){L"iCUE LINK", linkOk ? leds : L"hub not answering", !linkOk};
    }
    if (hasRam) p[n++] = (Part){L"FURY RAM", L"", FALSE};
    if (ddcIds[0]) p[n++] = (Part){L"Monitor power", ddcIds, FALSE};
    return n;
}
// The parts flow like words and wrap: a dot, the name, and what the dot cannot say. Returns the height used, and
// only measures when draw is not set.
static float partsFlow(float x, float y, float w, BOOL draw) {
    Part p[4];
    int n = partsList(p);
    float cx = x, cy = y;
    for (int i = 0; i < n; i++) {
        float lw = measure(p[i].label, F_SMALL), vw = p[i].value[0] ? measure(p[i].value, F_SMALL) + 5 : 0;
        if (cx > x && cx + 12 + lw + vw > x + w) cx = x, cy += LINE;
        if (12 + lw + vw > w) vw = w - 12 - lw; // a part longer than a line ends in an ellipsis
        if (draw) {
            dot(cx + 4, cy + 11, 3.5f, p[i].bad ? warn() : good(), 1);
            text(p[i].label, F_SMALL, rc(cx + 12, cy + 3, lw + 2, 16), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
            if (vw) text(p[i].value, F_SMALL, rc(cx + 17 + lw, cy + 3, vw - 3, 16), p[i].bad ? warn() : dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        cx += 12 + lw + vw + 16;
    }
    return n ? cy - y + LINE : 0;
}
static float logHeight(void) { return wakeCount ? LABEL + wakeCount * LOG + 6 : 0; }
static float partsHeight(void) {
    float h = partsFlow(0, 0, PANEL_W - 2 * PAD - 4, FALSE);
    return h ? h + 4 : 0;
}
static float panelHeight(void) {
    return PAD + 32 + GAP + (hasRgb ? 2 * ROW + GAP : 0) + 2 * ROW + logHeight() + GAP + ROW + 10 + partsHeight() + 28 + PAD - 4;
}

// ---- The view ----

static void paintBackground(void) {
    D2D1_COLOR_F clear = {0, 0, 0, 0};
    ID2D1PathGeometry *g;
    ID2D1GeometrySink *sink;
    float r = RADIUS, W = PANEL_W, H = panelH;
    ID2D1RenderTarget_Clear(rt, &clear);
    // One opaque shape, square where it meets the taskbar and the screen edge, rounded at the corner that faces the
    // desktop. The window is layered, so the curve is anti-aliased against whatever is behind it.
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
}

// The log of what woke the screens. A row is a picture of one wake: the time, what kind of input (mouse, keyboard,
// neither), the device, and "not blocked" set apart on the right when it got past the wake rules, so that is never
// the part that gets cut off.
static void paintLog(float x, float y, float w) {
    WCHAR at[8];
    float tw = measure(L"not blocked", F_TINY) + 8;
    text(L"WHAT WOKE THE SCREENS", F_TINY, rc(x, y + 9, w, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    y += LABEL;
    for (int i = 0; i < wakeCount; i++, y += LOG) {
        float cy = y + LOG / 2;
        wsprintfW(at, L"%02d:%02d", wakes[i].at.wHour, wakes[i].at.wMinute);
        text(at, F_BOLD, rc(x, cy - 9, 40, 18), ink(), i ? 0.7f : 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (wakes[i].icon == L'M') mouseIcon(x + 52, cy, i ? dim() : ink(), 1);
        else if (wakes[i].icon == L'K') keyboardIcon(x + 52, cy, i ? dim() : ink(), 1);
        else ring(x + 52, cy, 4, dim(), 0.7f, 1.3f);
        if (wakes[i].slip) text(L"not blocked", F_TINY, rc(x + w - tw, cy - 7, tw, 14), warn(), 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
        text(wakes[i].name, F_SMALL, rc(x + 68, cy - 8, w - 68 - (wakes[i].slip ? tw : 0), 18), i ? dim() : ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

static void paintPanel(void) {
    float x = PAD, w = PANEL_W - 2 * PAD, y = PAD, ix = x + INSET, iw = w - 2 * INSET;
    paintBackground();
    memset(hit, 0, sizeof(hit));

    moon(x + 9, y + 16, 9);
    text(L"Blackout", F_BRAND, rc(x + 26, y + 5, 150, 22), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    hit[H_SCREENS] = rc(x + w - 128, y, 128, 32);
    fillRound(hit[H_SCREENS], 16, ACCENT, hover == H_SCREENS ? 0.85f : 1);
    powerIcon(x + w - 110, y + 17, 0xFFFFFF);
    text(L"Screens off", F_BOLD, rc(x + w - 98, y + 7, 92, 18), 0xFFFFFF, 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    y += 32 + GAP;

    if (hasRgb) {
        card(x, y, w, 2 * ROW);
        toggleRow(L"RGB lighting", x, y, w, H_RGB, litWant);
        divider(x, y + ROW, w);
        toggleRow(L"RGB off with the screens", x, y + ROW, w, H_SYNC, syncOn);
        y += 2 * ROW + GAP;
    }

    // The title starts a sentence that the row under it finishes, so no line divides them: wake the screens only with
    // a left-click, with Esc, and (always, so it is words and not a pill) with Ctrl+Alt+Del.
    float lh = logHeight(), aw = measure(L"always", F_SMALL), cw = measure(L"Ctrl+Alt+Del", F_SMALL), a = hotkeyOn ? 1 : 0.45f;
    card(x, y, w, 2 * ROW + lh);
    toggleRow(L"Wake the screens only with", x, y, w, H_HOTKEY, hotkeyOn);
    float px = ix + pill(H_WAKE_CLICK, L"Left-click", ix, y + ROW + 2, wakeClick, hotkeyOn) + 8;
    pill(H_WAKE_ESC, L"Esc", px, y + ROW + 2, wakeEsc, hotkeyOn);
    text(L"Ctrl+Alt+Del", F_SMALL, rc(ix + iw - aw - 5 - cw, y + ROW + 8, cw + 2, 16), ink(), a, DWRITE_TEXT_ALIGNMENT_LEADING);
    text(L"always", F_SMALL, rc(ix + iw - aw, y + ROW + 8, aw + 2, 16), dim(), a, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (lh) {
        divider(x, y + 2 * ROW, w);
        paintLog(ix, y + 2 * ROW, iw);
    }
    y += 2 * ROW + lh + GAP;

    card(x, y, w, ROW);
    toggleRow(L"Start with Windows", x, y, w, H_START, startOn);
    y += ROW + 10;

    // The foot of the panel is about Blackout itself: what it drives on this PC, its version, and the way out.
    partsFlow(x + 2, y, w - 4, TRUE);
    y += partsHeight();
    text(L"Blackout " VERSION, F_TINY, rc(x + 2, y + 7, 160, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    hit[H_EXIT] = rc(x + w - 56, y, 56, 28);
    fillRound(hit[H_EXIT], 6, tone(), hover == H_EXIT ? 0.12f : 0.05f);
    text(L"Exit", F_SMALL, rc(x + w - 56, y + 6, 56, 16), hover == H_EXIT ? ink() : dim(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
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

// The bitmap the panel is drawn into, remade when the panel changes height.
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

// Draws the panel on the CPU and, when there is a window, hands the bitmap to it. pos moves the window too.
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
static void panelUpdate(void) { panelDirty(); }

// ---- Interaction ----

static int hitTest(LPARAM lp) {
    float mx = (short)LOWORD(lp) / panelScale, my = (short)HIWORD(lp) / panelScale;
    for (int i = 1; i < H_COUNT; i++)
        if (inside(hit[i], mx, my)) return i;
    return H_NONE;
}

// A switch is cached in memory (blackout.c) and written through to HKCU, except in the developer modes.
static void flip(BOOL *value, LPCWSTR name) {
    *value = !*value;
    if (!panelTest) setDword(name, *value);
}

static void panelClick(int id) {
    switch (id) {
    case H_NONE: return;
    case H_SCREENS:
        DestroyWindow(panel);
        if (!panelTest) SetTimer(panelOwner, TIMER_OFF, 400, NULL);
        return;
    case H_EXIT:
        DestroyWindow(panel);
        if (!panelTest) DestroyWindow(panelOwner);
        return;
    case H_RGB:
        if (panelTest) litWant = !litWant;
        else rgbToggle(panelOwner);
        break;
    case H_SYNC: flip(&syncOn, L"Sync"); break;
    case H_HOTKEY: flip(&hotkeyOn, L"HotkeyMode"); break;
    // Keep at least one wake input, so the screens can always be woken.
    case H_WAKE_CLICK: if (!wakeClick || wakeEsc) flip(&wakeClick, L"WakeClick"); break;
    case H_WAKE_ESC: if (!wakeEsc || wakeClick) flip(&wakeEsc, L"WakeEsc"); break;
    case H_START:
        if (!panelTest) toggleStartup();
        startOn = panelTest ? !startOn : startupEnabled();
        break;
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
    case WM_RENDER: {
        // The height follows what there is to show (a wake was logged, the hub stopped answering): the edge that
        // touches the taskbar stays where it is.
        float h = panelHeight();
        RECT r;
        dirty = FALSE;
        if (h == panelH || !GetWindowRect(hwnd, &r)) {
            panelRender(NULL);
            return 0;
        }
        panelH = h;
        POINT pos = {r.left, dockTop ? r.top : r.bottom - (int)(panelH * panelScale + 0.5f)};
        panelRender(&pos);
        return 0;
    }
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
    case WM_KEYDOWN: if (wp == VK_ESCAPE) DestroyWindow(hwnd); return 0;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE) DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        panelFree();
        panel = NULL;
        hover = H_NONE, dirty = FALSE;
        panelClosedAt = GetTickCount();
        if (panelTest && !panelHidden) PostQuitMessage(0);
        trim();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

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
    if (!panelTest) {
        DWORD light = 0, n = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &light, &n);
        panelLight = light != 0;
        startOn = startupEnabled();
    }
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
    HWND tray = shell, next = NULL;
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
                for (int c = 0; c < 3; c++) hist[c][(row[i] >> (16 - 8 * c)) & 255]++; // c = 0 is red
            for (int c = 0; c < 3; c++) {
                int seen = 0, med = 0;
                while (med < 255 && (seen += hist[c][med]) * 2 < w) med++;
                panelTint = panelTint << 8 | (UINT)med;
            }
        }
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
    }
    panelScale = dpiX / 96.0f;
    panelH = panelHeight();
    // Small or heavily scaled screens: shrink the whole panel so it still fits.
    if (panelH * panelScale > wa.bottom - wa.top) panelScale = (wa.bottom - wa.top) / panelH;
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

// ---- Developer modes ----
// No hardware access, no single-instance lock, blackout.ini and the registry are neither read nor written: what is
// set up comes from the flags, so a picture never depends on the PC that drew it. With no flags: nothing set up.
//   --panel-test  [flags]              open the panel with preview data; exits on close
//   --panel-shot  [flags] out=<file>   draw the panel off-screen to a .bmp, no window at all (README images, reviews)
//   --panel-bench [flags]              open and close five times, never shown; log time and memory to %TEMP%
// Set up:  openrgb badpath link nohub ram ddc longddc all (= openrgb link ram ddc)
// State:   rgboff nosync hotkey noclick noesc nostart wakes=<1..5> longwakes
// Picture: light top x2 scale=<percent> hover=<id>

static BOOL flag(LPCWSTR cmd, LPCWSTR name) {
    WCHAR key[32] = L" ";
    lstrcatW(key, name);
    LPCWSTR at = wcsstr(cmd, key);
    return at && (at[lstrlenW(key)] == L' ' || !at[lstrlenW(key)]);
}

static BOOL panelShot(LPCWSTR path) {
    DWORD wrote;
    if (!panelLoad()) return FALSE;
    panelH = panelHeight();
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
                      GetModuleHandleW(L"d3d10warp.dll") ? " warp" : "", GetModuleHandleW(L"nvwgf2umx.dll") || GetModuleHandleW(L"nvldumdx.dll") ? " graphics-driver" : "");
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
    // The everyday wakes, and the longest wakeRecord() can write: 40 character product names, with and without
    // "not blocked".
    static const struct { WCHAR icon; LPCWSTR name; BOOL slip; } usual[MAX_WAKES] = {
        {L'M', L"Left-click", 0}, {L'M', L"PRO X SUPERLIGHT 2c", 0}, {0, L"No input: Windows or an app", 0}, {L'K', L"Esc", 0}, {L'K', L"BlackWidow V4 75%", 1}},
    longest[MAX_WAKES] = {{L'K', L"Razer BlackWidow V4 Pro 75% Wireless Keyb", 1}, {L'M', L"Logitech G502 X PLUS LIGHTSPEED Wireless", 1},
                          {0, L"Input device: USB Composite Device Receiver 2.4G Wireless", 0}, {L'K', L"Esc", 0}, {0, L"No input: Windows or an app", 0}};
    LPCWSTR out = wcsstr(cmd, L"out="), at;
    BOOL all = flag(cmd, L"all"), longWakes = flag(cmd, L"longwakes");
    MSG m;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    panelTest = TRUE;
    if (flag(cmd, L"badpath") || (hasOpenRgb = all || flag(cmd, L"openrgb"))) lstrcpyW(openrgb, L"OpenRGB.exe");
    linkOk = all || flag(cmd, L"link");
    if (linkOk || flag(cmd, L"nohub")) linkLeds = 22;
    hasRam = all || flag(cmd, L"ram");
    if (all || flag(cmd, L"ddc")) lstrcpyW(ddcIds, L"BNQ7F2F");
    if (flag(cmd, L"longddc")) lstrcpyW(ddcIds, L"BNQ7F2F,DELA0EC,GSM5B7F,AUS27B1,SAM7106,ACR0B0C");
    hasRgb = hasOpenRgb || hasRam || linkLeds;
    litWant = !flag(cmd, L"rgboff"), syncOn = !flag(cmd, L"nosync"), startOn = !flag(cmd, L"nostart");
    hotkeyOn = flag(cmd, L"hotkey"), wakeClick = !flag(cmd, L"noclick"), wakeEsc = !flag(cmd, L"noesc");
    wakeCount = longWakes ? MAX_WAKES : (at = wcsstr(cmd, L"wakes=")) ? _wtoi(at + 6) : 0;
    if (wakeCount > MAX_WAKES) wakeCount = MAX_WAKES;
    for (int i = 0; i < wakeCount; i++) {
        GetLocalTime(&wakes[i].at);
        wakes[i].at.wHour = (WORD)(longWakes ? 23 - i * 5 : 9 - i * 2), wakes[i].at.wMinute = (WORD)(longWakes ? 48 - i * 9 : 13 + i * 11);
        wakes[i].icon = (longWakes ? longest : usual)[i].icon, wakes[i].slip = (longWakes ? longest : usual)[i].slip;
        lstrcpyW(wakes[i].name, (longWakes ? longest : usual)[i].name);
    }
    panelLight = flag(cmd, L"light"), dockTop = flag(cmd, L"top");
    if ((at = wcsstr(cmd, L"hover="))) hover = _wtoi(at + 6);
    if (wcsstr(cmd, L"--panel-bench")) {
        panelBench();
        return 0;
    }
    if (wcsstr(cmd, L"--panel-shot")) {
        panelScale = flag(cmd, L"x2") ? 2 : (at = wcsstr(cmd, L"scale=")) ? _wtoi(at + 6) / 100.0f : 1;
        return out && panelShot(out + 4) ? 0 : 1;
    }
    if (!panelShow(NULL)) return 1;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}
