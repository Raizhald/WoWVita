// ndsp.cpp — the 3DS DSP sound API (libctru NDSP) as a software mixer.
//
// 24 channels of PCM8 / PCM16, mono or stereo, any rate (linear interpolation), wave buffer queues with the same
// status protocol as libctru (QUEUED -> PLAYING -> DONE; looping buffers repeat), front left / right mix levels.
// The mixed stereo stream goes to the platform's audio output (platform_*.cpp: compat::audioStart).
#include <3ds.h>

#include <cmath>
#include <cstring>
#include <mutex>

#include "audio_out.h"

namespace {

struct Chn {
    u16 format = NDSP_FORMAT_PCM16;
    float rate = 1.0f;
    ndspInterpType interp = NDSP_INTERP_LINEAR;
    float mix[12] = {1, 1};
    ndspWaveBuf* head = nullptr;
    double pos = 0;          // sample position in the head buffer
    bool playing = false;
    bool paused = false;
    u16 seq = 0;
};

std::mutex g_lock;
Chn g_chn[24];
float g_master = 1.0f;
bool g_up = false;
u32 g_frames = 0;
ndspCallback g_cb = nullptr;
void* g_cbData = nullptr;

inline float sampleAt(const ndspWaveBuf* b, u16 fmt, u32 i, int c) {
    const int chans = (fmt & 3) == 2 ? 2 : 1;
    const bool pcm8 = ((fmt >> 2) & 3) == NDSP_ENCODING_PCM8;
    if (c >= chans) c = chans - 1;
    if (pcm8) return b->data_pcm8[i * chans + c] / 128.0f;
    return b->data_pcm16[i * chans + c] / 32768.0f;
}

// Mixes `frames` stereo frames at `outRate` Hz into out (interleaved float), adding.
void mixChannel(Chn& ch, float* out, int frames, int outRate) {
    if (!ch.playing || ch.paused) return;
    const double step = ch.rate / outRate;
    const bool stereo = (ch.format & 3) == 2;
    const float gl = ch.mix[0] * g_master, gr = ch.mix[1] * g_master;
    for (int f = 0; f < frames; ++f) {
        ndspWaveBuf* b = ch.head;
        while (b && ch.pos >= b->nsamples) {
            ch.pos -= b->nsamples;
            if (b->looping) continue;
            b->status = NDSP_WBUF_DONE;
            b = ch.head = b->next;
            if (b) b->status = NDSP_WBUF_PLAYING;
        }
        if (!b) {
            ch.playing = false;
            ch.pos = 0;
            return;
        }
        const u32 i = (u32)ch.pos;
        const float t = ch.interp == NDSP_INTERP_NONE ? 0.0f : (float)(ch.pos - i);
        const u32 j = i + 1 < b->nsamples ? i + 1 : (b->looping ? 0 : i);
        float l = sampleAt(b, ch.format, i, 0), r = stereo ? sampleAt(b, ch.format, i, 1) : l;
        if (t > 0) {
            const float l2 = sampleAt(b, ch.format, j, 0), r2 = stereo ? sampleAt(b, ch.format, j, 1) : l2;
            l += (l2 - l) * t;
            r += (r2 - r) * t;
        }
        out[2 * f] += l * gl;
        out[2 * f + 1] += r * gr;
        ch.pos += step;
    }
}

float g_mixBuf[2 * 4096];

void render(s16* out, int frames, int rate) {
    if (frames > 4096) frames = 4096;
    memset(g_mixBuf, 0, sizeof(float) * 2 * frames);
    ndspCallback cb = nullptr;
    void* cbData = nullptr;
    {
        std::lock_guard<std::mutex> g(g_lock);
        for (Chn& c : g_chn) mixChannel(c, g_mixBuf, frames, rate);
        g_frames += (u32)frames / 160;
        cb = g_cb;
        cbData = g_cbData;
    }
    for (int i = 0; i < 2 * frames; ++i) {
        float v = g_mixBuf[i];
        v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
        out[i] = (s16)(v * 32767.0f);
    }
    if (cb) cb(cbData);
}

}  // namespace

extern "C" {

void ndspUseComponent(const void* binary, u32 size, u16 progMask, u16 dataMask) { (void)binary; (void)size; (void)progMask; (void)dataMask; }

Result ndspInit(void) {
    if (g_up) return 0;
    for (int i = 0; i < 24; ++i) ndspChnReset(i);
    if (!compat::audioStart(render)) return -1;
    g_up = true;
    return 0;
}

void ndspExit(void) {
    if (!g_up) return;
    compat::audioStop();
    g_up = false;
}

u32 ndspGetDroppedFrames(void) { return 0; }
u32 ndspGetFrameCount(void) { return g_frames; }
void ndspSetMasterVol(float volume) { g_master = volume; }
float ndspGetMasterVol(void) { return g_master; }
void ndspSetOutputMode(ndspOutputMode mode) { (void)mode; }
void ndspSetClippingMode(ndspClippingMode mode) { (void)mode; }
void ndspSetOutputCount(int count) { (void)count; }
int ndspGetOutputCount(void) { return 1; }
void ndspSetCapture(ndspWaveBuf* capture) { (void)capture; }
void ndspSetCallback(ndspCallback callback, void* data) {
    std::lock_guard<std::mutex> g(g_lock);
    g_cb = callback;
    g_cbData = data;
}
void ndspSurroundSetDepth(u16 depth) { (void)depth; }
u16 ndspSurroundGetDepth(void) { return 0; }
void ndspSurroundSetPos(ndspSpeakerPos pos) { (void)pos; }
void ndspSurroundSetRearRatio(u16 ratio) { (void)ratio; }
u16 ndspSurroundGetRearRatio(void) { return 0; }
void ndspAuxSetEnable(int id, bool enable) { (void)id; (void)enable; }
bool ndspAuxIsEnabled(int id) { (void)id; return false; }
void ndspAuxSetFrontBypass(int id, bool bypass) { (void)id; (void)bypass; }
bool ndspAuxGetFrontBypass(int id) { (void)id; return false; }
void ndspAuxSetVolume(int id, float volume) { (void)id; (void)volume; }
float ndspAuxGetVolume(int id) { (void)id; return 0; }
void ndspAuxSetCallback(int id, ndspAuxCallback callback, void* data) { (void)id; (void)callback; (void)data; }

static void clearQueue(Chn& c) {
    while (c.head) {
        c.head->status = NDSP_WBUF_DONE;
        c.head = c.head->next;
    }
    c.playing = false;
    c.pos = 0;
    c.seq = 0;
}

void ndspChnReset(int id) {
    std::lock_guard<std::mutex> g(g_lock);
    Chn& c = g_chn[id % 24];
    clearQueue(c);
    c.paused = false;
    c.format = NDSP_FORMAT_PCM16;
    c.interp = NDSP_INTERP_POLYPHASE;
    c.rate = 1.0f;
    memset(c.mix, 0, sizeof c.mix);
    c.mix[0] = c.mix[1] = 1.0f;
}
void ndspChnInitParams(int id) {
    std::lock_guard<std::mutex> g(g_lock);
    Chn& c = g_chn[id % 24];
    c.format = NDSP_FORMAT_PCM16;
    c.rate = 1.0f;
    memset(c.mix, 0, sizeof c.mix);
    c.mix[0] = c.mix[1] = 1.0f;
}
bool ndspChnIsPlaying(int id) {
    std::lock_guard<std::mutex> g(g_lock);
    return g_chn[id % 24].playing;
}
u32 ndspChnGetSamplePos(int id) {
    std::lock_guard<std::mutex> g(g_lock);
    return (u32)g_chn[id % 24].pos;
}
u16 ndspChnGetWaveBufSeq(int id) {
    std::lock_guard<std::mutex> g(g_lock);
    const Chn& c = g_chn[id % 24];
    return c.head ? c.head->sequence_id : 0;
}
bool ndspChnIsPaused(int id) { return g_chn[id % 24].paused; }
void ndspChnSetPaused(int id, bool paused) {
    std::lock_guard<std::mutex> g(g_lock);
    g_chn[id % 24].paused = paused;
}
void ndspChnSetFormat(int id, u16 format) {
    std::lock_guard<std::mutex> g(g_lock);
    g_chn[id % 24].format = format;
}
u16 ndspChnGetFormat(int id) { return g_chn[id % 24].format; }
void ndspChnSetInterp(int id, ndspInterpType type) {
    std::lock_guard<std::mutex> g(g_lock);
    g_chn[id % 24].interp = type;
}
ndspInterpType ndspChnGetInterp(int id) { return g_chn[id % 24].interp; }
void ndspChnSetRate(int id, float rate) {
    std::lock_guard<std::mutex> g(g_lock);
    g_chn[id % 24].rate = rate;
}
float ndspChnGetRate(int id) { return g_chn[id % 24].rate; }
void ndspChnSetMix(int id, float mix[12]) {
    std::lock_guard<std::mutex> g(g_lock);
    memcpy(g_chn[id % 24].mix, mix, sizeof(float) * 12);
}
void ndspChnGetMix(int id, float mix[12]) {
    std::lock_guard<std::mutex> g(g_lock);
    memcpy(mix, g_chn[id % 24].mix, sizeof(float) * 12);
}
void ndspChnSetAdpcmCoefs(int id, u16 coefs[16]) { (void)id; (void)coefs; }

void ndspChnWaveBufClear(int id) {
    std::lock_guard<std::mutex> g(g_lock);
    clearQueue(g_chn[id % 24]);
}

void ndspChnWaveBufAdd(int id, ndspWaveBuf* buf) {
    if (!buf || !buf->nsamples) return;
    std::lock_guard<std::mutex> g(g_lock);
    if (buf->status == NDSP_WBUF_QUEUED || buf->status == NDSP_WBUF_PLAYING) return;
    Chn& c = g_chn[id % 24];
    buf->next = nullptr;
    buf->sequence_id = ++c.seq;
    if (!c.head) {
        c.head = buf;
        c.pos = 0;
        buf->status = NDSP_WBUF_PLAYING;
    } else {
        ndspWaveBuf* t = c.head;
        while (t->next) t = t->next;
        t->next = buf;
        buf->status = NDSP_WBUF_QUEUED;
    }
    c.playing = true;
}

void ndspChnIirMonoSetEnable(int id, bool enable) { (void)id; (void)enable; }
bool ndspChnIirMonoSetParamsCustomFilter(int id, float a0, float a1, float b0) { (void)id; (void)a0; (void)a1; (void)b0; return true; }
bool ndspChnIirMonoSetParamsLowPassFilter(int id, float f0) { (void)id; (void)f0; return true; }
bool ndspChnIirMonoSetParamsHighPassFilter(int id, float f0) { (void)id; (void)f0; return true; }
void ndspChnIirBiquadSetEnable(int id, bool enable) { (void)id; (void)enable; }
bool ndspChnIirBiquadSetParamsCustomFilter(int id, float a0, float a1, float a2, float b0, float b1, float b2) {
    (void)id; (void)a0; (void)a1; (void)a2; (void)b0; (void)b1; (void)b2;
    return true;
}
bool ndspChnIirBiquadSetParamsLowPassFilter(int id, float f0, float Q) { (void)id; (void)f0; (void)Q; return true; }
bool ndspChnIirBiquadSetParamsHighPassFilter(int id, float f0, float Q) { (void)id; (void)f0; (void)Q; return true; }
bool ndspChnIirBiquadSetParamsBandPassFilter(int id, float f0, float Q) { (void)id; (void)f0; (void)Q; return true; }
bool ndspChnIirBiquadSetParamsNotchFilter(int id, float f0, float Q) { (void)id; (void)f0; (void)Q; return true; }
bool ndspChnIirBiquadSetParamsPeakingEqualizer(int id, float f0, float Q, float gain) { (void)id; (void)f0; (void)Q; (void)gain; return true; }

}  // extern "C"
