// fsr_scene.cpp — scene target + TEV upscale for w3dproto.
#include "fsr_scene.hpp"
#if defined(W3D_COMPAT)
// WoWVita: the world is drawn at the Vita's own 906x544 - no FSR upscale; the 3DS PICA TEV code is left out.
namespace fsrScene {
bool init() { return false; }
void shutdown() {}
bool ensure(int, int) { return false; }
void release() {}
bool ready() { return false; }
void vramBlocks(int, int, u32 blocks[2]) { blocks[0] = blocks[1] = 0; }
int width() { return 0; }
int height() { return 0; }
int texWidth() { return 0; }
int texHeight() { return 0; }
C3D_RenderTarget* target() { return nullptr; }
void beginPass() {}
void upscale(C3D_RenderTarget*, int, int, bool) {}
}   // namespace fsrScene
#else

#include "vram_guard.hpp"

extern "C" {
#include "fsr3ds.h"
}
#include "fsr_blit_shbin.h"

#include <string.h>

namespace fsrScene {
namespace {

struct Vert {
    float x, y;
    float u0, v0;
    float u1, v1;
    float u2, v2;
    float r, g, b, a;
};

constexpr int kMaxTex = 1024;   // PICA limit; enough for 800x240 in a 1024x256 texture

shaderProgram_s s_prog;
DVLB_s* s_dvlb = nullptr;
int s_uProj = -1;
Vert* s_v = nullptr;

C3D_Tex s_tex;
C3D_RenderTarget* s_target = nullptr;
bool s_texOk = false;
int s_texW = 0, s_texH = 0;
int s_w = 0, s_h = 0;
C3D_Tex s_dummy;   // units 1/2 after the pass: never a deleted scene texture
bool s_dummyOk = false;

int pot(int v) {
    int p = 8;
    while (p < v && p < kMaxTex) p <<= 1;
    return p;
}

void dropTarget() {
    if (s_target) {
        C3D_RenderTargetDelete(s_target);
        s_target = nullptr;
    }
    if (s_texOk) {
        C3D_TexDelete(&s_tex);
        s_texOk = false;
    }
    s_texW = s_texH = 0;
    s_w = s_h = 0;
}

}   // namespace

bool init() {
    if (s_dvlb) return true;
    s_dvlb = DVLB_ParseFile((u32*)fsr_blit_shbin, fsr_blit_shbin_size);
    if (!s_dvlb) return false;
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    s_uProj = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");
    s_v = (Vert*)linearAlloc(sizeof(Vert) * 6);
    if (s_uProj < 0 || !s_v) {
        if (s_v) { linearFree(s_v); s_v = nullptr; }
        shaderProgramFree(&s_prog);
        DVLB_Free(s_dvlb);
        s_dvlb = nullptr;
        return false;
    }
    // A tiny texture for units 1/2 after the pass: the scene texture may be
    // deleted on a mode change, and a bound (freed) texture is never safe.
    s_dummyOk = C3D_TexInit(&s_dummy, 8, 8, GPU_RGBA8);
    return true;
}

void shutdown() {
    dropTarget();
    if (s_dummyOk) { C3D_TexDelete(&s_dummy); s_dummyOk = false; }
    if (s_v) { linearFree(s_v); s_v = nullptr; }
    if (s_dvlb) {
        shaderProgramFree(&s_prog);
        DVLB_Free(s_dvlb);
        s_dvlb = nullptr;
    }
}

void release() { dropTarget(); }

bool ready() { return s_target != nullptr; }

int width() { return s_w; }
int height() { return s_h; }
int texWidth() { return s_texW; }
int texHeight() { return s_texH; }
C3D_RenderTarget* target() { return s_target; }

void vramBlocks(int w, int h, u32 blocks[2]) {
    const u32 tw = (u32)pot(w), th = (u32)pot(h);
    blocks[0] = tw * th * 4u;   // RGBA8 colour texture
    blocks[1] = tw * th * 3u;   // DEPTH24
}

bool ensure(int w, int h) {
    if (!s_v) return false;
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    const int tw = pot(w), th = pot(h);

    if (s_target && tw <= s_texW && th <= s_texH) {
        s_w = w;
        s_h = h;
        return true;
    }

    dropTarget();

    if (!vramRoomFor((u32)tw * (u32)th * 4u)) return false;
    if (!C3D_TexInitVRAM(&s_tex, (u16)tw, (u16)th, GPU_RGBA8)) return false;
    s_texOk = true;
    C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    if (!vramRoomFor((u32)tw * (u32)th * 3u)) {
        C3D_TexDelete(&s_tex);
        s_texOk = false;
        return false;
    }
    s_target = C3D_RenderTargetCreateFromTex(&s_tex, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(GPU_RB_DEPTH24));
    if (!s_target) {
        C3D_TexDelete(&s_tex);
        s_texOk = false;
        return false;
    }
    s_texW = tw;
    s_texH = th;
    s_w = w;
    s_h = h;
    return true;
}

void beginPass() {
    if (!s_target) return;
    C3D_FrameDrawOn(s_target);
    // The scene occupies the top rows of the texture (PICA's viewport Y is
    // measured from the bottom); upscale() samples v in [1-h/th, 1].
    C3D_SetViewport(0, (u32)(s_texH - s_h), (u32)s_w, (u32)s_h);
}

void upscale(C3D_RenderTarget* dst, int outW, int outH, bool sharpen) {
    if (!s_target || !s_v || !dst) return;

    const float umax = (float)s_w / (float)s_texW;
    const float vmin = 1.0f - (float)s_h / (float)s_texH;
    const float du = 1.0f / (float)s_texW;   // one source texel
    const float dv = 1.0f / (float)s_texH;

    const float xs[4] = {0.0f, (float)outW, (float)outW, 0.0f};
    const float ys[4] = {0.0f, 0.0f, (float)outH, (float)outH};
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    auto clampU = [&](float x) { return x < 0.0f ? 0.0f : (x > umax ? umax : x); };
    auto clampV = [&](float x) { return x < vmin ? vmin : (x > 1.0f ? 1.0f : x); };
    for (int i = 0; i < 6; ++i) {
        const int k = idx[i];
        const float u = xs[k] / (float)outW * umax;
        const float v = 1.0f - ys[k] / (float)outH * (1.0f - vmin);
        Vert& o = s_v[i];
        o.x = xs[k];
        o.y = ys[k];
        o.u0 = u;
        o.v0 = v;
        o.u1 = clampU(u - du);   // never sample the cleared part of the POT texture
        o.v1 = clampV(v - dv);
        o.u2 = clampU(u + du);
        o.v2 = clampV(v + dv);
        o.r = o.g = o.b = o.a = 1.0f;
    }
    GSPGPU_FlushDataCache(s_v, sizeof(Vert) * 6);

    C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0, &s_tex);
    C3D_TexBind(1, &s_tex);
    C3D_TexBind(2, &s_tex);
    fsr3dsTevKernel(sharpen ? FSR3DS_KERNEL_LITE : FSR3DS_KERNEL_REPLACE, 0);

    C3D_FrameDrawOn(dst);

    C3D_BindProgram(&s_prog);
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);   // position
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);   // uv0
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);   // uv1
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);   // uv2
    AttrInfo_AddLoader(ai, 4, GPU_FLOAT, 4);   // colour
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, s_v, sizeof(Vert), 5, 0x43210);

    C3D_Mtx proj;
    // Same projection the UI uses on the top screen: y = 0 is the top row.
    Mtx_OrthoTilt(&proj, 0.0f, (float)outW, (float)outH, 0.0f, 1.0f, -1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProj, &proj);

    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
    if (s_dummyOk) {   // the scene texture may be released on the next mode change
        C3D_TexBind(1, &s_dummy);
        C3D_TexBind(2, &s_dummy);
    }

    // What the 3D passes and uiDraw expect afterwards.
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

}   // namespace fsrScene
#endif   // W3D_COMPAT
