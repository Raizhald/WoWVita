// platform_pc.cpp — the PC test platform: headless OpenGL ES (EGL, Mesa llvmpipe), scripted input, silent audio.
//
// Lets the Vita build's compatibility layer run the whole game on a Linux PC without a window:
//   W3D_FRAMES=N        quit after N frames (aptMainLoop returns false)
//   W3D_SHOTS=a,b,...   write the display (960x544) after these frames to $W3D_SHOT_DIR/frame_<n>.ppm
//   W3D_INPUT=f:KEYS;.. hold buttons from frame f (KEYS = A+B+DUP..., "-" releases all), e.g. "100:A;104:-"
//   W3D_TOUCH=f:x,y;..  touch the touch screen at (x,y) from frame f ("f:-" lifts)
//   W3D_LAYOUT=0|1      screen layout (see layout.cpp)
#include <3ds.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "audio_out.h"
#include "platform.h"

namespace {

EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLContext g_ctx = EGL_NO_CONTEXT;
GLuint g_fbo = 0, g_color = 0;
const int kW = 960, kH = 544;
long g_frame = 0;
long g_maxFrames = -1;
std::vector<long> g_shots;

struct Ev { long frame; u32 keys; bool keysSet; int tx, ty; bool touchSet; bool touch; };
std::vector<Ev> g_events;
u32 g_held = 0, g_prevHeld = 0;
bool g_touch = false;
int g_tx = 0, g_ty = 0;

u32 keyByName(const std::string& n) {
    static const struct { const char* n; u32 k; } t[] = {
        {"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y}, {"L", KEY_L}, {"R", KEY_R}, {"ZL", KEY_ZL}, {"ZR", KEY_ZR},
        {"START", KEY_START}, {"SELECT", KEY_SELECT}, {"DUP", KEY_DUP}, {"DDOWN", KEY_DDOWN}, {"DLEFT", KEY_DLEFT},
        {"DRIGHT", KEY_DRIGHT}, {"CUP", KEY_CPAD_UP}, {"CDOWN", KEY_CPAD_DOWN}, {"CLEFT", KEY_CPAD_LEFT}, {"CRIGHT", KEY_CPAD_RIGHT}};
    for (auto& e : t)
        if (n == e.n) return e.k;
    return 0;
}

void parseScripts() {
    if (const char* f = getenv("W3D_FRAMES")) g_maxFrames = atol(f);
    if (const char* s = getenv("W3D_SHOTS"))
        for (const char* p = s; *p;) {
            g_shots.push_back(strtol(p, (char**)&p, 10));
            if (*p == ',') ++p; else break;
        }
    if (const char* s = getenv("W3D_INPUT")) {
        std::string in(s);
        size_t i = 0;
        while (i < in.size()) {
            size_t j = in.find(';', i);
            std::string item = in.substr(i, j == std::string::npos ? std::string::npos : j - i);
            i = j == std::string::npos ? in.size() : j + 1;
            size_t c = item.find(':');
            if (c == std::string::npos) continue;
            Ev e{atol(item.c_str()), 0, true, 0, 0, false, false};
            std::string ks = item.substr(c + 1);
            size_t k = 0;
            while (ks != "-" && k < ks.size()) {
                size_t pl = ks.find('+', k);
                e.keys |= keyByName(ks.substr(k, pl == std::string::npos ? std::string::npos : pl - k));
                k = pl == std::string::npos ? ks.size() : pl + 1;
            }
            g_events.push_back(e);
        }
    }
    if (const char* s = getenv("W3D_TOUCH")) {
        std::string in(s);
        size_t i = 0;
        while (i < in.size()) {
            size_t j = in.find(';', i);
            std::string item = in.substr(i, j == std::string::npos ? std::string::npos : j - i);
            i = j == std::string::npos ? in.size() : j + 1;
            size_t c = item.find(':');
            if (c == std::string::npos) continue;
            Ev e{atol(item.c_str()), 0, false, 0, 0, true, false};
            std::string v = item.substr(c + 1);
            if (v != "-") {
                e.touch = true;
                sscanf(v.c_str(), "%d,%d", &e.tx, &e.ty);
            }
            g_events.push_back(e);
        }
    }
    if (const char* l = getenv("W3D_LAYOUT")) compat::layoutSetMode(atoi(l));
}

void writeShot(long frame) {
    std::vector<unsigned char> px((size_t)kW * kH * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const char* dir = getenv("W3D_SHOT_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%ld.ppm", dir && *dir ? dir : ".", frame);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", kW, kH);
    for (int y = kH - 1; y >= 0; --y)
        for (int x = 0; x < kW; ++x) fwrite(&px[((size_t)y * kW + x) * 4], 1, 3, f);
    fclose(f);
    fprintf(stderr, "compat: wrote %s\n", path);
}

}  // namespace

namespace compat {

bool platformInitGL() {
    parseScripts();
    auto gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint ma, mi;
    if (!eglInitialize(g_dpy, &ma, &mi)) { fprintf(stderr, "compat: eglInitialize failed\n"); return false; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    eglChooseConfig(g_dpy, ca, &cfg, 1, &n);
    EGLint cx[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    g_ctx = eglCreateContext(g_dpy, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cx);
    if (g_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_ctx)) {
        fprintf(stderr, "compat: no GL context (0x%x)\n", eglGetError());
        return false;
    }
    fprintf(stderr, "compat: %s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glGenFramebuffers(1, &g_fbo);
    glGenTextures(1, &g_color);
    glBindTexture(GL_TEXTURE_2D, g_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void platformShutdownGL() {
    if (g_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_ctx != EGL_NO_CONTEXT) eglDestroyContext(g_dpy, g_ctx);
        eglTerminate(g_dpy);
    }
    g_dpy = EGL_NO_DISPLAY;
}

int displayWidth() { return kW; }
int displayHeight() { return kH; }
GLuint displayFramebuffer() { return g_fbo; }

void platformPresent() {
    ++g_frame;
    for (long s : g_shots)
        if (s == g_frame) writeShot(g_frame);
    if (g_frame % 100 == 0) fprintf(stderr, "compat: frame %ld\n", g_frame);
}

// ------------------------------------------------------------------ silent audio: render in real time, discard
static std::atomic<bool> g_audioRun{false};
static std::thread g_audioThread;
bool audioStart(AudioRender render) {
    g_audioRun = true;
    g_audioThread = std::thread([render] {
        std::vector<s16> buf(2 * 1024);
        while (g_audioRun) {
            render(buf.data(), 1024, 48000);
            std::this_thread::sleep_for(std::chrono::microseconds(1024 * 1000000 / 48000));
        }
    });
    return true;
}
void audioStop() {
    g_audioRun = false;
    if (g_audioThread.joinable()) g_audioThread.join();
}

}  // namespace compat

// ------------------------------------------------------------------ hid / apt / swkbd
extern "C" {

Handle hidMemHandle = 0;
Result hidInit(void) { return 0; }
void hidExit(void) {}
void hidSetRepeatParameters(u32 delay, u32 interval) { (void)delay; (void)interval; }

void hidScanInput(void) {
    g_prevHeld = g_held;
    for (const Ev& e : g_events)
        if (e.frame == g_frame) {
            if (e.keysSet) g_held = e.keys;
            if (e.touchSet) { g_touch = e.touch; g_tx = e.tx; g_ty = e.ty; }
        }
    if (g_touch) g_held |= KEY_TOUCH; else g_held &= ~KEY_TOUCH;
}
u32 hidKeysHeld(void) { return g_held; }
u32 hidKeysDown(void) { return g_held & ~g_prevHeld; }
u32 hidKeysDownRepeat(void) { return hidKeysDown(); }
u32 hidKeysUp(void) { return g_prevHeld & ~g_held; }
void hidTouchRead(touchPosition* pos) {
    if (!pos) return;
    pos->px = g_touch ? (u16)g_tx : 0;
    pos->py = g_touch ? (u16)g_ty : 0;
}
void hidCircleRead(circlePosition* pos) {
    if (!pos) return;
    pos->dx = (g_held & KEY_CPAD_RIGHT) ? 156 : (g_held & KEY_CPAD_LEFT) ? -156 : 0;
    pos->dy = (g_held & KEY_CPAD_UP) ? 156 : (g_held & KEY_CPAD_DOWN) ? -156 : 0;
}
void hidCstickRead(circlePosition* pos) {
    if (pos) pos->dx = pos->dy = 0;
}
void hidAccelRead(accelVector* vector) { if (vector) memset(vector, 0, sizeof *vector); }
void hidGyroRead(angularRate* rate) { if (rate) memset(rate, 0, sizeof *rate); }

bool aptMainLoop(void) { return g_maxFrames < 0 || g_frame < g_maxFrames; }

void swkbdInit(SwkbdState* swkbd, SwkbdType type, int numButtons, int maxTextLength) {
    memset(swkbd, 0, sizeof *swkbd);
    (void)type; (void)numButtons; (void)maxTextLength;
}
void swkbdSetFeatures(SwkbdState* swkbd, u32 features) { (void)swkbd; (void)features; }
void swkbdSetHintText(SwkbdState* swkbd, const char* text) { (void)swkbd; (void)text; }
void swkbdSetInitialText(SwkbdState* swkbd, const char* text) { (void)swkbd; (void)text; }
SwkbdButton swkbdInputText(SwkbdState* swkbd, char* buf, size_t bufsize) {
    (void)swkbd;
    if (buf && bufsize) buf[0] = 0;
    return SWKBD_BUTTON_NONE;
}

}  // extern "C"
