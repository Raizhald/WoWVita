// offline_nn.hpp — dependency-free INT8 inference for the tiny PartyBot policy (Model A: 24 -> 16 -> 8).
//
//   * fixed-size tensors, no heap allocation, no floating point in the inference path
//   * INT8 weights, INT32 accumulators, a documented fixed-point requantisation
//   * a versioned, checksummed model file ("W3NN") that the PC trainer writes
//   * the deterministic Utility AI in offline_bots.cpp remains the fallback and the teacher
//
// The model only *scores tactical categories*. The legal-action mask and the emergency rules in
// offline_bots.cpp are applied afterwards, so the network can never order an illegal cast, heal a
// dead unit or bypass threat/range/cooldown checks.
#pragma once

#include <cstddef>
#include <cstdint>

#include "offline_bots.hpp"

namespace w3n {
namespace offline {

class NeuralPolicy {
public:
    static constexpr int kInputs = kFeatureCount;    // 24
    static constexpr int kHidden = 16;
    static constexpr int kOutputs = kActionCount;    // 8

    void clear();
    bool load(const char* path);
    bool loadBuffer(const uint8_t* data, size_t size);
    bool ready() const { return ready_; }

    // Raw forward pass. 'out' receives INT8 scores in [-127,127] for the eight categories.
    void scores(const int8_t in[kInputs], int8_t out[kOutputs]) const;
    // argmax over the eight scores (-1 when the model is not loaded).
    int infer(const int8_t in[kInputs]) const;
    uint32_t checksum() const { return checksum_; }
    uint16_t version() const { return version_; }

private:
    int8_t w1_[kHidden][kInputs] = {};
    int32_t b1_[kHidden] = {};
    int8_t w2_[kOutputs][kHidden] = {};
    int32_t b2_[kOutputs] = {};
    uint8_t shift1_ = 0, shift2_ = 0;
    bool ready_ = false;
    uint16_t version_ = 0;
    uint32_t checksum_ = 0;
};

// Process-wide instance the offline session installs; nullptr = deterministic AI only.
NeuralPolicy* neuralPolicy();
void setNeuralPolicy(NeuralPolicy* p);

// Fixed-point helper shared by the runtime and the bit-exact tests: round(n / 127) with truncation
// toward zero on both sides. The Python exporter implements exactly the same function.
int32_t nnDiv127(int32_t n);
uint32_t nnChecksum(const uint8_t* data, size_t size);

}  // namespace offline
}  // namespace w3n
