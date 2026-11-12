// The world's reverb (audio/eax_reverb.hpp): the OFF preset is silence, a
// preset leaves a tail that dies over its decay time.
#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

#include "audio/eax_reverb.hpp"

using wowee::audio::EaxReverb;
using wowee::audio::EaxReverbParams;

namespace {
constexpr uint32_t kRate = 44100;
float energy(const std::vector<float>& x, size_t from, size_t to) {
    float e = 0.0f;
    for (size_t i = from; i < to && i < x.size(); ++i) e += x[i] * x[i];
    return e;
}
EaxReverbParams hall(float decay) {
    EaxReverbParams p;
    p.room = -1000;
    p.roomHF = -100;
    p.decayTime = decay;
    p.decayHFRatio = 0.8f;
    p.reflections = -1500;
    p.reflectionsDelay = 0.02f;
    p.reverb = 0;
    p.reverbDelay = 0.03f;
    p.envSize = 20.0f;
    return p;
}
}  // namespace

TEST_CASE("the OFF preset leaves the sound as it is", "[eax_reverb]") {
    EaxReverb r;
    r.configure(EaxReverbParams{}, kRate, 2);
    CHECK(r.silent());
    std::vector<float> x(2 * 4096, 0.0f);
    x[0] = 1.0f;
    x[1] = 1.0f;
    const std::vector<float> before = x;
    r.process(x.data(), 4096);
    CHECK(x == before);
}

TEST_CASE("a preset leaves a tail after an impulse, after its pre-delay", "[eax_reverb]") {
    EaxReverb r;
    r.configure(hall(2.0f), kRate, 2);
    CHECK_FALSE(r.silent());
    const size_t frames = kRate;  // one second
    std::vector<float> x(2 * frames, 0.0f);
    x[0] = 1.0f;
    x[1] = 1.0f;
    r.process(x.data(), static_cast<uint32_t>(frames));
    // Nothing before the reflections' delay (20 ms) but the impulse itself.
    CHECK(energy(x, 2, 2 * static_cast<size_t>(0.015f * kRate)) == 0.0f);
    CHECK(energy(x, 2 * static_cast<size_t>(0.06f * kRate), 2 * static_cast<size_t>(0.3f * kRate)) > 0.0f);
}

TEST_CASE("a longer decay keeps more of the tail", "[eax_reverb]") {
    const auto tail = [](float decay) {
        EaxReverb r;
        r.configure(hall(decay), kRate, 1);
        std::vector<float> x(2 * kRate, 0.0f);
        x[0] = 1.0f;
        r.process(x.data(), 2 * kRate);
        return energy(x, kRate, 2 * kRate);
    };
    CHECK(tail(4.0f) > tail(0.5f) * 10.0f);
}
