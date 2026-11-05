// c3d_gl.cpp — citro3d re-implemented on OpenGL ES 2 (vitaGL on the PS Vita, Mesa on the PC test build).
//
// The game keeps talking citro3d; this file turns the PICA200 state it sets into GL state:
//   * float uniforms: the 96-register file C3D_FVUnif (citro3d's own globals, written by the inline helpers in
//     uniforms.h) is uploaded as "uniform vec4 c[N]" to the vertex shaders made by tools/pica2glsl.py;
//   * the six TEV combiner stages become a generated fragment shader (cached per configuration);
//   * textures stay in PICA format in their CPU-side buffer (tex->data, which the game fills directly, with
//     C3D_TexLoadImage or with copies) and are converted + uploaded to GL lazily when bound after a change;
//   * render targets are framebuffer objects; at C3D_FrameEnd the two screens are composed onto the display.
// Coordinate conventions: projection matrices are made without the 3DS screen tilt (see maths/tilt.c); PICA samples
// t=1 at the top of the image, GL at t=0, so the fragment shader flips t except for render-target textures.
#include <citro3d.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gl_inc.h"
#include "platform.h"

// ------------------------------------------------------------------ citro3d's uniform globals
C3D_FVec C3D_FVUnif[2][C3D_FVUNIF_COUNT];
C3D_IVec C3D_IVUnif[2][C3D_IVUNIF_COUNT];
u16 C3D_BoolUnifs[2];
bool C3D_FVUnifDirty[2][C3D_FVUNIF_COUNT];
bool C3D_IVUnifDirty[2][C3D_IVUNIF_COUNT];
bool C3D_BoolUnifsDirty[2];

#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif
#ifndef GL_MIN_EXT
#define GL_MIN_EXT 0x8007
#define GL_MAX_EXT 0x8008
#endif

namespace {

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// ================================================================== textures
struct TexRec {
    GLuint gl = 0;
    bool dirty = true;
    bool target = false;      // colour buffer of a render target: GL owns the pixels, never uploaded from data
    u16 width = 0, height = 0;
    GPU_TEXCOLOR fmt = GPU_RGBA8;
    int maxLevel = 0;
    u32 total = 0;            // bytes of data incl. mip levels
    u32 appliedParam = 0xFFFFFFFF;
};

std::mutex g_texLock;                         // the streamer's threads flush their buffers too
std::map<uintptr_t, TexRec> g_texByData;      // keyed by the start of tex->data
std::vector<GLuint> g_glTexToDelete;          // GL work happens on the render thread only

TexRec* findTexContaining(const void* p) {
    uintptr_t a = (uintptr_t)p;
    auto it = g_texByData.upper_bound(a);
    if (it == g_texByData.begin()) return nullptr;
    --it;
    if (a >= it->first && a < it->first + std::max<u32>(it->second.total, 1)) return &it->second;
    return nullptr;
}

void markDirtyRange(const void* p, u32 size) {
    if (!p || !size) return;
    std::lock_guard<std::mutex> g(g_texLock);
    uintptr_t a = (uintptr_t)p, e = a + size;
    auto it = g_texByData.upper_bound(a);
    if (it != g_texByData.begin()) --it;
    for (; it != g_texByData.end() && it->first < e; ++it)
        if (it->first + it->second.total > a) it->second.dirty = true;
}

int fmtBits(GPU_TEXCOLOR f) {
    switch (f) {
        case GPU_RGBA8: return 32;
        case GPU_RGB8: return 24;
        case GPU_RGBA5551: case GPU_RGB565: case GPU_RGBA4: case GPU_LA8: case GPU_HILO8: return 16;
        case GPU_L8: case GPU_A8: case GPU_LA4: case GPU_ETC1A4: return 8;
        case GPU_L4: case GPU_A4: case GPU_ETC1: return 4;
        default: return 32;
    }
}

inline u32 morton8(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

// PICA 8x8 tiles (Morton inside) -> rows top to bottom, bpp bytes per texel.
void detile(const u8* src, u8* dst, int w, int h, int bpp) {
    const int tilesX = w / 8;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const u32 si = ((u32)((y >> 3) * tilesX + (x >> 3)) * 64 + morton8(x & 7, y & 7)) * bpp;
            memcpy(dst + ((size_t)y * w + x) * bpp, src + si, bpp);
        }
}

// 4-bit formats: two texels a byte, low nibble first, in tile/Morton order.
void detile4(const u8* src, u8* dst, int w, int h) {
    const int tilesX = w / 8;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const u32 i = (u32)((y >> 3) * tilesX + (x >> 3)) * 64 + morton8(x & 7, y & 7);
            const u8 b = src[i >> 1];
            const u8 n = (i & 1) ? (b >> 4) : (b & 0xF);
            dst[(size_t)y * w + x] = (u8)(n * 17);
        }
}

const int kEtcMod[8][4] = {{2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
                           {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}};
inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// One standard ETC1 block (big-endian value v) -> 4x4 RGB, out[y*4+x].
void etc1DecodeBlock(u64 v, u8 out[16][3]) {
    const bool diff = (v >> 33) & 1, flip = (v >> 32) & 1;
    int c[2][3];
    if (diff) {
        const int r = (int)((v >> 59) & 31), g = (int)((v >> 51) & 31), b = (int)((v >> 43) & 31);
        auto sx = [](int d) { return d >= 4 ? d - 8 : d; };
        const int dr = sx((int)((v >> 56) & 7)), dg = sx((int)((v >> 48) & 7)), db = sx((int)((v >> 40) & 7));
        const int base[3] = {r, g, b}, delta[3] = {dr, dg, db};
        for (int k = 0; k < 3; ++k) {
            const int a = base[k], s = (a + delta[k]) & 31;
            c[0][k] = (a << 3) | (a >> 2);
            c[1][k] = (s << 3) | (s >> 2);
        }
    } else {
        const int sh[3] = {60, 52, 44};
        for (int k = 0; k < 3; ++k) {
            const int a = (int)((v >> sh[k]) & 15), b = (int)((v >> (sh[k] - 4)) & 15);
            c[0][k] = a * 17;
            c[1][k] = b * 17;
        }
    }
    const int cw[2] = {(int)((v >> 37) & 7), (int)((v >> 34) & 7)};
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) {
            const int i = x * 4 + y;
            const int idx = (int)(((v >> (i + 16)) & 1) << 1 | ((v >> i) & 1));
            const int sub = flip ? (y >= 2) : (x >= 2);
            const int m = kEtcMod[cw[sub]][idx];
            for (int k = 0; k < 3; ++k) out[y * 4 + x][k] = (u8)clamp255(c[sub][k] + m);
        }
}

// PICA ETC1 / ETC1A4 level -> standard ETC1 blocks in raster order (etcOut) or RGBA8 rows (rgbaOut).
void etcLevel(const u8* src, int w, int h, bool alpha, u8* etcOut, u8* rgbaOut) {
    const int bw = std::max(1, w / 4), bh = std::max(1, h / 4), tilesX = std::max(1, w / 8);
    const int blk = alpha ? 16 : 8;
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const int tile = (by >> 1) * tilesX + (bx >> 1), sub = (by & 1) * 2 + (bx & 1);
            const u8* p = src + ((size_t)tile * 4 + sub) * blk;
            const u8* a = alpha ? p : nullptr;
            const u8* e = alpha ? p + 8 : p;
            u64 v = 0;
            for (int i = 7; i >= 0; --i) v = (v << 8) | e[i];   // little-endian in memory = the big-endian ETC1 value
            if (etcOut) {
                u8* o = etcOut + ((size_t)by * bw + bx) * 8;
                for (int i = 0; i < 8; ++i) o[i] = (u8)(v >> (56 - 8 * i));
            }
            if (rgbaOut) {
                u8 px[16][3];
                etc1DecodeBlock(v, px);
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) {
                        const int X = bx * 4 + x, Y = by * 4 + y;
                        if (X >= w || Y >= h) continue;
                        u8* o = rgbaOut + ((size_t)Y * w + X) * 4;
                        o[0] = px[y * 4 + x][0];
                        o[1] = px[y * 4 + x][1];
                        o[2] = px[y * 4 + x][2];
                        if (a) {
                            const u8 byte = a[2 * x + y / 2];
                            o[3] = (u8)(((y & 1) ? (byte >> 4) : (byte & 15)) * 17);
                        } else {
                            o[3] = 255;
                        }
                    }
            }
        }
}

GLenum glWrap(u32 w) {
    switch (w & 3) {
        case GPU_CLAMP_TO_EDGE: return GL_CLAMP_TO_EDGE;
        case GPU_MIRRORED_REPEAT: return GL_MIRRORED_REPEAT;
        case GPU_REPEAT: return GL_REPEAT;
        default: return GL_CLAMP_TO_EDGE;   // CLAMP_TO_BORDER: not in GLES2
    }
}

void applyParams(TexRec& r, u32 param) {
    if (r.appliedParam == param) return;
    r.appliedParam = param;
    const bool mag = (param >> 1) & 1, min = (param >> 2) & 1, mip = (param >> 24) & 1;
    GLenum minf = min ? GL_LINEAR : GL_NEAREST;
    if (r.maxLevel > 0 && !r.target)
        minf = min ? (mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : (mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrap(param >> 12));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrap(param >> 8));
}

std::vector<u8> g_conv, g_conv2;

// Converts every level of the PICA data into GL. Called with the texture bound.
void uploadTex(TexRec& r, const u8* data) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const bool etc = r.fmt == GPU_ETC1 || r.fmt == GPU_ETC1A4;
    const bool compressed = r.fmt == GPU_ETC1;
    int levels = r.maxLevel + 1;
    if (!compressed) levels = 1;   // the GPU makes the smaller levels (vitaGL ignores level data of plain formats)
    u32 off = 0;
    for (int l = 0; l < levels; ++l) {
        const int w = std::max(8, r.width >> l), h = std::max(8, r.height >> l);
        const u32 levelSize = (u32)w * h * fmtBits(r.fmt) / 8;
        const u8* src = data + off;
        off += levelSize;
        if (compressed) {
            g_conv.resize((size_t)w * h / 2);
            etcLevel(src, w, h, false, g_conv.data(), nullptr);
            glCompressedTexImage2D(GL_TEXTURE_2D, l, GL_ETC1_RGB8_OES, w, h, 0, (GLsizei)g_conv.size(), g_conv.data());
            continue;
        }
        if (etc) {   // ETC1A4: no GL equivalent, decoded
            g_conv.resize((size_t)w * h * 4);
            etcLevel(src, w, h, true, nullptr, g_conv.data());
            glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_conv.data());
            continue;
        }
        switch (r.fmt) {
            case GPU_RGBA8: {
                g_conv.resize((size_t)w * h * 4);
                detile(src, g_conv.data(), w, h, 4);
                for (size_t i = 0; i < g_conv.size(); i += 4) {   // PICA bytes A B G R -> R G B A
                    std::swap(g_conv[i], g_conv[i + 3]);
                    std::swap(g_conv[i + 1], g_conv[i + 2]);
                }
                glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_conv.data());
                break;
            }
            case GPU_RGB8: {
                g_conv.resize((size_t)w * h * 3);
                detile(src, g_conv.data(), w, h, 3);
                for (size_t i = 0; i < g_conv.size(); i += 3) std::swap(g_conv[i], g_conv[i + 2]);
                glTexImage2D(GL_TEXTURE_2D, l, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, g_conv.data());
                break;
            }
            case GPU_RGBA5551: case GPU_RGB565: case GPU_RGBA4: {
                g_conv.resize((size_t)w * h * 2);
                detile(src, g_conv.data(), w, h, 2);
                const GLenum type = r.fmt == GPU_RGBA5551 ? GL_UNSIGNED_SHORT_5_5_5_1 : r.fmt == GPU_RGB565 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_SHORT_4_4_4_4;
                const GLenum f = r.fmt == GPU_RGB565 ? GL_RGB : GL_RGBA;
                glTexImage2D(GL_TEXTURE_2D, l, f, w, h, 0, f, type, g_conv.data());
                break;
            }
            case GPU_LA8: case GPU_HILO8: {
                g_conv.resize((size_t)w * h * 2);
                detile(src, g_conv.data(), w, h, 2);
                for (size_t i = 0; i < g_conv.size(); i += 2) std::swap(g_conv[i], g_conv[i + 1]);   // A L -> L A
                glTexImage2D(GL_TEXTURE_2D, l, GL_LUMINANCE_ALPHA, w, h, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, g_conv.data());
                break;
            }
            case GPU_L8: case GPU_A8: {
                g_conv.resize((size_t)w * h);
                detile(src, g_conv.data(), w, h, 1);
                const GLenum f = r.fmt == GPU_A8 ? GL_ALPHA : GL_LUMINANCE;
                glTexImage2D(GL_TEXTURE_2D, l, f, w, h, 0, f, GL_UNSIGNED_BYTE, g_conv.data());
                break;
            }
            case GPU_LA4: {
                g_conv.resize((size_t)w * h);
                detile(src, g_conv.data(), w, h, 1);
                g_conv2.resize((size_t)w * h * 2);
                for (size_t i = 0; i < g_conv.size(); ++i) {
                    g_conv2[2 * i] = (u8)((g_conv[i] >> 4) * 17);
                    g_conv2[2 * i + 1] = (u8)((g_conv[i] & 15) * 17);
                }
                glTexImage2D(GL_TEXTURE_2D, l, GL_LUMINANCE_ALPHA, w, h, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, g_conv2.data());
                break;
            }
            case GPU_L4: case GPU_A4: {
                g_conv.resize((size_t)w * h);
                detile4(src, g_conv.data(), w, h);
                const GLenum f = r.fmt == GPU_A4 ? GL_ALPHA : GL_LUMINANCE;
                glTexImage2D(GL_TEXTURE_2D, l, f, w, h, 0, f, GL_UNSIGNED_BYTE, g_conv.data());
                break;
            }
            default: break;
        }
    }
    if (compressed) {
#if !defined(__vita__)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, r.maxLevel);
#endif
    } else if (r.maxLevel > 0) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    r.appliedParam = 0xFFFFFFFF;
}

// ================================================================== render targets
struct Target {
    C3D_RenderTarget t;       // what the game sees (must be first: C3D_FrameBuf* -> Target*)
    GLuint fbo = 0, color = 0, depth = 0;
    int w = 1, h = 1;         // GL size
    int logicalW = 1, logicalH = 1;
    bool screen = false;      // made by C3D_RenderTargetCreate: a 3DS screen (tilted), else a texture
    void* texData = nullptr;  // the C3D_Tex data it renders into
    bool drawn = false;
};

std::vector<Target*> g_targets;
Target* g_outputs[2][2] = {};   // [screen][side]

Target* targetOf(C3D_FrameBuf* fb) {
    for (Target* t : g_targets)
        if (&t->t.frameBuf == fb) return t;
    return nullptr;
}

// ================================================================== shaders
struct VsInfo { DVLE_s* dvle; GLuint shader = 0; bool failed = false; };
std::unordered_map<u32, VsInfo> g_vs;

struct Program {
    GLuint prog = 0;
    bool failed = false;
    GLint uC = -1, uConst = -1, uBuf = -1, uMisc = -1, uFlip = -1, uTex[3] = {-1, -1, -1};
    int numRegs = 0;
    C3D_FVec lastC[C3D_FVUNIF_COUNT];
    bool cValid = false;
    u32 lastConst[6] = {};
    bool constValid = false;
    float lastFlip[6] = {9, 9, 9, 9, 9, 9};
    float lastMisc = -1, lastBuf[4] = {-1, -1, -1, -1};
    u32 texMask = 0;          // texture units sampled
};
std::unordered_map<std::string, Program> g_programs;

GLuint compileShader(GLenum type, const std::string& src, const char* what) {
#if W3D_GL_CG
    GLuint s = glCreateShader(type == GL_VERTEX_SHADER ? GL_CG_VERTEX_SHADER_EXT : GL_CG_FRAGMENT_SHADER_EXT);
#else
    GLuint s = glCreateShader(type);
#endif
    const char* p = src.c_str();
    GLint len = (GLint)src.size();
    glShaderSource(s, 1, &p, &len);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof log, &n, log);
        logf("compat: %s shader failed to compile:\n%s\n--- source ---\n%s\n", what, log, src.c_str());
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// ================================================================== context state
struct State {
    bool inited = false;
    shaderProgram_s* program = nullptr;
    C3D_AttrInfo attr;
    C3D_BufInfo buf;
    C3D_TexEnv env[6];
    u32 envBufColor = 0;
    // fragment ops
    bool alphaTest = false;
    GPU_TESTFUNC alphaFunc = GPU_ALWAYS;
    int alphaRef = 0;
    bool depthTest = false;
    GPU_TESTFUNC depthFunc = GPU_GREATER;
    u32 writeMask = GPU_WRITE_ALL;
    GPU_CULLMODE cull = GPU_CULL_BACK_CCW;
    GPU_BLENDEQUATION eqC = GPU_BLEND_ADD, eqA = GPU_BLEND_ADD;
    GPU_BLENDFACTOR sC = GPU_SRC_ALPHA, dC = GPU_ONE_MINUS_SRC_ALPHA, sA = GPU_SRC_ALPHA, dA = GPU_ONE_MINUS_SRC_ALPHA;
    u32 blendColor = 0;
    // textures bound to units 0..2 (tex->data and the parameters at bind time)
    void* texData[3] = {};
    u32 texParam[3] = {};
    // current target
    Target* drawOn = nullptr;
    bool inFrame = false;
    // timing
    u64 frameStartUs = 0;
    float drawMs = 0, procMs = 0;
} S;

u64 nowUs() {
    return svcGetSystemTick() * 1000000ull / SYSCLOCK_ARM11;
}

// ------------------------------------------------------------------ TEV -> fragment shader
struct TevKey {
    u16 srcRgb[6], srcAlpha[6], funcRgb[6], funcAlpha[6], scaleRgb[6], scaleAlpha[6];
    u32 op[6];
    u8 alphaFunc;
};

bool stageIsPassThrough(const C3D_TexEnv& e) {
    return (e.srcRgb & 0xF) == GPU_PREVIOUS && (e.srcAlpha & 0xF) == GPU_PREVIOUS && e.funcRgb == GPU_REPLACE &&
           e.funcAlpha == GPU_REPLACE && (e.opRgb & 0xF) == 0 && (e.opAlpha & 0xF) == 0 && e.scaleRgb == 0 && e.scaleAlpha == 0;
}

struct Dialect {
    bool cg;
    const char* v2() const { return cg ? "float2" : "vec2"; }
    const char* v3() const { return cg ? "float3" : "vec3"; }
    const char* v4() const { return cg ? "float4" : "vec4"; }
    const char* tex() const { return cg ? "tex2D" : "texture2D"; }
    const char* lerp() const { return cg ? "lerp" : "mix"; }
};

std::string srcExpr(int s, int stage, u32& texUsed) {
    switch (s) {
        case GPU_PRIMARY_COLOR: case GPU_FRAGMENT_PRIMARY_COLOR: return "pri";
        case GPU_FRAGMENT_SECONDARY_COLOR: return "zero4";
        case GPU_TEXTURE0: texUsed |= 1; return "t0";
        case GPU_TEXTURE1: texUsed |= 2; return "t1";
        case GPU_TEXTURE2: texUsed |= 4; return "t2";
        case GPU_TEXTURE3: return "zero4";
        case GPU_PREVIOUS_BUFFER: return "buf";
        case GPU_CONSTANT: { char b[16]; snprintf(b, sizeof b, "k%d", stage); return b; }
        case GPU_PREVIOUS: default: return "prev";
    }
}

std::string opRgbExpr(int op, const std::string& v, const Dialect& d) {
    const std::string o = std::string(d.v3()) + "(1.0)";
    switch (op) {
        case GPU_TEVOP_RGB_SRC_COLOR: return v + ".rgb";
        case GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR: return "(" + o + " - " + v + ".rgb)";
        case GPU_TEVOP_RGB_SRC_ALPHA: return v + ".aaa";
        case GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA: return "(" + o + " - " + v + ".aaa)";
        case GPU_TEVOP_RGB_SRC_R: return v + ".rrr";
        case GPU_TEVOP_RGB_ONE_MINUS_SRC_R: return "(" + o + " - " + v + ".rrr)";
        case GPU_TEVOP_RGB_SRC_G: return v + ".ggg";
        case GPU_TEVOP_RGB_ONE_MINUS_SRC_G: return "(" + o + " - " + v + ".ggg)";
        case GPU_TEVOP_RGB_SRC_B: return v + ".bbb";
        case GPU_TEVOP_RGB_ONE_MINUS_SRC_B: return "(" + o + " - " + v + ".bbb)";
        default: return v + ".rgb";
    }
}

std::string opAlphaExpr(int op, const std::string& v) {
    switch (op) {
        case GPU_TEVOP_A_SRC_ALPHA: return v + ".a";
        case GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA: return "(1.0 - " + v + ".a)";
        case GPU_TEVOP_A_SRC_R: return v + ".r";
        case GPU_TEVOP_A_ONE_MINUS_SRC_R: return "(1.0 - " + v + ".r)";
        case GPU_TEVOP_A_SRC_G: return v + ".g";
        case GPU_TEVOP_A_ONE_MINUS_SRC_G: return "(1.0 - " + v + ".g)";
        case GPU_TEVOP_A_SRC_B: return v + ".b";
        case GPU_TEVOP_A_ONE_MINUS_SRC_B: return "(1.0 - " + v + ".b)";
        default: return v + ".a";
    }
}

std::string combine(int func, const std::string& a, const std::string& b, const std::string& c, bool rgb, const Dialect& d) {
    const std::string half = rgb ? std::string(d.v3()) + "(0.5)" : "0.5";
    const std::string one = rgb ? std::string(d.v3()) + "(1.0)" : "1.0";
    switch (func) {
        case GPU_REPLACE: return a;
        case GPU_MODULATE: return a + " * " + b;
        case GPU_ADD: return a + " + " + b;
        case GPU_ADD_SIGNED: return a + " + " + b + " - " + half;
        case GPU_INTERPOLATE: return std::string(d.lerp()) + "(" + b + ", " + a + ", " + c + ")";
        case GPU_SUBTRACT: return a + " - " + b;
        case GPU_DOT3_RGB: case GPU_DOT3_RGBA: {
            if (!rgb) return a;   // handled by the caller for DOT3_RGBA
            return std::string(d.v3()) + "(4.0 * dot(" + a + " - " + half + ", " + b + " - " + half + "))";
        }
        case GPU_MULTIPLY_ADD: return a + " * " + b + " + " + c;
        case GPU_ADD_MULTIPLY: return "min(" + a + " + " + b + ", " + one + ") * " + c;
        default: return a;
    }
}

const char* scaleStr(int s) { return s == GPU_TEVSCALE_2 ? "2.0" : s == GPU_TEVSCALE_4 ? "4.0" : nullptr; }

// outMask: varyings the vertex shader writes (bit n texcoord n, bit 8 colour).
std::string genFragment(const Dialect& d, u32 outMask, u32& texUsed) {
    texUsed = 0;
    std::string body;
    char line[512];
    for (int i = 0; i < 6; ++i) {
        const C3D_TexEnv& e = S.env[i];
        if (stageIsPassThrough(e)) continue;
        std::string sr[3], sa[3];
        for (int k = 0; k < 3; ++k) {
            sr[k] = opRgbExpr((e.opRgb >> (4 * k)) & 0xF, srcExpr((e.srcRgb >> (4 * k)) & 0xF, i, texUsed), d);
            sa[k] = opAlphaExpr((e.opAlpha >> (4 * k)) & 0xF, srcExpr((e.srcAlpha >> (4 * k)) & 0xF, i, texUsed));
        }
        // only the operands a function reads count as used textures: redo the bookkeeping exactly
        auto nIn = [](int f) { return f == GPU_REPLACE ? 1 : (f == GPU_INTERPOLATE || f == GPU_MULTIPLY_ADD || f == GPU_ADD_MULTIPLY) ? 3 : 2; };
        (void)nIn;
        std::string rgb = combine(e.funcRgb, sr[0], sr[1], sr[2], true, d);
        std::string alpha;
        if (e.funcRgb == GPU_DOT3_RGBA) alpha = "(" + rgb + ").r";
        else alpha = combine(e.funcAlpha, sa[0], sa[1], sa[2], false, d);
        const char* scR = scaleStr(e.scaleRgb);
        const char* scA = scaleStr(e.scaleAlpha);
        snprintf(line, sizeof line, "  { %s cr = clamp(%s, 0.0, 1.0); float ca = clamp(%s, 0.0, 1.0);\n", d.v3(), rgb.c_str(), alpha.c_str());
        body += line;
        snprintf(line, sizeof line, "    prev = %s(%s, %s); }\n", d.v4(), scR ? ("min(cr * " + std::string(scR) + ", 1.0)").c_str() : "cr",
                 scA ? ("min(ca * " + std::string(scA) + ", 1.0)").c_str() : "ca");
        body += line;
    }
    // texture unit 2 reads texcoord2 when the vertex shader writes one, else texcoord1 (PICA default)
    const char* tc[3] = {"vtc0", "vtc1", (outMask & 4) ? "vtc2" : "vtc1"};
    std::string alphaCond;
    const char* ref = "u_misc.x";
    switch (S.alphaTest ? S.alphaFunc : GPU_ALWAYS) {
        case GPU_NEVER: alphaCond = "true"; break;
        case GPU_EQUAL: alphaCond = std::string("abs(prev.a - ") + ref + ") > 0.002"; break;
        case GPU_NOTEQUAL: alphaCond = std::string("abs(prev.a - ") + ref + ") <= 0.002"; break;
        case GPU_LESS: alphaCond = std::string("prev.a >= ") + ref; break;
        case GPU_LEQUAL: alphaCond = std::string("prev.a > ") + ref + " + 0.002"; break;
        case GPU_GREATER: alphaCond = std::string("prev.a <= ") + ref; break;
        case GPU_GEQUAL: alphaCond = std::string("prev.a < ") + ref + " - 0.002"; break;
        default: break;
    }
    std::string s;
    std::string head;   // texture fetches and inputs
    for (int u = 0; u < 3; ++u)
        if (texUsed & (1u << u)) {
            const char* v = tc[u];
            if (!(outMask & (1u << (v[3] - '0')))) {   // vertex shader has no such texcoord: sample at 0
                snprintf(line, sizeof line, "  %s t%d = %s(tex%d, %s(0.0, 0.0));\n", d.v4(), u, d.tex(), u, d.v2());
            } else {
                snprintf(line, sizeof line, "  %s t%d = %s(tex%d, %s(%s.x, u_flip[%d].x + u_flip[%d].y * %s.y));\n", d.v4(), u, d.tex(), u,
                         d.v2(), v, u, u, v);
            }
            head += line;
        }
    std::string consts;
    for (int i = 0; i < 6; ++i) {
        snprintf(line, sizeof line, "  %s k%d = u_const[%d];\n", d.v4(), i, i);
        consts += line;
    }
    const std::string priExpr = (outMask & 0x100) ? std::string("clamp(vcol, 0.0, 1.0)") : std::string(d.v4()) + "(1.0)";
    if (!d.cg) {
        s = "#version 100\nprecision mediump float;\n";
        for (int n = 0; n < 3; ++n)
            if (outMask & (1u << n)) { snprintf(line, sizeof line, "varying vec2 vtc%d;\n", n); s += line; }
        if (outMask & 0x100) s += "varying vec4 vcol;\n";
        s += "uniform sampler2D tex0;\nuniform sampler2D tex1;\nuniform sampler2D tex2;\n";
        s += "uniform vec4 u_const[6];\nuniform vec4 u_buf;\nuniform vec4 u_misc;\nuniform vec4 u_flip[3];\n";
        s += "void main() {\n";
        s += "  vec4 zero4 = vec4(0.0);\n  vec4 pri = " + priExpr + ";\n  vec4 buf = u_buf;\n  vec4 prev = vec4(0.0);\n";
        s += consts + head + body;
        if (!alphaCond.empty()) s += "  if (" + alphaCond + ") discard;\n";
        s += "  gl_FragColor = prev;\n}\n";
    } else {
        std::string params;
        for (int n = 0; n < 3; ++n)
            if (outMask & (1u << n)) { snprintf(line, sizeof line, "float2 vtc%d : TEXCOORD%d, ", n, n); params += line; }
        if (outMask & 0x100) params += "float4 vcol : COLOR, ";
        params += "uniform sampler2D tex0, uniform sampler2D tex1, uniform sampler2D tex2, ";
        params += "uniform float4 u_const[6], uniform float4 u_buf, uniform float4 u_misc, uniform float4 u_flip[3]";
        s = "float4 main(" + params + ") : COLOR {\n";
        s += "  float4 zero4 = float4(0.0);\n  float4 pri = " + priExpr + ";\n  float4 buf = u_buf;\n  float4 prev = float4(0.0);\n";
        s += consts + head + body;
        if (!alphaCond.empty()) s += "  if (" + alphaCond + ") discard;\n";
        s += "  return prev;\n}\n";
    }
    return s;
}

std::string tevKeyString(u32 vsId) {
    TevKey k;
    memset(&k, 0, sizeof k);
    for (int i = 0; i < 6; ++i) {
        const C3D_TexEnv& e = S.env[i];
        k.srcRgb[i] = e.srcRgb;
        k.srcAlpha[i] = e.srcAlpha;
        k.funcRgb[i] = e.funcRgb;
        k.funcAlpha[i] = e.funcAlpha;
        k.scaleRgb[i] = e.scaleRgb;
        k.scaleAlpha[i] = e.scaleAlpha;
        k.op[i] = e.opAll & 0xFFFFFF;
    }
    k.alphaFunc = S.alphaTest ? (u8)S.alphaFunc : (u8)GPU_ALWAYS;
    std::string s((const char*)&k, sizeof k);
    s.append((const char*)&vsId, sizeof vsId);
    return s;
}

Program* currentProgram() {
    if (!S.program || !S.program->vertexShader || !S.program->vertexShader->dvle) return nullptr;
    DVLE_s* dv = S.program->vertexShader->dvle;
    const std::string key = tevKeyString(dv->id);
    auto it = g_programs.find(key);
    if (it != g_programs.end()) return it->second.failed ? nullptr : &it->second;
    Program& p = g_programs[key];
    VsInfo& vs = g_vs[dv->id];
    if (!vs.shader && !vs.failed) {
        vs.dvle = dv;
        vs.shader = compileShader(GL_VERTEX_SHADER, dv->source, "vertex");
        vs.failed = !vs.shader;
    }
    if (vs.failed) { p.failed = true; return nullptr; }
    Dialect d{W3D_GL_CG != 0};
    u32 texUsed = 0;
    const std::string fsrc = genFragment(d, dv->outMask, texUsed);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsrc, "fragment (TEV)");
    if (!fs) { p.failed = true; return nullptr; }
    p.prog = glCreateProgram();
    glAttachShader(p.prog, vs.shader);
    glAttachShader(p.prog, fs);
    for (int i = 0; i < dv->numInputs && i < 16; ++i) {
        char n[8];
        snprintf(n, sizeof n, "a%d", i);
        glBindAttribLocation(p.prog, i, n);
    }
    glLinkProgram(p.prog);
    GLint ok = 0;
    glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetProgramInfoLog(p.prog, sizeof log, nullptr, log);
        logf("compat: program link failed: %s\n%s\n", log, fsrc.c_str());
        p.failed = true;
        return nullptr;
    }
    glDeleteShader(fs);
    p.uC = glGetUniformLocation(p.prog, "c");
    if (p.uC < 0) p.uC = glGetUniformLocation(p.prog, "c[0]");
    p.uConst = glGetUniformLocation(p.prog, "u_const");
    if (p.uConst < 0) p.uConst = glGetUniformLocation(p.prog, "u_const[0]");
    p.uBuf = glGetUniformLocation(p.prog, "u_buf");
    p.uMisc = glGetUniformLocation(p.prog, "u_misc");
    p.uFlip = glGetUniformLocation(p.prog, "u_flip");
    if (p.uFlip < 0) p.uFlip = glGetUniformLocation(p.prog, "u_flip[0]");
    glUseProgram(p.prog);
    for (int u = 0; u < 3; ++u) {
        char n[8];
        snprintf(n, sizeof n, "tex%d", u);
        p.uTex[u] = glGetUniformLocation(p.prog, n);
        if (p.uTex[u] >= 0) glUniform1i(p.uTex[u], u);
    }
    p.numRegs = dv->numRegs;
    p.texMask = texUsed;
    return &p;
}

void colorToVec(u32 c, float* v) {   // TEV / blend colours: 0xAABBGGRR
    v[0] = (c & 0xFF) / 255.0f;
    v[1] = ((c >> 8) & 0xFF) / 255.0f;
    v[2] = ((c >> 16) & 0xFF) / 255.0f;
    v[3] = (c >> 24) / 255.0f;
}

GLenum glTest(GPU_TESTFUNC f) {
    switch (f) {
        case GPU_NEVER: return GL_NEVER;
        case GPU_ALWAYS: return GL_ALWAYS;
        case GPU_EQUAL: return GL_EQUAL;
        case GPU_NOTEQUAL: return GL_NOTEQUAL;
        case GPU_LESS: return GL_LESS;
        case GPU_LEQUAL: return GL_LEQUAL;
        case GPU_GREATER: return GL_GREATER;
        case GPU_GEQUAL: return GL_GEQUAL;
    }
    return GL_ALWAYS;
}

GLenum glFactor(GPU_BLENDFACTOR f) {
    switch (f) {
        case GPU_ZERO: return GL_ZERO;
        case GPU_ONE: return GL_ONE;
        case GPU_SRC_COLOR: return GL_SRC_COLOR;
        case GPU_ONE_MINUS_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
        case GPU_DST_COLOR: return GL_DST_COLOR;
        case GPU_ONE_MINUS_DST_COLOR: return GL_ONE_MINUS_DST_COLOR;
        case GPU_SRC_ALPHA: return GL_SRC_ALPHA;
        case GPU_ONE_MINUS_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        case GPU_DST_ALPHA: return GL_DST_ALPHA;
        case GPU_ONE_MINUS_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
#if defined(GL_CONSTANT_COLOR)
        case GPU_CONSTANT_COLOR: return GL_CONSTANT_COLOR;
        case GPU_ONE_MINUS_CONSTANT_COLOR: return GL_ONE_MINUS_CONSTANT_COLOR;
        case GPU_CONSTANT_ALPHA: return GL_CONSTANT_ALPHA;
        case GPU_ONE_MINUS_CONSTANT_ALPHA: return GL_ONE_MINUS_CONSTANT_ALPHA;
#else   // vitaGL: the Vita's GPU has no blend constant (the game never uses one)
        case GPU_CONSTANT_COLOR: case GPU_CONSTANT_ALPHA: return GL_ONE;
        case GPU_ONE_MINUS_CONSTANT_COLOR: case GPU_ONE_MINUS_CONSTANT_ALPHA: return GL_ZERO;
#endif
        case GPU_SRC_ALPHA_SATURATE: return GL_SRC_ALPHA_SATURATE;
    }
    return GL_ONE;
}

GLenum glEq(GPU_BLENDEQUATION e) {
    switch (e) {
        case GPU_BLEND_ADD: return GL_FUNC_ADD;
        case GPU_BLEND_SUBTRACT: return GL_FUNC_SUBTRACT;
        case GPU_BLEND_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
        case GPU_BLEND_MIN: return GL_MIN_EXT;
        case GPU_BLEND_MAX: return GL_MAX_EXT;
    }
    return GL_FUNC_ADD;
}

// GL state shadow: avoid re-sending what did not change.
struct GlShadow {
    int depthEnabled = -1, depthMask = -1, blendEnabled = -1, cullEnabled = -1;
    GLenum depthFunc = 0, frontFace = 0;
    int colorMask = -1;
    GLenum blend[6] = {};
    u32 blendColor = 1;
    GLuint program = 0;
    u32 attribEnabled = 0;
    GLuint boundTex[3] = {};
    int activeUnit = -1;
} G;

void setCap(GLenum cap, bool on, int& shadow) {
    if (shadow == (int)on) return;
    shadow = on;
    if (on) glEnable(cap); else glDisable(cap);
}

void applyFragmentOps() {
    // depth
    const bool depthWrite = (S.writeMask & GPU_WRITE_DEPTH) != 0;
    const bool test = S.depthTest || depthWrite;   // PICA writes depth with the test off; GL needs it on (ALWAYS)
    setCap(GL_DEPTH_TEST, test, G.depthEnabled);
    const GLenum df = S.depthTest ? glTest(S.depthFunc) : GL_ALWAYS;
    if (test && G.depthFunc != df) { glDepthFunc(df); G.depthFunc = df; }
    if (G.depthMask != (int)depthWrite) { glDepthMask(depthWrite); G.depthMask = depthWrite; }
    const int cm = (int)(S.writeMask & 0xF);
    if (G.colorMask != cm) {
        glColorMask(cm & 1, (cm >> 1) & 1, (cm >> 2) & 1, (cm >> 3) & 1);
        G.colorMask = cm;
    }
    // cull
    setCap(GL_CULL_FACE, S.cull != GPU_CULL_NONE, G.cullEnabled);
    if (S.cull != GPU_CULL_NONE) {
        const GLenum ff = S.cull == GPU_CULL_BACK_CCW ? GL_CCW : GL_CW;   // the kept face's winding
        if (G.frontFace != ff) { glFrontFace(ff); glCullFace(GL_BACK); G.frontFace = ff; }
    }
    // blend
    const bool noBlend = S.eqC == GPU_BLEND_ADD && S.eqA == GPU_BLEND_ADD && S.sC == GPU_ONE && S.dC == GPU_ZERO && S.sA == GPU_ONE && S.dA == GPU_ZERO;
    setCap(GL_BLEND, !noBlend, G.blendEnabled);
    if (!noBlend) {
        const GLenum b[6] = {glEq(S.eqC), glEq(S.eqA), glFactor(S.sC), glFactor(S.dC), glFactor(S.sA), glFactor(S.dA)};
        if (memcmp(b, G.blend, sizeof b)) {
            glBlendEquationSeparate(b[0], b[1]);
            glBlendFuncSeparate(b[2], b[3], b[4], b[5]);
            memcpy(G.blend, b, sizeof b);
        }
#if defined(GL_CONSTANT_COLOR)
        if (G.blendColor != S.blendColor) {
            float c[4];
            colorToVec(S.blendColor, c);
            glBlendColor(c[0], c[1], c[2], c[3]);
            G.blendColor = S.blendColor;
        }
#endif
    }
}

void bindTextures(Program& p, float flip[6]) {
    for (int u = 0; u < 3; ++u) {
        flip[u * 2] = 1.0f;     // t' = 1 - t
        flip[u * 2 + 1] = -1.0f;
        if (!(p.texMask & (1u << u))) continue;
        void* data = S.texData[u];
        GLuint gl = 0;
        TexRec* rec = nullptr;
        {
            std::lock_guard<std::mutex> g(g_texLock);
            auto it = data ? g_texByData.find((uintptr_t)data) : g_texByData.end();
            if (it != g_texByData.end()) rec = &it->second;
        }
        if (G.activeUnit != u) { glActiveTexture(GL_TEXTURE0 + u); G.activeUnit = u; }
        if (rec) {
            if (!rec->gl) glGenTextures(1, &rec->gl);
            gl = rec->gl;
            glBindTexture(GL_TEXTURE_2D, gl);
            G.boundTex[u] = gl;
            if (rec->dirty && !rec->target) {
                rec->dirty = false;
                uploadTex(*rec, (const u8*)data);
            }
            applyParams(*rec, S.texParam[u]);
            if (rec->target) { flip[u * 2] = 0.0f; flip[u * 2 + 1] = 1.0f; }
        } else if (G.boundTex[u]) {
            glBindTexture(GL_TEXTURE_2D, 0);
            G.boundTex[u] = 0;
        }
    }
}

int attrSize(int fmt, int count) {
    const int c = fmt == GPU_FLOAT ? 4 : fmt == GPU_SHORT ? 2 : 1;
    return c * count;
}

GLenum attrType(int fmt) {
    switch (fmt) {
        case GPU_BYTE: return GL_BYTE;
        case GPU_UNSIGNED_BYTE: return GL_UNSIGNED_BYTE;
        case GPU_SHORT: return GL_SHORT;
        default: return GL_FLOAT;
    }
}

bool prepareDraw() {
    if (!S.drawOn) return false;
    Program* p = currentProgram();
    if (!p) return false;
    if (G.program != p->prog) { glUseProgram(p->prog); G.program = p->prog; }
    // vertex uniforms c[0..numRegs-1] (PICA vectors are stored w z y x)
    static float c[C3D_FVUNIF_COUNT * 4];
    const int n = std::min(p->numRegs, C3D_FVUNIF_COUNT);
    if (!p->cValid || memcmp(p->lastC, C3D_FVUnif[GPU_VERTEX_SHADER], sizeof(C3D_FVec) * n)) {
        for (int i = 0; i < n; ++i) {
            const C3D_FVec& v = C3D_FVUnif[GPU_VERTEX_SHADER][i];
            c[i * 4] = v.x;
            c[i * 4 + 1] = v.y;
            c[i * 4 + 2] = v.z;
            c[i * 4 + 3] = v.w;
        }
        if (p->uC >= 0) glUniform4fv(p->uC, n, c);
        memcpy(p->lastC, C3D_FVUnif[GPU_VERTEX_SHADER], sizeof(C3D_FVec) * n);
        p->cValid = true;
    }
    // fragment uniforms
    u32 consts[6];
    for (int i = 0; i < 6; ++i) consts[i] = S.env[i].color;
    if (!p->constValid || memcmp(consts, p->lastConst, sizeof consts)) {
        float k[24];
        for (int i = 0; i < 6; ++i) colorToVec(consts[i], k + i * 4);
        if (p->uConst >= 0) glUniform4fv(p->uConst, 6, k);
        memcpy(p->lastConst, consts, sizeof consts);
        p->constValid = true;
    }
    float bufc[4];
    colorToVec(S.envBufColor, bufc);
    if (memcmp(bufc, p->lastBuf, sizeof bufc)) {
        if (p->uBuf >= 0) glUniform4fv(p->uBuf, 1, bufc);
        memcpy(p->lastBuf, bufc, sizeof bufc);
    }
    const float ref = S.alphaRef / 255.0f;
    if (p->lastMisc != ref) {
        if (p->uMisc >= 0) glUniform4f(p->uMisc, ref, 0, 0, 0);
        p->lastMisc = ref;
    }
    float flip[6];
    bindTextures(*p, flip);
    if (memcmp(flip, p->lastFlip, sizeof flip)) {
        float f[12] = {flip[0], flip[1], 0, 0, flip[2], flip[3], 0, 0, flip[4], flip[5], 0, 0};
        if (p->uFlip >= 0) glUniform4fv(p->uFlip, 3, f);
        memcpy(p->lastFlip, flip, sizeof flip);
    }
    applyFragmentOps();
    // vertex attributes: loader i -> shader input permutation[i]; buffers list their loaders in order
    u32 enabled = 0;
    const C3D_AttrInfo& ai = S.attr;
    for (int b = 0; b < S.buf.bufCount; ++b) {
        const C3D_BufCfg& bc = S.buf.buffers[b];
        const int count = (int)(bc.flags[1] >> 28);
        const int stride = (int)((bc.flags[1] >> 16) & 0xFFF);
        const u64 perm = (u64)bc.flags[0] | ((u64)(bc.flags[1] & 0xFFFF) << 32);
        int off = 0;
        for (int k = 0; k < count; ++k) {
            const int loader = (int)((perm >> (4 * k)) & 0xF);
            if (loader >= 12) {   // padding entries (0xC..0xF): 4/8/12/16 bytes
                off += 4 * (loader - 11);
                continue;
            }
            const u32 f = loader < 8 ? (ai.flags[0] >> (loader * 4)) & 0xF : (ai.flags[1] >> ((loader - 8) * 4)) & 0xF;
            const int fmt = (int)(f & 3), cnt = (int)(f >> 2) + 1;
            const int csz = fmt == GPU_FLOAT ? 4 : fmt == GPU_SHORT ? 2 : 1;
            off = (off + csz - 1) / csz * csz;
            const int reg = (int)((ai.permutation >> (loader * 4)) & 0xF);
            glVertexAttribPointer(reg, cnt, attrType(fmt), GL_FALSE, stride, (const u8*)bc.ptr + off);
            enabled |= 1u << reg;
            off += attrSize(fmt, cnt);
        }
    }
    for (int r = 0; r < 16; ++r) {
        const u32 bit = 1u << r;
        if ((enabled & bit) && !(G.attribEnabled & bit)) glEnableVertexAttribArray(r);
        else if (!(enabled & bit) && (G.attribEnabled & bit)) {
            glDisableVertexAttribArray(r);
            glVertexAttrib4f(r, 0, 0, 0, 1);
        }
    }
    G.attribEnabled = enabled;
    return true;
}

GLenum glPrim(GPU_Primitive_t p) {
    switch (p) {
        case GPU_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
        case GPU_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
        default: return GL_TRIANGLES;
    }
}

void bindTarget(Target* t) {
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
}

// ------------------------------------------------------------------ composing the screens onto the display
GLuint g_blitProg = 0;
GLint g_blitTex = -1;

void initBlit() {
#if W3D_GL_CG
    const char* vs =
        "void main(float2 a0, float2 a1, float4 out vgl_Position : POSITION, float2 out vtc0 : TEXCOORD0) {\n"
        "  vgl_Position = float4(a0, 0.0, 1.0);\n  vtc0 = a1;\n}\n";
    const char* fs = "float4 main(float2 vtc0 : TEXCOORD0, uniform sampler2D tex0) : COLOR {\n  return float4(tex2D(tex0, vtc0).rgb, 1.0);\n}\n";
#else
    const char* vs =
        "#version 100\nattribute vec2 a0;\nattribute vec2 a1;\nvarying vec2 vtc0;\n"
        "void main() {\n  gl_Position = vec4(a0, 0.0, 1.0);\n  vtc0 = a1;\n}\n";
    const char* fs = "#version 100\nprecision mediump float;\nvarying vec2 vtc0;\nuniform sampler2D tex0;\n"
                     "void main() {\n  gl_FragColor = vec4(texture2D(tex0, vtc0).rgb, 1.0);\n}\n";
#endif
    GLuint v = compileShader(GL_VERTEX_SHADER, vs, "blit vertex");
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs, "blit fragment");
    g_blitProg = glCreateProgram();
    glAttachShader(g_blitProg, v);
    glAttachShader(g_blitProg, f);
    glBindAttribLocation(g_blitProg, 0, "a0");
    glBindAttribLocation(g_blitProg, 1, "a1");
    glLinkProgram(g_blitProg);
    g_blitTex = glGetUniformLocation(g_blitProg, "tex0");
}

// A texture onto a display rect. Render targets have their row 0 at the bottom (GL), the console's at the top.
void blitTex(GLuint tex, compat::Rect r, bool rowZeroTop) {
    const float W = (float)compat::displayWidth(), H = (float)compat::displayHeight();
    // display rect (y down) -> clip space
    const float x0 = r.x / W * 2 - 1, x1 = (r.x + r.w) / W * 2 - 1;
    const float y0 = 1 - (r.y + r.h) / H * 2, y1 = 1 - r.y / H * 2;
    const float vb = rowZeroTop ? 1.0f : 0.0f, vt = 1.0f - vb;
    const float v[] = {x0, y0, 0, vb, x1, y0, 1, vb, x0, y1, 0, vt, x1, y1, 1, vt};
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void blit(Target* t, compat::Rect r) { blitTex(t->color, r, false); }

// libctru's text console on the touch screen (console.cpp): a 320x240 texture updated when its text changed
GLuint g_consoleTex = 0;

void blitConsole(compat::Rect r) {
    const u32* px = compat::consolePixels();
    if (!g_consoleTex) {
        glGenTextures(1, &g_consoleTex);
        glBindTexture(GL_TEXTURE_2D, g_consoleTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 320, 240, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        px = nullptr;
    }
    glBindTexture(GL_TEXTURE_2D, g_consoleTex);
    if (px) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 320, 240, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    blitTex(g_consoleTex, r, true);
}

void compose() {
    glBindFramebuffer(GL_FRAMEBUFFER, compat::displayFramebuffer());
    glViewport(0, 0, compat::displayWidth(), compat::displayHeight());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(1, 1, 1, 1);
    G = GlShadow();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!g_blitProg) initBlit();
    glUseProgram(g_blitProg);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(g_blitTex, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    for (int a = 2; a < 16; ++a) glDisableVertexAttribArray(a);
    if (Target* top = g_outputs[GFX_TOP][GFX_LEFT]) blit(top, compat::layoutTop());
    // the touch screen: the GPU's target once drawn, the console from consoleInit / consoleClear until then
    Target* bot = g_outputs[GFX_BOTTOM][GFX_LEFT];
    if (bot && bot->drawn) compat::consoleHide();
    if (compat::layoutMode() != 0) {   // a frame around the touch screen where it lies over the scene
        const compat::Rect b = compat::layoutBottom();
        const int H = compat::displayHeight(), f = compat::layoutPanelOpen() ? 3 : 2;
        glEnable(GL_SCISSOR_TEST);
        glScissor(b.x - f, H - (b.y + b.h) - f, b.w + 2 * f, b.h + 2 * f);
        glClearColor(0.55f, 0.42f, 0.16f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    }
    if (compat::consoleShown() || !bot) blitConsole(compat::layoutBottom());
    else blit(bot, compat::layoutBottom());
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    G = GlShadow();
}

}  // namespace

// ================================================================== public citro3d API
extern "C" {

bool C3D_Init(size_t cmdBufSize) {
    (void)cmdBufSize;
    if (S.inited) return true;
    if (!compat::platformInitGL()) return false;
    S.inited = true;
    for (int i = 0; i < 6; ++i) C3D_TexEnvInit(&S.env[i]);
    AttrInfo_Init(&S.attr);
    BufInfo_Init(&S.buf);
    S.alphaTest = false;
    S.depthTest = true;
    S.depthFunc = GPU_GREATER;
    S.writeMask = GPU_WRITE_ALL;
    S.cull = GPU_CULL_BACK_CCW;
    S.eqC = S.eqA = GPU_BLEND_ADD;
    S.sC = S.sA = GPU_SRC_ALPHA;
    S.dC = S.dA = GPU_ONE_MINUS_SRC_ALPHA;
    G = GlShadow();
    return true;
}

void C3D_Fini(void) {
    if (!S.inited) return;
    for (auto& kv : g_programs)
        if (kv.second.prog) glDeleteProgram(kv.second.prog);
    g_programs.clear();
    for (auto& kv : g_vs)
        if (kv.second.shader) glDeleteShader(kv.second.shader);
    g_vs.clear();
    compat::platformShutdownGL();
    S.inited = false;
}

float C3D_GetCmdBufUsage(void) { return 0.05f; }
float C3D_GetDrawingTime(void) { return S.drawMs; }
float C3D_GetProcessingTime(void) { return S.procMs; }
float C3D_FrameRate(float fps) { (void)fps; return 60.0f; }
void C3D_FrameSync(void) {}
u32 C3D_FrameCounter(int id) { (void)id; return 0; }

void C3D_BindProgram(shaderProgram_s* program) { S.program = program; }
void C3D_SetViewport(u32 x, u32 y, u32 w, u32 h) {
    if (!S.drawOn) return;
    // the game's viewport is in tilted 3DS units on screens; only full-target viewports are used, keep it full
    (void)x; (void)y; (void)w; (void)h;
}
void C3D_SetScissor(GPU_SCISSORMODE mode, u32 left, u32 top, u32 right, u32 bottom) {
    (void)left; (void)top; (void)right; (void)bottom;
    if (mode == GPU_SCISSOR_DISABLE) glDisable(GL_SCISSOR_TEST);
}

void C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size) {
    if (!prepareDraw()) return;
    glDrawArrays(glPrim(primitive), first, size);
}

void C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void* indices) {
    if (!prepareDraw()) return;
    glDrawElements(glPrim(primitive), count, type == C3D_UNSIGNED_SHORT ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, indices);
}

void C3D_ImmDrawBegin(GPU_Primitive_t primitive) { (void)primitive; }
void C3D_ImmSendAttrib(float x, float y, float z, float w) { (void)x; (void)y; (void)z; (void)w; }
void C3D_ImmDrawEnd(void) {}
C3D_FVec* C3D_FixedAttribGetWritePtr(int id) {
    static C3D_FVec fixed[12];
    return &fixed[(unsigned)id % 12];
}

// ------------------------------------------------------------------ attributes / buffers (citro3d's encoding)
void AttrInfo_Init(C3D_AttrInfo* info) {
    memset(info, 0, sizeof(*info));
    info->flags[1] = 0xFFF << 16;
}

int AttrInfo_AddLoader(C3D_AttrInfo* info, int regId, GPU_FORMATS format, int count) {
    if (info->attrCount == 12) return -1;
    int id = info->attrCount++;
    if (regId < 0) regId = id;
    if (id < 8) info->flags[0] |= GPU_ATTRIBFMT(id, count, format);
    else info->flags[1] |= GPU_ATTRIBFMT(id - 8, count, format);
    info->flags[1] = (info->flags[1] & ~(0xF0000000 | BIT(id + 16))) | (id << 28);
    info->permutation |= (u64)regId << (id * 4);
    return id;
}

int AttrInfo_AddFixed(C3D_AttrInfo* info, int regId) {
    if (info->attrCount == 12) return -1;
    int id = info->attrCount++;
    if (regId < 0) regId = id;
    info->flags[1] = (info->flags[1] & ~0xF0000000) | (id << 28);
    info->permutation |= (u64)regId << (id * 4);
    return id;
}

C3D_AttrInfo* C3D_GetAttrInfo(void) { return &S.attr; }
void C3D_SetAttrInfo(C3D_AttrInfo* info) {
    if (info != &S.attr) memcpy(&S.attr, info, sizeof(*info));
}

void BufInfo_Init(C3D_BufInfo* info) {
    memset(info, 0, sizeof(*info));
}

int BufInfo_Add(C3D_BufInfo* info, const void* data, ptrdiff_t stride, int attribCount, u64 permutation) {
    if (info->bufCount == 12) return -1;
    int id = info->bufCount++;
    C3D_BufCfg* buf = &info->buffers[id];
    buf->offset = (u32)(uintptr_t)data;
    buf->ptr = data;
    buf->flags[0] = (u32)(permutation & 0xFFFFFFFF);
    buf->flags[1] = (u32)((permutation >> 32) & 0xFFFF) | ((u32)stride << 16) | ((u32)attribCount << 28);
    return id;
}

C3D_BufInfo* C3D_GetBufInfo(void) { return &S.buf; }
void C3D_SetBufInfo(C3D_BufInfo* info) {
    if (info != &S.buf) memcpy(&S.buf, info, sizeof(*info));
}

// ------------------------------------------------------------------ TEV
C3D_TexEnv* C3D_GetTexEnv(int id) { return &S.env[id % 6]; }
void C3D_SetTexEnv(int id, C3D_TexEnv* env) {
    if (env) memcpy(&S.env[id % 6], env, sizeof(*env));
    else C3D_TexEnvInit(&S.env[id % 6]);
}
void C3D_DirtyTexEnv(C3D_TexEnv* env) { (void)env; }
void C3D_TexEnvBufUpdate(int mode, int mask) { (void)mode; (void)mask; }
void C3D_TexEnvBufColor(u32 color) { S.envBufColor = color; }

// ------------------------------------------------------------------ fragment operations
void C3D_DepthMap(bool bIsZBuffer, float zScale, float zOffset) { (void)bIsZBuffer; (void)zScale; (void)zOffset; }
void C3D_CullFace(GPU_CULLMODE mode) { S.cull = mode; }
void C3D_StencilTest(bool enable, GPU_TESTFUNC function, int ref, int inputMask, int writeMask) {
    (void)enable; (void)function; (void)ref; (void)inputMask; (void)writeMask;
}
void C3D_StencilOp(GPU_STENCILOP sfail, GPU_STENCILOP dfail, GPU_STENCILOP pass) { (void)sfail; (void)dfail; (void)pass; }
void C3D_BlendingColor(u32 color) { S.blendColor = color; }
void C3D_EarlyDepthTest(bool enable, GPU_EARLYDEPTHFUNC function, u32 ref) { (void)enable; (void)function; (void)ref; }
void C3D_DepthTest(bool enable, GPU_TESTFUNC function, GPU_WRITEMASK writemask) {
    S.depthTest = enable;
    S.depthFunc = function;
    S.writeMask = writemask;
}
void C3D_AlphaTest(bool enable, GPU_TESTFUNC function, int ref) {
    S.alphaTest = enable;
    S.alphaFunc = function;
    S.alphaRef = ref;
}
void C3D_AlphaBlend(GPU_BLENDEQUATION colorEq, GPU_BLENDEQUATION alphaEq, GPU_BLENDFACTOR srcClr, GPU_BLENDFACTOR dstClr,
                    GPU_BLENDFACTOR srcAlpha, GPU_BLENDFACTOR dstAlpha) {
    S.eqC = colorEq;
    S.eqA = alphaEq;
    S.sC = srcClr;
    S.dC = dstClr;
    S.sA = srcAlpha;
    S.dA = dstAlpha;
}
void C3D_ColorLogicOp(GPU_LOGICOP op) {
    (void)op;   // only "copy" makes sense here: plain replace
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}
void C3D_FragOpMode(GPU_FRAGOPMODE mode) { (void)mode; }
void C3D_FragOpShadow(float scale, float bias) { (void)scale; (void)bias; }

// fog / gas: the game computes its fog in the shaders; the fixed-function fog is not emulated
void FogLut_FromArray(C3D_FogLut* lut, const float data[256]) { (void)lut; (void)data; }
void FogLut_Exp(C3D_FogLut* lut, float density, float gradient, float near, float far) { (void)lut; (void)density; (void)gradient; (void)near; (void)far; }
void C3D_FogGasMode(GPU_FOGMODE fogMode, GPU_GASMODE gasMode, bool zFlip) { (void)fogMode; (void)gasMode; (void)zFlip; }
void C3D_FogColor(u32 color) { (void)color; }
void C3D_FogLutBind(C3D_FogLut* lut) { (void)lut; }

void C3D_UpdateUniforms(GPU_SHADER_TYPE type) { (void)type; }

// ------------------------------------------------------------------ textures
bool C3D_TexInitWithParams(C3D_Tex* tex, C3D_TexCube* cube, C3D_TexInitParams p) {
    (void)cube;
    if (p.type != GPU_TEX_2D && p.type != GPU_TEX_SHADOW_2D) return false;
    const u32 size = (u32)p.width * p.height * fmtBits(p.format) / 8;
    const u32 total = C3D_TexCalcTotalSize(size, p.maxLevel);
    void* data = p.onVram ? vramAlloc(total) : linearAlloc(total);
    if (!data) return false;
    memset(tex, 0, sizeof(*tex));
    tex->data = data;
    tex->fmt = p.format;
    tex->size = size;
    tex->width = p.width;
    tex->height = p.height;
    tex->param = GPU_TEXTURE_MODE(p.type);
    if (p.format == GPU_ETC1) tex->param |= GPU_TEXTURE_ETC1_PARAM;
    tex->border = 0;
    tex->maxLevel = p.maxLevel;
    tex->minLevel = 0;
    TexRec r;
    r.width = p.width;
    r.height = p.height;
    r.fmt = p.format;
    r.maxLevel = p.maxLevel;
    r.total = total;
    std::lock_guard<std::mutex> g(g_texLock);
    g_texByData[(uintptr_t)data] = r;
    return true;
}

void C3D_TexLoadImage(C3D_Tex* tex, const void* data, GPU_TEXFACE face, int level) {
    (void)face;
    u32 size = 0;
    void* out = C3D_Tex2DGetImagePtr(tex, level, &size);
    if (!out || !data) return;
    memcpy(out, data, size);
    markDirtyRange(out, size);
}

void C3D_TexGenerateMipmap(C3D_Tex* tex, GPU_TEXFACE face) { (void)face; markDirtyRange(tex->data, 1); }

void C3D_TexBind(int unitId, C3D_Tex* tex) {
    if (unitId < 0 || unitId > 2) return;
    S.texData[unitId] = tex ? tex->data : nullptr;
    S.texParam[unitId] = tex ? tex->param : 0;
}

void C3D_TexFlush(C3D_Tex* tex) {
    if (tex && tex->data) markDirtyRange(tex->data, 1);
}

void C3D_TexDelete(C3D_Tex* tex) {
    if (!tex || !tex->data) return;
    void* data = tex->data;
    bool vram = false;
    {
        std::lock_guard<std::mutex> g(g_texLock);
        auto it = g_texByData.find((uintptr_t)data);
        if (it != g_texByData.end()) {
            if (it->second.gl) g_glTexToDelete.push_back(it->second.gl);
            g_texByData.erase(it);
        }
    }
    (void)vram;
    linearFree(data);   // vramAlloc and linearAlloc share the allocator here
    for (int u = 0; u < 3; ++u)
        if (S.texData[u] == data) S.texData[u] = nullptr;
    tex->data = nullptr;
}

void C3D_TexShadowParams(bool perspective, float bias) { (void)perspective; (void)bias; }

void C3D_SyncTextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags) {
    (void)indim; (void)outdim; (void)flags;
    if (!inadr || !outadr || !size) return;
    memmove(outadr, inadr, size);
    markDirtyRange(outadr, size);
}
void C3D_SyncDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags) {
    (void)inadr; (void)indim; (void)outadr; (void)outdim; (void)flags;
}
void C3D_SyncMemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0, u32* buf1a, u32 buf1v, u32* buf1e, u16 control1) {
    (void)control0; (void)control1;
    if (buf0a && buf0e) for (u32* p = buf0a; p < buf0e; ++p) *p = buf0v;
    if (buf1a && buf1e) for (u32* p = buf1a; p < buf1e; ++p) *p = buf1v;
}

// ------------------------------------------------------------------ render targets
static Target* newTarget(int w, int h, bool screen, GLuint colorTex, bool depth) {
    Target* t = new Target();
    memset(&t->t, 0, sizeof t->t);
    t->screen = screen;
    t->w = w;
    t->h = h;
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    if (colorTex) {
        t->color = colorTex;
    } else {
        glGenTextures(1, &t->color);
        glBindTexture(GL_TEXTURE_2D, t->color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0);
    if (depth) {
        glGenRenderbuffers(1, &t->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
#if defined(__vita__)
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
#else
        glRenderbufferStorage(GL_RENDERBUFFER, 0x81A6 /* GL_DEPTH_COMPONENT24 (ES3 context) */, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
#endif
    }
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) logf("compat: framebuffer %dx%d incomplete (0x%x)\n", w, h, st);
    glBindFramebuffer(GL_FRAMEBUFFER, compat::displayFramebuffer());
    G.boundTex[0] = G.boundTex[1] = G.boundTex[2] = 0xFFFFFFFF;
    g_targets.push_back(t);
    return t;
}

C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, C3D_DEPTHTYPE depthFmt) {
    // A 3DS screen target is made portrait (240 x 400): the GL one is landscape, and the top screen is made at the
    // size it is shown at on the display, so the scene is rendered at the Vita's resolution.
    const int lw = height, lh = width;
    // (the touch screen stays at its own 320x240: its 2D UI gains nothing from more pixels)
    const compat::Rect r = lw >= 400 ? compat::layoutTopMax() : compat::Rect{0, 0, lw, lh};
    const int w = std::max(lw, r.w), h = std::max(lh, r.h);
    Target* t = newTarget(w, h, true, 0, C3D_DEPTHTYPE_OK(depthFmt));
    t->logicalW = lw;
    t->logicalH = lh;
    C3D_FrameBuf* fb = &t->t.frameBuf;
    fb->width = (u16)width;
    fb->height = (u16)height;
    fb->colorFmt = colorFmt;
    fb->colorBuf = t;   // non-null, never dereferenced by the game
    fb->colorMask = 0xF;
    if (C3D_DEPTHTYPE_OK(depthFmt)) {
        fb->depthBuf = t;
        fb->depthFmt = C3D_DEPTHTYPE_VAL(depthFmt);
        fb->depthMask = 0x3;
    }
    t->t.ownsColor = t->t.ownsDepth = true;
    return &t->t;
}

C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, GPU_TEXFACE face, int level, C3D_DEPTHTYPE depthFmt) {
    (void)face; (void)level;
    if (!tex || !tex->data) return nullptr;
    GLuint gl = 0;
    {
        std::lock_guard<std::mutex> g(g_texLock);
        auto it = g_texByData.find((uintptr_t)tex->data);
        if (it == g_texByData.end()) return nullptr;
        it->second.target = true;
        it->second.dirty = false;
        if (!it->second.gl) glGenTextures(1, &it->second.gl);
        gl = it->second.gl;
    }
    glBindTexture(GL_TEXTURE_2D, gl);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex->width, tex->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    Target* t = newTarget(tex->width, tex->height, false, gl, C3D_DEPTHTYPE_OK(depthFmt));
    t->texData = tex->data;
    t->logicalW = tex->width;
    t->logicalH = tex->height;
    C3D_FrameBuf* fb = &t->t.frameBuf;
    fb->width = tex->width;
    fb->height = tex->height;
    fb->colorBuf = tex->data;
    fb->colorFmt = GPU_RB_RGBA8;
    fb->colorMask = 0xF;
    if (C3D_DEPTHTYPE_OK(depthFmt)) {
        fb->depthBuf = t;
        fb->depthFmt = C3D_DEPTHTYPE_VAL(depthFmt);
        fb->depthMask = 0x3;
    }
    return &t->t;
}

void C3D_RenderTargetDelete(C3D_RenderTarget* target) {
    if (!target) return;
    Target* t = (Target*)target;
    for (auto& s : g_outputs)
        for (auto& o : s)
            if (o == t) o = nullptr;
    if (S.drawOn == t) S.drawOn = nullptr;
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    if (t->depth) glDeleteRenderbuffers(1, &t->depth);
    if (t->screen && t->color) glDeleteTextures(1, &t->color);   // a texture target's colour belongs to the C3D_Tex
    g_targets.erase(std::remove(g_targets.begin(), g_targets.end(), t), g_targets.end());
    delete t;
}

void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags) {
    (void)transferFlags;
    const int s = screen == GFX_BOTTOM ? 1 : 0, d = side == GFX_RIGHT ? 1 : 0;
    if (g_outputs[s][d] && g_outputs[s][d] != (Target*)target) g_outputs[s][d]->t.linked = false;
    g_outputs[s][d] = (Target*)target;
    if (target) {
        target->linked = true;
        target->screen = screen;
        target->side = side;
        target->transferFlags = transferFlags;
    }
}

void C3D_FrameBufClear(C3D_FrameBuf* fb, C3D_ClearBits clearBits, u32 clearColor, u32 clearDepth) {
    Target* t = targetOf(fb);
    if (!t) return;
    bindTarget(t);
    GLbitfield bits = 0;
    if (clearBits & C3D_CLEAR_COLOR) {   // RGBA8: 0xRRGGBBAA
        glClearColor((clearColor >> 24) / 255.0f, ((clearColor >> 16) & 0xFF) / 255.0f, ((clearColor >> 8) & 0xFF) / 255.0f,
                     (clearColor & 0xFF) / 255.0f);
        glColorMask(1, 1, 1, 1);
        G.colorMask = 0xF;
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if ((clearBits & C3D_CLEAR_DEPTH) && t->depth) {
        glClearDepthf((clearDepth & 0xFFFFFF) / 16777215.0f);
        glDepthMask(GL_TRUE);
        G.depthMask = 1;
        bits |= GL_DEPTH_BUFFER_BIT;
#if defined(__vita__)
        glClearStencil(0);
        bits |= GL_STENCIL_BUFFER_BIT;
#endif
    }
    if (bits) glClear(bits);
    if (S.drawOn && S.drawOn != t) bindTarget(S.drawOn);
}

C3D_FrameBuf* C3D_GetFrameBuf(void) { return S.drawOn ? &S.drawOn->t.frameBuf : nullptr; }
void C3D_SetFrameBuf(C3D_FrameBuf* fb) {
    Target* t = targetOf(fb);
    if (t) { S.drawOn = t; bindTarget(t); }
}

bool C3D_FrameBegin(u8 flags) {
    (void)flags;
    if (S.inFrame) return true;
    S.inFrame = true;
    S.frameStartUs = nowUs();
    {
        std::lock_guard<std::mutex> g(g_texLock);
        if (!g_glTexToDelete.empty()) {
            glDeleteTextures((GLsizei)g_glTexToDelete.size(), g_glTexToDelete.data());
            for (GLuint id : g_glTexToDelete)
                for (auto& b : G.boundTex)
                    if (b == id) b = 0xFFFFFFFF;
            g_glTexToDelete.clear();
        }
    }
    return true;
}

bool C3D_FrameDrawOn(C3D_RenderTarget* target) {
    if (!target) return false;
    Target* t = (Target*)target;
    S.drawOn = t;
    t->drawn = true;
    bindTarget(t);
    return true;
}

void C3D_FrameSplit(u8 flags) { (void)flags; glFlush(); }

void C3D_FrameEnd(u8 flags) {
    (void)flags;
    if (!S.inFrame) return;
    const u64 t0 = nowUs();
    S.procMs = (t0 - S.frameStartUs) / 1000.0f;
    compose();
    compat::platformPresent();
    S.drawMs = (nowUs() - t0) / 1000.0f;
    S.inFrame = false;
    S.drawOn = nullptr;
    for (Target* t : g_targets) t->drawn = false;
}

void C3D_FrameEndHook(void (*hook)(void*), void* param) { (void)hook; (void)param; }

u32 C3D_CalcColorBufSize(u32 width, u32 height, GPU_COLORBUF fmt) { return width * height * (fmt == GPU_RB_RGBA8 ? 4 : fmt == GPU_RB_RGB8 ? 3 : 2); }
u32 C3D_CalcDepthBufSize(u32 width, u32 height, GPU_DEPTHBUF fmt) { return width * height * (fmt == GPU_RB_DEPTH16 ? 2 : fmt == GPU_RB_DEPTH24 ? 3 : 4); }

// ------------------------------------------------------------------ compat-only helpers
// Reads a render-target texture back into its C3D_Tex data in PICA layout (RGBA8, tiled, row 0 = top), for game
// code that inspects rendered pixels on the CPU (portrait calibration). Waits for the GPU: use rarely.
void C3D_CompatReadback(C3D_Tex* tex) {
    if (!tex || !tex->data) return;
    Target* t = nullptr;
    for (Target* x : g_targets)
        if (x->texData == tex->data) t = x;
    if (!t) return;
    const int w = t->w, h = t->h;
    std::vector<u8> px((size_t)w * h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (S.drawOn) bindTarget(S.drawOn);
    else glBindFramebuffer(GL_FRAMEBUFFER, compat::displayFramebuffer());
    u8* dst = (u8*)tex->data;
    const int tilesX = w / 8;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const u8* s = &px[((size_t)(h - 1 - y) * w + x) * 4];   // GL row 0 is the bottom
            u8* d = dst + ((size_t)((y >> 3) * tilesX + (x >> 3)) * 64 + morton8(x & 7, y & 7)) * 4;
            d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0];
        }
}

// The shbin replacement (see pica2glsl.py)
DVLB_s* DVLB_ParseFile(u32* shbinData, u32 shbinSize) {
    if (shbinSize != 0xC0DE5A7Eu || !shbinData) return nullptr;
    return (DVLB_s*)shbinData;
}
void DVLB_Free(DVLB_s* dvlb) { (void)dvlb; }
Result shaderProgramInit(shaderProgram_s* sp) {
    if (!sp) return -1;
    memset(sp, 0, sizeof(*sp));
    return 0;
}
Result shaderProgramFree(shaderProgram_s* sp) {
    if (!sp) return -1;
    delete sp->vertexShader;
    sp->vertexShader = nullptr;
    return 0;
}
Result shaderProgramSetVsh(shaderProgram_s* sp, DVLE_s* dvle) {
    if (!sp || !dvle) return -1;
    if (!sp->vertexShader) sp->vertexShader = new shaderInstance_s();
    sp->vertexShader->dvle = dvle;
    return 0;
}
s8 shaderInstanceGetUniformLocation(shaderInstance_s* si, const char* name) {
    if (!si || !si->dvle || !name) return -1;
    for (int i = 0; i < si->dvle->numUniforms; ++i)
        if (!strcmp(si->dvle->uniforms[i].name, name)) return si->dvle->uniforms[i].index;
    return -1;
}

Result GSPGPU_FlushDataCache(const void* adr, u32 size) {
    markDirtyRange(adr, size);
    return 0;
}
Result GSPGPU_InvalidateDataCache(const void* adr, u32 size) { (void)adr; (void)size; return 0; }

}  // extern "C"

// Shows the last composed frame again (the render targets keep their pixels), for platform code that holds the
// game inside a blocking call such as the on-screen keyboard (platform_vita.cpp). The caller presents it.
namespace compat {
void recompose() {
    compose();
    if (S.drawOn) bindTarget(S.drawOn);
}
}  // namespace compat
