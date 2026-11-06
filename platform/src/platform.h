// platform.h — what the citro3d replacement (c3d_gl.cpp) needs from the platform: a GL context, the screen layout
// and presenting a frame. Implemented by platform_vita.cpp (vitaGL) and platform_pc.cpp (EGL, offscreen).
#pragma once
#include <stdint.h>

#include "gl_inc.h"

namespace compat {

struct Rect { int x, y, w, h; };

bool platformInitGL();
void platformShutdownGL();
// The window / display framebuffer size and object (0 on the Vita, the offscreen FBO on the PC).
int displayWidth();
int displayHeight();
GLuint displayFramebuffer();
void platformPresent();   // after the screens were composed into the display framebuffer
void recompose();         // c3d_gl.cpp: compose the last frame onto the display again (no present)

// Where the 3DS screens go on the display. The top screen render target is made at (top.w x top.h) pixels so the
// scene is drawn at the display resolution, not stretched up from 400x240.
Rect layoutTop();
Rect layoutBottom();
// Display position -> touch-screen pixel (0..319, 0..239); false outside the touch screen (or its panel is closed).
bool layoutToTouch(int dx, int dy, int* tx, int* ty);
// A finger on the display this frame (down, at dx, dy) -> a touch-screen touch for the game (true, at tx, ty).
// Opens / closes the touch-screen panel in mode 2; call once per frame, also with down = false.
bool layoutTouch(bool down, int dx, int dy, int* tx, int* ty);
int layoutMode();          // 0 = side by side, 1 = big top screen + small touch screen, 2 = one screen + touch panel
void layoutSetMode(int mode);
void layoutNextMode();
bool layoutPanelOpen();    // the touch screen is shown full size (always, except mode 2 with the panel closed)
Rect layoutTopMax();       // the largest top-screen rectangle of all modes (the render target's size)

// c3d_gl.cpp: linear / VRAM allocations (vertex and index data become GL buffers)
void gpuMemAdded(void* p, size_t size);
void gpuMemRemoved(void* p);

// console.cpp: libctru's text console on the touch screen
bool consoleShown();
void consoleHide();
const uint32_t* consolePixels();   // 320x240 RGBA8, row 0 = top; nullptr when unchanged since the last call

}  // namespace compat
