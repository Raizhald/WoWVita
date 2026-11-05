// layout.cpp — where the two 3DS screens go on the 960x544 display.
//
// Mode 0 (default): the top screen scaled x1.6 on the left (640x384), the touch screen 1:1 on the right (320x240);
//                   960 = 640 + 320, so both are fully visible and the touch screen is tapped directly.
// Mode 1: the top screen as large as the display allows (906x544) with the touch screen shrunk (x0.8) into the
//         bottom right corner over it.
#include "platform.h"

namespace compat {

static int g_mode = 0;

int layoutMode() { return g_mode; }
void layoutSetMode(int mode) { g_mode = mode ? 1 : 0; }

Rect layoutTop() {
    const int W = displayWidth(), H = displayHeight();
    if (g_mode == 1) {
        const int h = H, w = h * 400 / 240;
        return {(W - w) / 2, 0, w, h};
    }
    const int w = W - 320, h = w * 240 / 400;
    return {0, (H - h) / 2, w, h};
}

Rect layoutBottom() {
    const int W = displayWidth(), H = displayHeight();
    if (g_mode == 1) return {W - 256 - 8, H - 192 - 8, 256, 192};
    return {W - 320, (H - 240) / 2, 320, 240};
}

bool layoutToTouch(int dx, int dy, int* tx, int* ty) {
    const Rect r = layoutBottom();
    if (dx < r.x || dy < r.y || dx >= r.x + r.w || dy >= r.y + r.h) return false;
    *tx = (dx - r.x) * 320 / r.w;
    *ty = (dy - r.y) * 240 / r.h;
    return true;
}

}  // namespace compat
