#include "audio/sound_filter.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wowee::audio::sound_filter {

namespace {

constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

/// One channel's samples, read at a fractional position (0 outside).
float at(const std::vector<float>& x, uint32_t channels, uint32_t c, double pos) {
    if (pos < 0.0) return 0.0f;
    const auto frames = x.size() / channels;
    const auto i = static_cast<size_t>(pos);
    if (i + 1 >= frames) return i < frames ? x[i * channels + c] : 0.0f;
    const float t = static_cast<float>(pos - static_cast<double>(i));
    return x[i * channels + c] * (1.0f - t) + x[(i + 1) * channels + c] * t;
}

/// WSOLA: the clip made `factor` times as long at the same pitch, each
/// frame taken where it best continues the last.
std::vector<float> stretch(const std::vector<float>& x, uint32_t channels, uint32_t rate, double factor) {
    const size_t frames = x.size() / channels;
    const size_t n = std::max<size_t>(64, static_cast<size_t>(rate) * 1024 / 44100);
    const size_t hs = n / 2, overlap = n - hs, tol = n / 4;
    const double ha = static_cast<double>(hs) / factor;
    const auto outFrames = static_cast<size_t>(std::llround(static_cast<double>(frames) * factor));
    std::vector<float> out((outFrames + n) * channels, 0.0f);
    std::vector<float> norm(outFrames + n, 0.0f);
    std::vector<float> mono(frames);
    for (size_t f = 0; f < frames; ++f) {
        float m = 0.0f;
        for (uint32_t c = 0; c < channels; ++c) m += x[f * channels + c];
        mono[f] = m;
    }
    const auto sample = [&](const std::vector<float>& v, long long i) {
        return i >= 0 && static_cast<size_t>(i) < v.size() ? v[static_cast<size_t>(i)] : 0.0f;
    };
    long long prev = 0;
    for (size_t k = 0; k * hs < outFrames; ++k) {
        long long pos = 0;
        if (k > 0) {
            const auto nominal = static_cast<long long>(std::llround(static_cast<double>(k) * ha));
            const long long natural = prev + static_cast<long long>(hs);
            double best = -1e30;
            pos = nominal;
            for (long long d = -static_cast<long long>(tol); d <= static_cast<long long>(tol); d += 2) {
                const long long cand = nominal + d;
                if (cand < 0) continue;
                double corr = 0.0;
                for (size_t i = 0; i < overlap; i += 2)
                    corr += double(sample(mono, cand + static_cast<long long>(i))) *
                            sample(mono, natural + static_cast<long long>(i));
                if (corr > best) { best = corr; pos = cand; }
            }
        }
        prev = pos;
        for (size_t i = 0; i < n; ++i) {
            const size_t o = k * hs + i;
            const float w = 0.5f - 0.5f * std::cos(kTwoPi * static_cast<float>(i) / static_cast<float>(n));
            const long long src = pos + static_cast<long long>(i);
            for (uint32_t c = 0; c < channels; ++c) {
                const float v = src >= 0 && static_cast<size_t>(src) < frames ? x[static_cast<size_t>(src) * channels + c] : 0.0f;
                out[o * channels + c] += w * v;
            }
            norm[o] += w;
        }
    }
    out.resize(outFrames * channels);
    for (size_t f = 0; f < outFrames; ++f) {
        const float d = norm[f] > 1e-3f ? norm[f] : 1.0f;
        for (uint32_t c = 0; c < channels; ++c) out[f * channels + c] /= d;
    }
    return out;
}

/// Read at the pitch's rate - higher and shorter, or lower and longer -
/// then stretched back to its length.
void pitchShift(std::vector<float>& x, uint32_t channels, uint32_t rate, float pitch) {
    if (!(pitch > 0.0f) || std::abs(pitch - 1.0f) < 1e-4f) return;
    const size_t frames = x.size() / channels;
    const auto readFrames = static_cast<size_t>(static_cast<double>(frames) / pitch);
    std::vector<float> read(readFrames * channels);
    for (size_t j = 0; j < readFrames; ++j)
        for (uint32_t c = 0; c < channels; ++c) read[j * channels + c] = at(x, channels, c, double(j) * pitch);
    auto out = stretch(read, channels, rate, static_cast<double>(frames) / static_cast<double>(readFrames));
    out.resize(x.size(), 0.0f);
    x = std::move(out);
}

/// An RBJ biquad, low or high pass; FMOD's resonance as its Q.
void biquad(std::vector<float>& x, uint32_t channels, uint32_t rate, float cutoff, float resonance, bool high) {
    const float nyquist = static_cast<float>(rate) * 0.5f;
    cutoff = std::clamp(cutoff, 10.0f, nyquist * 0.99f);
    const float q = std::max(resonance, 0.1f) * std::numbers::sqrt2_v<float> * 0.5f;
    const float w0 = kTwoPi * cutoff / static_cast<float>(rate);
    const float alpha = std::sin(w0) / (2.0f * q);
    const float cw = std::cos(w0);
    const float b1 = high ? -(1.0f + cw) : (1.0f - cw);
    const float b0 = high ? (1.0f + cw) * 0.5f : (1.0f - cw) * 0.5f;
    const float b2 = b0, a0 = 1.0f + alpha, a1 = -2.0f * cw, a2 = 1.0f - alpha;
    const size_t frames = x.size() / channels;
    for (uint32_t c = 0; c < channels; ++c) {
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (size_t f = 0; f < frames; ++f) {
            float& s = x[f * channels + c];
            const float y = (b0 * s + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
            x2 = x1; x1 = s; y2 = y1; y1 = y;
            s = y;
        }
    }
}

void chorus(std::vector<float>& x, uint32_t channels, uint32_t rate, const std::array<float, 9>& p) {
    const float dry = p[1], wet[3] = {p[2], p[3], p[4]};
    const float delay = std::max(p[5], 0.1f) * 0.001f * static_cast<float>(rate);
    const float hz = p[6], depth = p[7], feedback = p[8];
    const size_t frames = x.size() / channels;
    for (uint32_t c = 0; c < channels; ++c) {
        std::vector<float> line(frames, 0.0f);
        for (size_t f = 0; f < frames; ++f) {
            const float in = x[f * channels + c];
            const float phase = kTwoPi * hz * static_cast<float>(f) / static_cast<float>(rate);
            float out = in * dry, fed = 0.0f;
            for (int t = 0; t < 3; ++t) {
                const float d = delay * (1.0f + depth * std::sin(phase + static_cast<float>(t) * kTwoPi / 4.0f));
                const double pos = static_cast<double>(f) - static_cast<double>(d);
                float tap = 0.0f;
                if (pos >= 0.0) {
                    const auto i = static_cast<size_t>(pos);
                    const float fr = static_cast<float>(pos - static_cast<double>(i));
                    tap = line[i] * (1.0f - fr) + (i + 1 < f ? line[i + 1] : line[i]) * fr;
                }
                out += tap * wet[t];
                if (t == 0) fed = tap;
            }
            line[f] = in + fed * feedback;
            x[f * channels + c] = out;
        }
    }
}

void echo(std::vector<float>& x, uint32_t channels, uint32_t rate, const std::array<float, 9>& p) {
    const auto d = static_cast<size_t>(std::max(p[1], 1.0f) * 0.001f * static_cast<float>(rate));
    const float decay = p[2], dry = p[3], wet = p[4];
    const size_t frames = x.size() / channels;
    for (uint32_t c = 0; c < channels; ++c) {
        std::vector<float> line(frames, 0.0f);
        for (size_t f = 0; f < frames; ++f) {
            const float in = x[f * channels + c];
            const float back = f >= d ? line[f - d] : 0.0f;
            line[f] = in + back * decay;
            x[f * channels + c] = in * dry + back * wet;
        }
    }
}

}  // namespace

void apply(std::vector<float>& samples, uint32_t channels, uint32_t sampleRate, const std::vector<Element>& chain) {
    if (channels == 0 || sampleRate == 0 || samples.empty()) return;
    for (const Element& e : chain) {
        if (e.params[0] == 0.0f) continue;  // bypassed
        switch (static_cast<Type>(e.type)) {
        case Type::PitchShift: pitchShift(samples, channels, sampleRate, e.params[1]); break;
        case Type::HighPass: biquad(samples, channels, sampleRate, e.params[1], e.params[2], true); break;
        case Type::LowPass: biquad(samples, channels, sampleRate, e.params[1], e.params[2], false); break;
        case Type::Chorus: chorus(samples, channels, sampleRate, e.params); break;
        case Type::Echo: echo(samples, channels, sampleRate, e.params); break;
        default: break;
        }
    }
}

std::vector<uint8_t> encodeWav(const std::vector<float>& samples, uint32_t channels, uint32_t sampleRate) {
    std::vector<uint8_t> out;
    const auto dataBytes = static_cast<uint32_t>(samples.size() * 2);
    const auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    const auto put16 = [&](uint16_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); };
    const auto tag = [&](const char* s) { out.insert(out.end(), s, s + 4); };
    out.reserve(44 + dataBytes);
    tag("RIFF"); put32(36 + dataBytes); tag("WAVE");
    tag("fmt "); put32(16); put16(1); put16(static_cast<uint16_t>(channels)); put32(sampleRate);
    put32(sampleRate * channels * 2); put16(static_cast<uint16_t>(channels * 2)); put16(16);
    tag("data"); put32(dataBytes);
    for (float s : samples) {
        const auto v = static_cast<int16_t>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
        put16(static_cast<uint16_t>(v));
    }
    return out;
}

}  // namespace wowee::audio::sound_filter
