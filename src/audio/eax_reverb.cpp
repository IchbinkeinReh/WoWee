#include "audio/eax_reverb.hpp"

#include "pipeline/dbc_loader.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::audio {

std::optional<EaxReverbParams> soundProviderPreset(const pipeline::DBCFile& dbc, uint32_t id) {
    if (id == 0 || !dbc.isLoaded() || dbc.getFieldCount() < 23) return std::nullopt;
    const int32_t idx = dbc.findRecordById(id);
    if (idx < 0) return std::nullopt;
    const auto r = static_cast<uint32_t>(idx);
    EaxReverbParams p;
    p.decayTime = dbc.getFloat(r, 4);
    p.envSize = dbc.getFloat(r, 5);
    p.envDiffusion = dbc.getFloat(r, 6);
    p.room = dbc.getInt32(r, 7);
    p.roomHF = dbc.getInt32(r, 8);
    p.decayHFRatio = dbc.getFloat(r, 9);
    p.reflections = dbc.getInt32(r, 10);
    p.reflectionsDelay = dbc.getFloat(r, 11);
    p.reverb = dbc.getInt32(r, 12);
    p.reverbDelay = dbc.getFloat(r, 13);
    p.hfReference = dbc.getFloat(r, 22);
    return p;
}

namespace {

float millibels(int32_t mB) {
    return mB <= -10000 ? 0.0f : std::pow(10.0f, static_cast<float>(mB) / 2000.0f);
}

// Freeverb's tunings, in samples at its own rate, the right side spread by
// 23 samples.
constexpr float kTuningRate = 44.1e3f;
constexpr std::array<int, 8> kCombs = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr std::array<int, 4> kAllpasses = {556, 441, 341, 225};
constexpr int kSpread = 23;
// Freeverb's input and output scales.
constexpr float kInputGain = 0.015f;
constexpr float kOutputGain = 3.0f;

}  // namespace

void EaxReverb::configure(const EaxReverbParams& params, uint32_t sampleRate, uint32_t channels) {
    channels_ = std::max<uint32_t>(1, channels);
    const float rate = static_cast<float>(std::max<uint32_t>(8000, sampleRate));
    const float size = std::clamp(params.envSize / 7.5f, 0.5f, 2.0f) * rate / kTuningRate;
    const float decay = std::clamp(params.decayTime, 0.1f, 20.0f);
    for (size_t s = 0; s < sides_.size(); ++s) {
        const int spread = s == 0 ? 0 : kSpread;
        for (size_t c = 0; c < kCombs.size(); ++c) {
            auto& comb = sides_[s].combs[c];
            const size_t length = std::max<size_t>(1, static_cast<size_t>(static_cast<float>(kCombs[c] + spread) * size));
            if (comb.buffer.size() != length) {
                comb.buffer.assign(length, 0.0f);
                comb.index = 0;
                comb.store = 0.0f;
            }
            // RT60: the loop loses 60 dB over the decay time.
            comb.feedback = std::pow(10.0f, -3.0f * (static_cast<float>(length) / rate) / decay);
        }
        for (size_t a = 0; a < kAllpasses.size(); ++a) {
            auto& ap = sides_[s].allpasses[a];
            const size_t length =
                std::max<size_t>(1, static_cast<size_t>(static_cast<float>(kAllpasses[a] + spread) * rate / kTuningRate));
            if (ap.buffer.size() != length) {
                ap.buffer.assign(length, 0.0f);
                ap.index = 0;
            }
        }
    }
    // High frequencies die faster by the ratio's shortfall.
    damp_ = std::clamp(1.0f - std::clamp(params.decayHFRatio, 0.1f, 2.0f), 0.0f, 0.9f);
    // RoomHF: a one-pole cut on the send, its gain at HFReference that level.
    const float hfGain = std::clamp(millibels(params.roomHF), 0.0f, 1.0f);
    inputCoef_ = std::clamp(hfGain, 0.05f, 1.0f);
    diffusion_ = 0.5f * std::clamp(params.envDiffusion, 0.0f, 1.0f);
    const float roomGain = millibels(params.room);
    wet_ = roomGain * std::clamp(millibels(params.reverb), 0.0f, 4.0f);
    early_ = roomGain * std::clamp(millibels(params.reflections), 0.0f, 4.0f);
    const float preDelaySec = std::clamp(params.reflectionsDelay + params.reverbDelay, 0.0f, 0.4f);
    earlyLength_ = std::max<size_t>(1, static_cast<size_t>(std::clamp(params.reflectionsDelay, 0.0f, 0.3f) * rate));
    preLength_ = std::max<size_t>(earlyLength_ + 1, static_cast<size_t>(preDelaySec * rate) + 1);
    if (preDelay_.size() < preLength_) preDelay_.assign(preLength_, 0.0f);
    preIndex_ %= preDelay_.size();
}

void EaxReverb::process(float* samples, uint32_t frames) {
    if (silent() || preDelay_.empty()) return;
    const size_t preSize = preDelay_.size();
    for (uint32_t f = 0; f < frames; ++f) {
        float* frame = samples + static_cast<size_t>(f) * channels_;
        float mono = 0.0f;
        for (uint32_t c = 0; c < channels_; ++c) mono += frame[c];
        mono /= static_cast<float>(channels_);
        inputLowpass_ += inputCoef_ * (mono - inputLowpass_);
        preDelay_[preIndex_] = inputLowpass_;
        const float late = preDelay_[(preIndex_ + preSize - (preLength_ - 1)) % preSize];
        const float early = preDelay_[(preIndex_ + preSize - earlyLength_) % preSize];
        preIndex_ = (preIndex_ + 1) % preSize;

        const float input = late * kInputGain;
        std::array<float, 2> out{};
        for (size_t s = 0; s < sides_.size(); ++s) {
            float sum = 0.0f;
            for (auto& comb : sides_[s].combs) {
                const float y = comb.buffer[comb.index];
                comb.store = y * (1.0f - damp_) + comb.store * damp_;
                comb.buffer[comb.index] = input + comb.store * comb.feedback;
                comb.index = (comb.index + 1) % comb.buffer.size();
                sum += y;
            }
            for (auto& ap : sides_[s].allpasses) {
                const float b = ap.buffer[ap.index];
                ap.buffer[ap.index] = sum + b * diffusion_;
                sum = b - sum;
                ap.index = (ap.index + 1) % ap.buffer.size();
            }
            out[s] = sum * kOutputGain * wet_ + early * early_ * 0.3f;
        }
        if (channels_ == 1) {
            frame[0] += 0.5f * (out[0] + out[1]);
        } else {
            frame[0] += out[0];
            frame[1] += out[1];
        }
    }
}

}  // namespace wowee::audio
