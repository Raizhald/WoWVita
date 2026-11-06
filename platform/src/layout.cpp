// layout.cpp — where the two 3DS screens go on the 960x544 display, and what a touch on the display means.
//
// Mode 0: the top screen scaled x1.6 on the left (640x384), the touch screen 1:1 on the right (320x240);
//         960 = 640 + 320, so both are fully visible and the touch screen is tapped directly.
// Mode 1: the top screen as large as the display allows (906x544) with the touch screen shrunk (x0.8) into the
//         bottom right corner over it, tapped directly.
// Mode 2 (one screen, the Vita default): the top screen 906x544; the touch screen is a preview in the bottom right
//         corner. A tap on the preview opens it as a panel (x1.5, 480x360, finger-sized); taps inside the panel go to
//         the game, a tap outside closes it. A tap on the scene selects the unit under it (game/source/main.cpp:
//         unitAtScreen; a tap on the selected unit interacts). The tap that opens or closes the panel is not passed on.
#include "platform.h"

namespace compat {

namespace {

#if defined(__vita__)
int g_mode = 2;
#else
int g_mode = 0;   // the PC test's scripts are written for mode 0 (W3D_LAYOUT changes it)
#endif
bool g_panel = false;      // mode 2: the touch-screen panel is open
bool g_wasDown = false;
bool g_gameTouch = false;  // the current touch started inside the touch screen: it goes to the game
bool g_worldTap = false;   // a tap on the scene since the game last asked (compatWorldTap)
int g_worldX = 0, g_worldY = 0;

const int kPanelW = 480, kPanelH = 360, kThumbW = 128, kThumbH = 96, kMargin = 8;

Rect panelRect() {
    const int W = displayWidth(), H = displayHeight();
    return {W - kPanelW - kMargin, (H - kPanelH) / 2, kPanelW, kPanelH};
}

bool inside(const Rect& r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

void toTouch(const Rect& r, int dx, int dy, int* tx, int* ty) {
    int x = (dx - r.x) * 320 / r.w, y = (dy - r.y) * 240 / r.h;
    *tx = x < 0 ? 0 : x > 319 ? 319 : x;
    *ty = y < 0 ? 0 : y > 239 ? 239 : y;
}

// A tap on the scene: kept for the game in top-screen pixels (400x240), when it hit the top screen's picture.
void worldTap(int dx, int dy) {
    const Rect t = layoutTop();
    if (!inside(t, dx, dy)) return;
    g_worldX = (dx - t.x) * 400 / t.w;
    g_worldY = (dy - t.y) * 240 / t.h;
    g_worldTap = true;
}

}  // namespace

int layoutMode() { return g_mode; }
void layoutSetMode(int mode) {
    g_mode = mode < 0 || mode > 2 ? 0 : mode;
    g_panel = false;
    g_gameTouch = false;
}
void layoutNextMode() { layoutSetMode((g_mode + 1) % 3); }
bool layoutPanelOpen() { return g_mode != 2 || g_panel; }

Rect layoutTop() {
    const int W = displayWidth(), H = displayHeight();
    if (g_mode != 0) {
        const int h = H, w = h * 400 / 240;
        return {(W - w) / 2, 0, w, h};
    }
    const int w = W - 320, h = w * 240 / 400;
    return {0, (H - h) / 2, w, h};
}

// The largest top-screen rectangle of all modes: the top screen's render target is made this size, so a mode switch
// never upscales the scene.
Rect layoutTopMax() {
    const int H = displayHeight();
    return {0, 0, H * 400 / 240, H};
}

Rect layoutBottom() {
    const int W = displayWidth(), H = displayHeight();
    if (g_mode == 2) return g_panel ? panelRect() : Rect{W - kThumbW - kMargin, H - kThumbH - kMargin, kThumbW, kThumbH};
    if (g_mode == 1) return {W - 256 - 8, H - 192 - 8, 256, 192};
    return {W - 320, (H - 240) / 2, 320, 240};
}

bool layoutToTouch(int dx, int dy, int* tx, int* ty) {
    if (g_mode == 2 && !g_panel) return false;
    const Rect r = layoutBottom();
    if (!inside(r, dx, dy)) return false;
    toTouch(r, dx, dy, tx, ty);
    return true;
}

bool layoutTouch(bool down, int dx, int dy, int* tx, int* ty) {
    const bool start = down && !g_wasDown;
    g_wasDown = down;
    if (!down) {
        g_gameTouch = false;
        return false;
    }
    if (start) {
        if (g_mode == 2 && !g_panel) {   // the preview opens the panel (this tap is only for that); the scene: nothing
            const int W = displayWidth(), H = displayHeight(), pad = 12;   // (a little around the preview counts)
            const Rect thumb{W - kThumbW - kMargin - pad, H - kThumbH - kMargin - pad, kThumbW + 2 * pad, kThumbH + 2 * pad};
            if (inside(thumb, dx, dy)) g_panel = true;
            else worldTap(dx, dy);
            g_gameTouch = false;
            return false;
        }
        g_gameTouch = inside(layoutBottom(), dx, dy);
        if (!g_gameTouch && g_mode == 2) {   // outside the open panel: close it
            g_panel = false;
            return false;
        }
        if (!g_gameTouch) worldTap(dx, dy);   // modes 0 / 1: a tap on the top screen's picture
    }
    if (!g_gameTouch) return false;
    // a drag that leaves the touch screen stays on its edge, as on the 3DS
    toTouch(layoutBottom(), dx, dy, tx, ty);
    return true;
}

}  // namespace compat

// game/source/main.cpp: the last tap on the scene, once (top-screen pixels)
extern "C" int compatWorldTap(int* x, int* y) {
    if (!compat::g_worldTap) return 0;
    compat::g_worldTap = false;
    *x = compat::g_worldX;
    *y = compat::g_worldY;
    return 1;
}
