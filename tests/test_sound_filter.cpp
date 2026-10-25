// A sound filter's chain (audio/sound_filter.hpp): the units SoundFilterElem
// builds, run over a clip.
#include <catch_amalgamated.hpp>

#include <cmath>
#include <numbers>

#include "audio/sound_filter.hpp"

namespace sf = wowee::audio::sound_filter;

namespace {
constexpr uint32_t kRate = 22050;
std::vector<float> tone(float hz, size_t frames) {
    std::vector<float> x(frames);
    for (size_t i = 0; i < frames; ++i)
        x[i] = 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * hz * static_cast<float>(i) / kRate);
    return x;
}
float rms(const std::vector<float>& x, size_t from) {
    double s = 0;
    for (size_t i = from; i < x.size(); ++i) s += double(x[i]) * x[i];
    return static_cast<float>(std::sqrt(s / double(x.size() - from)));
}
/// The tone's frequency by its zero crossings.
float frequency(const std::vector<float>& x, size_t from) {
    int crossings = 0;
    for (size_t i = from + 1; i < x.size(); ++i)
        if ((x[i - 1] < 0) != (x[i] < 0)) ++crossings;
    return crossings * 0.5f * kRate / static_cast<float>(x.size() - from);
}
}  // namespace

TEST_CASE("an echo is the clip again its delay later, under the dry", "[sound_filter]") {
    std::vector<float> x(2000, 0.0f);
    x[0] = 1.0f;
    sf::Element e{5, {1.0f, 10.0f, 0.0f, 0.16f, 1.0f}};
    sf::apply(x, 1, kRate, {e});
    const size_t d = static_cast<size_t>(0.010f * kRate);
    CHECK(x[0] == Catch::Approx(0.16f));
    CHECK(x[d] == Catch::Approx(1.0f));
    CHECK(x[2 * d] == Catch::Approx(0.0f));  // no decay, no second echo
}

TEST_CASE("a pitch shift moves the tone and keeps the length", "[sound_filter]") {
    auto x = tone(440.0f, kRate);
    sf::apply(x, 1, kRate, {sf::Element{1, {1.0f, 0.9f}}});
    CHECK(x.size() == kRate);
    CHECK(frequency(x, 2000) == Catch::Approx(396.0f).margin(15.0f));
    // And up: the Forsaken male's 1.134.
    auto y = tone(440.0f, kRate);
    sf::apply(y, 1, kRate, {sf::Element{1, {1.0f, 1.134f}}});
    CHECK(y.size() == kRate);
    CHECK(frequency(y, 2000) == Catch::Approx(499.0f).margin(15.0f));
}

TEST_CASE("a low pass quiets what is above its cutoff", "[sound_filter]") {
    auto low = tone(200.0f, kRate), high = tone(6000.0f, kRate);
    const sf::Element lp{3, {1.0f, 1000.0f, 1.0f}};
    sf::apply(low, 1, kRate, {lp});
    sf::apply(high, 1, kRate, {lp});
    CHECK(rms(low, 1000) > 0.3f);
    CHECK(rms(high, 1000) < 0.05f);
}

TEST_CASE("a bypassed unit does nothing", "[sound_filter]") {
    auto x = tone(440.0f, 4000);
    const auto before = x;
    sf::apply(x, 1, kRate, {sf::Element{5, {0.0f, 67.0f, 0.035f, 0.16f, 1.0f}}});
    CHECK(x == before);
}

TEST_CASE("the chain's clip is a 16-bit WAV", "[sound_filter]") {
    const auto wav = sf::encodeWav({0.0f, 1.0f, -1.0f, 2.0f}, 2, kRate);
    REQUIRE(wav.size() == 44 + 8);
    CHECK(std::string(wav.begin(), wav.begin() + 4) == "RIFF");
    CHECK(wav[22] == 2);
    CHECK((wav[46] | (wav[47] << 8)) == 32767);
    CHECK((wav[50] | (wav[51] << 8)) == 32767);  // clipped
}
