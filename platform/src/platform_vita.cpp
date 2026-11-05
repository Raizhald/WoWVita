// platform_vita.cpp — the PS Vita platform: vitaGL display, SceCtrl / SceTouch input, SceAudioOut, the IME dialog.
//
// Controls (the 3DS buttons the game reads):
//   left stick -> Circle Pad          right stick -> C-Stick         D-pad, L, R, START, SELECT -> the same
//   Cross / Circle -> A / B (swapped when the system's enter button is Circle, as on Japanese consoles)
//   Triangle -> X   Square -> Y       rear touch panel, left / right half -> ZL / ZR
//   front touch on the touch screen's rectangle (layout.cpp) -> the 3DS touch screen
//   two fingers on the front panel -> the next screen layout (layout.cpp: one screen + touch panel, side by side,
//   big top screen + small touch screen)
#include <3ds.h>

#include <psp2/apputil.h>
#include <psp2/audioout.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "audio_out.h"
#include "platform.h"

// newlib's heap (malloc / new): the game's "linear" and "VRAM" allocations live here too (ctru_common.cpp budgets
// 160 + 64 MB) next to its own heap. vitaGL keeps its textures and buffers in CDRAM and its own RAM blocks outside
// this. ATTRIBUTE2=12 in the SFO (CMakeLists.txt) unlocks the extended memory budget this needs.
#ifndef W3D_VITA_HEAP_MB
#define W3D_VITA_HEAP_MB 280
#endif
extern "C" {
unsigned int _newlib_heap_size_user = W3D_VITA_HEAP_MB * 1024 * 1024;
}

namespace {

const int kW = 960, kH = 544;
bool g_enterCircle = false;   // the system's enter button (SCE_SYSTEM_PARAM_ID_ENTER_BUTTON)

// ------------------------------------------------------------------ input state
u32 g_held = 0, g_prevHeld = 0;
bool g_touch = false;
int g_tx = 0, g_ty = 0;
int g_lx = 0, g_ly = 0, g_rx = 0, g_ry = 0;   // sticks in 3DS units (about -156..156, y up)
int g_frontFingers = 0;
SceTouchPanelInfo g_front{}, g_rear{};
u64 g_lastPowerTickUs = 0;

// A Vita stick axis (0..255, 128 = centre) -> the 3DS Circle Pad's range (+-156), with a dead zone.
int axis(int v) {
    const int d = v - 128, dead = 20;
    if (d > -dead && d < dead) return 0;
    const int m = d > 0 ? d - dead : d + dead;
    int out = m * 156 / (127 - dead);
    return out > 156 ? 156 : out < -156 ? -156 : out;
}

// Touch report -> display pixels (front) or 0..1 across the panel (rear)
int frontX(int x) { return (x - g_front.minAaX) * kW / (g_front.maxAaX - g_front.minAaX + 1); }
int frontY(int y) { return (y - g_front.minAaY) * kH / (g_front.maxAaY - g_front.minAaY + 1); }

void readInput() {
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);
    const unsigned b = pad.buttons;
    u32 k = 0;
    if (b & SCE_CTRL_UP) k |= KEY_DUP;
    if (b & SCE_CTRL_DOWN) k |= KEY_DDOWN;
    if (b & SCE_CTRL_LEFT) k |= KEY_DLEFT;
    if (b & SCE_CTRL_RIGHT) k |= KEY_DRIGHT;
    if (b & SCE_CTRL_CROSS) k |= g_enterCircle ? KEY_B : KEY_A;
    if (b & SCE_CTRL_CIRCLE) k |= g_enterCircle ? KEY_A : KEY_B;
    if (b & SCE_CTRL_TRIANGLE) k |= KEY_X;
    if (b & SCE_CTRL_SQUARE) k |= KEY_Y;
    if (b & SCE_CTRL_LTRIGGER) k |= KEY_L;
    if (b & SCE_CTRL_RTRIGGER) k |= KEY_R;
    if (b & SCE_CTRL_START) k |= KEY_START;
    if (b & SCE_CTRL_SELECT) k |= KEY_SELECT;

    g_lx = axis(pad.lx);
    g_ly = -axis(pad.ly);
    g_rx = axis(pad.rx);
    g_ry = -axis(pad.ry);
    // the direction bits libctru sets from the analogue values
    const int th = 41;
    if (g_lx >= th) k |= KEY_CPAD_RIGHT;
    if (g_lx <= -th) k |= KEY_CPAD_LEFT;
    if (g_ly >= th) k |= KEY_CPAD_UP;
    if (g_ly <= -th) k |= KEY_CPAD_DOWN;
    if (g_rx >= th) k |= KEY_CSTICK_RIGHT;
    if (g_rx <= -th) k |= KEY_CSTICK_LEFT;
    if (g_ry >= th) k |= KEY_CSTICK_UP;
    if (g_ry <= -th) k |= KEY_CSTICK_DOWN;

    // rear panel: ZL / ZR
    SceTouchData rear;
    memset(&rear, 0, sizeof rear);
    sceTouchPeek(SCE_TOUCH_PORT_BACK, &rear, 1);
    const int rearMid = (g_rear.minAaX + g_rear.maxAaX) / 2;
    for (unsigned i = 0; i < rear.reportNum && i < 8; ++i) k |= rear.report[i].x < rearMid ? KEY_ZL : KEY_ZR;

    // front panel: the touch screen; two fingers switch the layout
    SceTouchData front;
    memset(&front, 0, sizeof front);
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &front, 1);
    const int fingers = (int)front.reportNum;
    if (fingers >= 2 && g_frontFingers < 2) compat::layoutNextMode();
    g_frontFingers = fingers;
    int tx = 0, ty = 0;
    g_touch = compat::layoutTouch(fingers == 1, fingers == 1 ? frontX(front.report[0].x) : 0,
                                  fingers == 1 ? frontY(front.report[0].y) : 0, &tx, &ty);
    if (g_touch) {
        g_tx = tx;
        g_ty = ty;
        k |= KEY_TOUCH;
    }

    g_prevHeld = g_held;
    g_held = k;

    // keep the console from suspending itself while the game runs (it would drop the connection)
    const u64 now = sceKernelGetProcessTimeWide();
    if (now - g_lastPowerTickUs > 1000000) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        g_lastPowerTickUs = now;
    }
}

// ------------------------------------------------------------------ UTF-8 <-> UTF-16 (the IME's text)
void utf8To16(const char* s, SceWChar16* out, size_t cap) {
    size_t n = 0;
    const unsigned char* p = (const unsigned char*)(s ? s : "");
    while (*p && n + 1 < cap) {
        u32 c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
        if (c >= 0x10000) {
            if (n + 2 >= cap) break;
            c -= 0x10000;
            out[n++] = (SceWChar16)(0xD800 | (c >> 10));
            out[n++] = (SceWChar16)(0xDC00 | (c & 0x3FF));
        } else {
            out[n++] = (SceWChar16)c;
        }
    }
    out[n] = 0;
}

size_t utf16To8(const SceWChar16* s, char* out, size_t cap) {
    size_t n = 0;
    for (; *s; ++s) {
        u32 c = *s;
        if (c >= 0xD800 && c < 0xDC00 && s[1] >= 0xDC00 && s[1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (s[1] - 0xDC00); ++s; }
        char b[4];
        int len;
        if (c < 0x80) { b[0] = (char)c; len = 1; }
        else if (c < 0x800) { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 63)); len = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 63)); b[2] = (char)(0x80 | (c & 63)); len = 3; }
        else { b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 63)); b[2] = (char)(0x80 | ((c >> 6) & 63)); b[3] = (char)(0x80 | (c & 63)); len = 4; }
        if (n + len >= cap) break;
        memcpy(out + n, b, len);
        n += len;
    }
    if (cap) out[n] = 0;
    return n;
}

// ------------------------------------------------------------------ audio: one BGM port, a thread feeding it
constexpr int kAudioRate = 48000, kAudioGrain = 1024;   // 21 ms per output call
compat::AudioRender g_render = nullptr;
volatile bool g_audioRun = false;
SceUID g_audioThread = -1;
int g_audioPort = -1;

int audioThread(SceSize args, void* argp) {
    (void)args; (void)argp;
    static s16 buf[2][kAudioGrain * 2];
    int i = 0;
    while (g_audioRun) {
        g_render(buf[i], kAudioGrain, kAudioRate);
        sceAudioOutOutput(g_audioPort, buf[i]);   // blocks until the previous buffer was played
        i ^= 1;
    }
    return 0;
}

}  // namespace

namespace compat {

bool platformInitGL() {
    // clocks: the game is CPU-bound on the 3DS too
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // common dialogs (the IME) want the app utility and its system language / enter button
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof init);
    memset(&boot, 0, sizeof boot);
    sceAppUtilInit(&init, &boot);
    SceCommonDialogConfigParam cfg;
    sceCommonDialogConfigParamInit(&cfg);
    int lang = 0, enter = 0;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
    cfg.language = (SceSystemParamLang)lang;
    cfg.enterButtonAssign = (SceSystemParamEnterButtonAssign)enter;
    sceCommonDialogSetConfigParam(&cfg);
    g_enterCircle = enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &g_front);
    sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &g_rear);
    if (g_front.maxAaX <= g_front.minAaX) { g_front.minAaX = 0; g_front.maxAaX = 1919; g_front.minAaY = 0; g_front.maxAaY = 1087; }
    if (g_rear.maxAaX <= g_rear.minAaX) { g_rear.minAaX = 0; g_rear.maxAaX = 1919; }

    // 16 MB of RAM for vitaGL's own use once CDRAM runs out
    if (!vglInitExtended(0, kW, kH, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE)) return false;
    return true;
}

void platformShutdownGL() { vglEnd(); }

int displayWidth() { return kW; }
int displayHeight() { return kH; }
GLuint displayFramebuffer() { return 0; }

void platformPresent() { vglSwapBuffers(GL_FALSE); }

bool audioStart(AudioRender render) {
    g_render = render;
    g_audioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, kAudioGrain, kAudioRate, SCE_AUDIO_OUT_MODE_STEREO);
    if (g_audioPort < 0) return false;
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(g_audioPort, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    g_audioRun = true;
    // above the game's threads (lower number = higher priority) so the mixer is never starved
    g_audioThread = sceKernelCreateThread("w3d_audio", audioThread, 0x10000100 - 20, 0x10000, 0, 0, nullptr);
    if (g_audioThread < 0 || sceKernelStartThread(g_audioThread, 0, nullptr) < 0) {
        g_audioRun = false;
        sceAudioOutReleasePort(g_audioPort);
        g_audioPort = -1;
        return false;
    }
    return true;
}

void audioStop() {
    if (!g_audioRun) return;
    g_audioRun = false;
    sceKernelWaitThreadEnd(g_audioThread, nullptr, nullptr);
    sceKernelDeleteThread(g_audioThread);
    sceAudioOutReleasePort(g_audioPort);
    g_audioThread = g_audioPort = -1;
}

}  // namespace compat

// ------------------------------------------------------------------ hid / apt / swkbd
extern "C" {

Handle hidMemHandle = 0;
Result hidInit(void) { return 0; }
void hidExit(void) {}
void hidSetRepeatParameters(u32 delay, u32 interval) { (void)delay; (void)interval; }

void hidScanInput(void) { readInput(); }
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
    pos->dx = (s16)g_lx;
    pos->dy = (s16)g_ly;
}
void hidCstickRead(circlePosition* pos) {
    if (!pos) return;
    pos->dx = (s16)g_rx;
    pos->dy = (s16)g_ry;
}
void hidAccelRead(accelVector* vector) { if (vector) memset(vector, 0, sizeof *vector); }
void hidGyroRead(angularRate* rate) { if (rate) memset(rate, 0, sizeof *rate); }

// The PS button and suspending are the system's business; the game ends itself (START -> quit).
bool aptMainLoop(void) { return true; }

void swkbdInit(SwkbdState* swkbd, SwkbdType type, int numButtons, int maxTextLength) {
    memset(swkbd, 0, sizeof *swkbd);
    swkbd->type = type;
    swkbd->num_buttons_m1 = numButtons - 1;
    swkbd->max_text_len = maxTextLength > 0 ? (u16)maxTextLength : 255;
}
void swkbdSetFeatures(SwkbdState* swkbd, u32 features) { (void)swkbd; (void)features; }
void swkbdSetHintText(SwkbdState* swkbd, const char* text) {
    utf8To16(text, (SceWChar16*)swkbd->hint_text, SWKBD_MAX_HINT_TEXT_LEN + 1);
}
void swkbdSetInitialText(SwkbdState* swkbd, const char* text) { swkbd->extra.initial_text = text; }

// The 3DS keyboard is a blocking applet: so is this. The IME dialog runs over the last frame of the game.
SwkbdButton swkbdInputText(SwkbdState* swkbd, char* buf, size_t bufsize) {
    if (!buf || !bufsize) return SWKBD_BUTTON_NONE;
    buf[0] = 0;
    static SceWChar16 title[SWKBD_MAX_HINT_TEXT_LEN + 1], initial[256], input[256];
    memcpy(title, swkbd->hint_text, sizeof title);
    utf8To16(swkbd->extra.initial_text, initial, sizeof initial / sizeof initial[0]);
    memset(input, 0, sizeof input);
    u32 maxLen = swkbd->max_text_len;
    if (maxLen > 255) maxLen = 255;
    if (maxLen >= bufsize) maxLen = (u32)bufsize - 1;

    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = 0;
    p.languagesForced = SCE_FALSE;
    p.type = SCE_IME_TYPE_BASIC_LATIN;
    p.option = 0;
    p.textBoxMode = swkbd->password_mode != SWKBD_PASSWORD_NONE ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD
                                                                : SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    p.title = title;
    p.maxTextLength = maxLen;
    p.initialText = initial;
    p.inputTextBuffer = input;
    if (sceImeDialogInit(&p) < 0) return SWKBD_BUTTON_NONE;

    bool ok = false;
    for (;;) {
        const SceCommonDialogStatus st = sceImeDialogGetStatus();
        if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
            SceImeDialogResult r;
            memset(&r, 0, sizeof r);
            sceImeDialogGetResult(&r);
            ok = r.button == SCE_IME_DIALOG_BUTTON_ENTER;
            break;
        }
        if (st != SCE_COMMON_DIALOG_STATUS_RUNNING) break;
        compat::recompose();
        vglSwapBuffers(GL_TRUE);   // GL_TRUE: draw the common dialog over the frame
    }
    sceImeDialogTerm();
    // the buttons held when the dialog closed must not reach the game as fresh presses
    readInput();
    g_prevHeld = g_held;

    if (!ok) {
        memset(input, 0, sizeof input);
        return SWKBD_BUTTON_LEFT;
    }
    utf16To8(input, buf, bufsize);
    memset(input, 0, sizeof input);
    if (swkbd->valid_input == SWKBD_NOTEMPTY_NOTBLANK || swkbd->valid_input == SWKBD_NOTEMPTY ||
        swkbd->valid_input == SWKBD_NOTBLANK) {
        bool blank = true;
        for (const char* c = buf; *c; ++c)
            if (*c != ' ' && *c != '\t') { blank = false; break; }
        if (blank && (buf[0] || swkbd->valid_input != SWKBD_NOTBLANK)) {
            buf[0] = 0;
            return SWKBD_BUTTON_LEFT;
        }
    }
    return SWKBD_BUTTON_RIGHT;
}

}  // extern "C"
