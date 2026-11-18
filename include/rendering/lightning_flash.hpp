#pragma once

// A thunderstorm's lightning lighting up the world for a moment.
//
// The original client has none of this. A thunderstorm there (Weather.dbc row
// 86, the server's WEATHER_STATE_THUNDERS) is rain (type 1) with its row's
// ambience - thunder heard, not seen: SMSG_WEATHER (0x00526530 case 0x2f4)
// hands the row to 0x007846a0, which keeps the type, the intensity and the
// row's light value, and 0x00784850 eases them into the storm light sets
// (0xd38b4c, read by 0x007f3920). Nothing in MapWeather.cpp or the light code
// (0x007f3230, 0x007ed4c0) times a strike or lifts a colour, and the client's
// CLightning (Common\Lightning.cpp) is the spells' chain lightning only.
//
// So the strike is WoWee's own, shaped after a real one: a few return
// strokes some 40-150 ms apart, each a near-instant rise and a ~50 ms decay,
// the first the brightest, over a short afterglow - half a second in all.
// What it lights is the sky's light: the flat ambient every surface takes
// (so the ray traced ambient occlusion and the interiors' own light shade it
// as they shade the ambient), the sky's bands, the clouds and the fog, in a
// cold blue-white. The direct light, its direction and its shadows are left
// as they are. Nothing here reads an earlier frame: the flash is a function
// of the time since the strike.

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>

namespace wowee::rendering::lightning_flash {

/// Weather.dbc's thunderstorm, the only row that strikes.
inline constexpr uint32_t kThunderstormWeather = 86;

/// The flash's colour, a cold blue-white.
inline constexpr glm::vec3 kColour{0.80f, 0.86f, 1.0f};

/// How much a full flash adds to the flat ambient, and how far it carries the
/// sky's bands, the clouds and the fog toward its colour.
inline constexpr float kAmbientAdd = 0.55f;
inline constexpr float kSkyMix = 0.65f;
inline constexpr float kSkyLevel = 0.95f;
inline constexpr float kCloudMix = 0.70f;
inline constexpr float kFogMix = 0.45f;
inline constexpr float kFogLevel = 0.75f;

/// One return stroke: the near-instant rise, then the decay.
inline constexpr float kStrokeRise = 0.012f;
inline constexpr float kStrokeDecay = 0.050f;
/// The glow the strokes leave, as a share of the strike, and its decay.
inline constexpr float kAfterglow = 0.22f;
inline constexpr float kAfterglowDecay = 0.180f;
/// After this long a strike is over (its last stroke's light well below a
/// hundredth).
inline constexpr float kStrikeLength = 1.2f;

inline constexpr int kMaxStrokes = 4;

struct Stroke {
    float at = 0.0f;         ///< seconds after the strike began
    float amplitude = 0.0f;  ///< 0-1
};

struct Strike {
    std::array<Stroke, kMaxStrokes> strokes{};
    int count = 0;
    /// How bright the strike is here, 0-1: a near one lights everything, a
    /// far one less.
    float strength = 1.0f;
};

/// The strike the test hook pins: four strokes, the first the brightest.
inline Strike referenceStrike() {
    Strike s;
    s.strokes = {Stroke{0.0f, 1.0f}, Stroke{0.075f, 0.65f}, Stroke{0.16f, 0.85f}, Stroke{0.30f, 0.45f}};
    s.count = 4;
    s.strength = 1.0f;
    return s;
}

/// One stroke's light `t` seconds after it began.
inline float strokeLight(float t) {
    if (t < 0.0f) return 0.0f;
    if (t < kStrokeRise) return t / kStrokeRise;
    return std::exp(-(t - kStrokeRise) / kStrokeDecay);
}

/// How bright the strike is `t` seconds after it began, 0-1.
inline float envelope(const Strike& s, float t) {
    if (t < 0.0f || t > kStrikeLength || s.count <= 0) return 0.0f;
    float strokes = 0.0f;
    for (int i = 0; i < s.count && i < kMaxStrokes; ++i)
        strokes = std::max(strokes, s.strokes[i].amplitude * strokeLight(t - s.strokes[i].at));
    // The afterglow rises with the first stroke and outlasts the rest.
    const float glow = kAfterglow * std::min(t / kStrokeRise, 1.0f) * std::exp(-t / kAfterglowDecay);
    const float v = std::max(strokes, glow);
    // Faded to nothing at the strike's end rather than cut there.
    const float tail = std::clamp((kStrikeLength - t) / 0.2f, 0.0f, 1.0f);
    return std::clamp(v * s.strength * tail, 0.0f, 1.0f);
}

/// A strike of 1-4 strokes, of a random strength.
template <class Rng>
Strike randomStrike(Rng& rng) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    Strike s;
    s.count = 1 + std::min(static_cast<int>(u(rng) * 4.0f), 3);
    float at = 0.0f;
    for (int i = 0; i < s.count; ++i) {
        s.strokes[i].at = at;
        s.strokes[i].amplitude = i == 0 ? 1.0f : 0.45f + 0.45f * u(rng);
        at += 0.04f + 0.11f * u(rng);
    }
    s.strength = 0.45f + 0.55f * u(rng);
    return s;
}

/// Whether the weather strikes, and how often: the thunderstorm only, by its
/// intensity (the eased one the weather draws with).
inline bool strikes(uint32_t weatherId, float intensity) {
    return weatherId == kThunderstormWeather && intensity > 0.05f;
}

/// Seconds to the next strike: about every 24 s in a light storm, every 8 s
/// in a full one, never twice within 2.5 s.
template <class Rng>
float nextInterval(Rng& rng, float intensity) {
    std::uniform_real_distribution<float> u(0.35f, 1.65f);
    const float mean = 24.0f + (8.0f - 24.0f) * std::clamp(intensity, 0.0f, 1.0f);
    return std::max(2.5f, mean * u(rng));
}

/// The strikes over time: started by the weather or by hand, and how bright
/// the one under way is. `pin` holds a strike at a fixed time after its start
/// (the test hook's frame strips).
class Scheduler {
public:
    explicit Scheduler(uint32_t seed = 0x5eed1u) : rng_(seed) {}

    /// Moves the clock on by `dt` seconds in `weatherId` at `intensity`.
    void update(float dt, uint32_t weatherId, float intensity) {
        dt = std::max(dt, 0.0f);
        if (pinned_) return;
        if (active_) {
            since_ += dt;
            if (since_ > kStrikeLength) active_ = false;
        }
        if (!strikes(weatherId, intensity)) {
            // The first strike of a storm comes a while after it begins.
            untilNext_ = -1.0f;
            return;
        }
        if (untilNext_ < 0.0f) untilNext_ = nextInterval(rng_, intensity) * 0.5f;
        untilNext_ -= dt;
        if (untilNext_ <= 0.0f) {
            start(randomStrike(rng_));
            untilNext_ = nextInterval(rng_, intensity);
        }
    }

    void start(const Strike& s) {
        strike_ = s;
        since_ = 0.0f;
        active_ = true;
        pinned_ = false;
    }

    /// The strike held `t` seconds after its start until the next start, or
    /// let go (t < 0).
    void pin(const Strike& s, float t) {
        if (t < 0.0f) {
            pinned_ = false;
            active_ = false;
            return;
        }
        strike_ = s;
        since_ = t;
        active_ = true;
        pinned_ = true;
    }

    /// The flash this frame, 0-1.
    [[nodiscard]] float value() const { return active_ ? envelope(strike_, since_) : 0.0f; }
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] float sinceStrike() const { return since_; }

private:
    std::mt19937 rng_;
    Strike strike_{};
    float since_ = 0.0f;
    float untilNext_ = -1.0f;
    bool active_ = false;
    bool pinned_ = false;
};

/// The colours a flash of `flash` (0-1, already scaled by how much of the sky
/// the camera is under) changes. Each takes the colour as the zone's light
/// left it and returns it lit.
inline glm::vec3 ambientAdd(float flash) { return kColour * (kAmbientAdd * flash); }
inline glm::vec3 skyBand(const glm::vec3& c, float flash) {
    return glm::max(c, glm::mix(c, kColour * kSkyLevel, kSkyMix * flash));
}
inline glm::vec3 cloud(const glm::vec3& c, float flash) {
    return glm::max(c, glm::mix(c, kColour, kCloudMix * flash));
}
inline glm::vec3 fog(const glm::vec3& c, float flash) {
    return glm::max(c, glm::mix(c, kColour * kFogLevel, kFogMix * flash));
}

}  // namespace wowee::rendering::lightning_flash
