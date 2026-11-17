// offline_nn.cpp — W3NN model loading and the fixed-point forward pass.
#include "offline_nn.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace w3n {
namespace offline {

namespace {
constexpr uint16_t kModelVersion = 1;
constexpr uint32_t kMagic = 0x4E4E3357u;   // 'W','3','N','N' little-endian
NeuralPolicy* g_policy = nullptr;

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
int32_t s8(int8_t v) { return (int32_t)v; }
int8_t clamp8(int32_t v) { return (int8_t)(v < -127 ? -127 : (v > 127 ? 127 : v)); }
}  // namespace

int32_t nnDiv127(int32_t n) {
    // Deterministic symmetric rounding. The Python exporter uses the same expression.
    return n >= 0 ? (n + 63) / 127 : -((-n + 63) / 127);
}

uint32_t nnChecksum(const uint8_t* data, size_t size) {
    uint32_t h = 2166136261u;   // FNV-1a
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

void NeuralPolicy::clear() {
    *this = NeuralPolicy();
}

bool NeuralPolicy::load(const char* path) {
    clear();
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    const long len = ftell(f);
    if (len <= 0 || len > 64 * 1024) { fclose(f); return false; }
    // The model file is tiny, but the main thread stack on the 3DS is not large enough for a fixed
    // 64 KB buffer here (it was the cause of a hard fault). Keep it on the heap.
    std::vector<uint8_t> buf((size_t)len);
    rewind(f);
    const size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (got != buf.size()) return false;
    return loadBuffer(buf.data(), buf.size());
}

bool NeuralPolicy::loadBuffer(const uint8_t* data, size_t size) {
    clear();
    constexpr size_t kHeader = 20;
    constexpr size_t kWeights = (size_t)kHidden * kInputs + (size_t)kOutputs * kHidden;
    constexpr size_t kBiases = (size_t)(kHidden + kOutputs) * 4;
    if (!data || size != kHeader + kWeights + kBiases) return false;
    if (rd32(data) != kMagic) return false;
    const uint16_t ver = rd16(data + 4);
    if (ver != kModelVersion) return false;
    if (rd16(data + 6) != kInputs || rd16(data + 8) != kHidden || rd16(data + 10) != kOutputs) return false;
    const uint8_t s1 = data[12], s2 = data[13];
    if (s1 > 8 || s2 > 8) return false;   // a corrupt header must not shift out of range
    const uint32_t want = rd32(data + 16);
    const uint8_t* payload = data + kHeader;
    if (nnChecksum(payload, kWeights + kBiases) != want) return false;

    size_t at = kHeader;
    for (int h = 0; h < kHidden; ++h)
        for (int i = 0; i < kInputs; ++i) w1_[h][i] = (int8_t)data[at++];
    for (int h = 0; h < kHidden; ++h) { b1_[h] = (int32_t)rd32(data + at); at += 4; }
    for (int o = 0; o < kOutputs; ++o)
        for (int h = 0; h < kHidden; ++h) w2_[o][h] = (int8_t)data[at++];
    for (int o = 0; o < kOutputs; ++o) { b2_[o] = (int32_t)rd32(data + at); at += 4; }
    version_ = ver;
    checksum_ = want;
    shift1_ = s1;
    shift2_ = s2;
    ready_ = true;
    return true;
}

void NeuralPolicy::scores(const int8_t in[kInputs], int8_t out[kOutputs]) const {
    for (int o = 0; o < kOutputs; ++o) out[o] = 0;
    if (!ready_) return;
    int8_t hidden[kHidden];
    for (int h = 0; h < kHidden; ++h) {
        int32_t acc = 0;
        for (int i = 0; i < kInputs; ++i) acc += s8(in[i]) * s8(w1_[h][i]);
        int32_t v = nnDiv127(acc * (1 << shift1_) + b1_[h]);
        if (v < 0) v = 0;           // ReLU
        if (v > 127) v = 127;
        hidden[h] = (int8_t)v;
    }
    for (int o = 0; o < kOutputs; ++o) {
        int32_t acc = 0;
        for (int h = 0; h < kHidden; ++h) acc += s8(hidden[h]) * s8(w2_[o][h]);
        out[o] = clamp8(nnDiv127(acc * (1 << shift2_) + b2_[o]));
    }
}

int NeuralPolicy::infer(const int8_t in[kInputs]) const {
    if (!ready_) return -1;
    int8_t s[kOutputs];
    scores(in, s);
    int best = 0;
    for (int i = 1; i < kOutputs; ++i)
        if (s[i] > s[best]) best = i;
    return best;
}

NeuralPolicy* neuralPolicy() { return g_policy; }
void setNeuralPolicy(NeuralPolicy* p) { g_policy = p; }

}  // namespace offline
}  // namespace w3n
