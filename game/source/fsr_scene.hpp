// fsr_scene.hpp — FSR-lite for w3dproto: the 3D scene is drawn into a smaller
// VRAM texture and upscaled to the top screen by the PICA TEV (libFSR3DS,
// source/fsr3ds.c). This is the FurMark "TEV 3-tap" upscale/sharpen, not AMD
// FidelityFX Super Resolution: the PICA200 has no fragment or compute shaders.
//
// The UI is drawn after the upscale at the output resolution, so text and the
// HUD stay sharp. The module owns one colour texture and one depth buffer; the
// application decides when to recreate them (mode / top-screen mode changed).
#pragma once
#include <3ds.h>
#include <citro3d.h>

namespace fsrScene {

// Loads the blit vertex shader. Call once after C3D_Init. No VRAM is taken.
bool init();
void shutdown();

// Makes sure a scene target with room for at least w x h pixels exists.
// The colour texture and the depth buffer are power-of-two PICA textures; the
// scene viewport uses only the w x h part of it. A smaller request reuses a
// larger target. Returns false when VRAM is not available.
bool ensure(int w, int h);
void release();
bool ready();

// The two VRAM blocks C3D allocates for ensure(w, h): colour texture, depth
// buffer. streamerEvictVram can use them to make room for a bigger mode.
void vramBlocks(int w, int h, u32 blocks[2]);

int width();        // requested scene width (the viewport)
int height();       // requested scene height
int texWidth();     // power-of-two texture size
int texHeight();
C3D_RenderTarget* target();

// Binds the scene target and sets the viewport to the requested w x h part.
// Call after C3D_FrameDrawOn is not needed: the function does both.
void beginPass();

// Draws the scene texture onto dst (a screen target), scaled to outW x outH.
// sharpen selects FSR3DS_KERNEL_LITE (3-tap unsharp) instead of a plain
// bilinear copy. Restores the depth/blend/cull state the renderer expects.
void upscale(C3D_RenderTarget* dst, int outW, int outH, bool sharpen);

}   // namespace fsrScene
