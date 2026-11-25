#pragma once

#include <cstdint>
#include <optional>

// The breath a unit's model lets out at its $BTH event (0x00732650): bubbles
// under water, a mist in the cold, and a drunk player's bubbles.

namespace wowee::rendering::unit_breath {

enum class Breath : uint8_t { None, Underwater, Cold, Drunk };

/// The attachment it is put on (0x00a32af4), where the model has that one.
inline constexpr int32_t kAttachment = 17;
/// How long what it breathes is kept before it is asked again (0x0071fa90).
inline constexpr uint32_t kAirKeptMs = 10000;

struct Ask {
    uint32_t modelFlags = 0;   ///< CreatureModelData Flags
    bool player = false;
    float drunk = 0.0f;        ///< 0x004f7290
    bool underwater = false;   ///< +0xa30 0x20
    bool cold = false;         ///< +0xa30 0x40
};

[[nodiscard]] constexpr Breath breathOf(const Ask& a) {
    if ((a.modelFlags & 0x2u) != 0) return Breath::None;
    if (a.player && a.drunk >= 0.5f) return Breath::Drunk;
    if (a.underwater) return Breath::Underwater;
    return a.cold ? Breath::Cold : Breath::None;
}

/// The SpellVisualEffectName it is (0x00ada764).
[[nodiscard]] constexpr const char* effectName(Breath b) {
    switch (b) {
        case Breath::Underwater: return "HARDCODED Breath Underwater";
        case Breath::Cold: return "HARDCODED Breath Cold";
        case Breath::Drunk: return "HARDCODED Inebriated Bubbles";
        case Breath::None: break;
    }
    return nullptr;
}

/// 0x0071fa90: the liquid's surface over the unit's feet by more than its
/// model's height and five yards.
[[nodiscard]] constexpr bool underwater(std::optional<float> surface, float feetZ, float height) {
    return surface && height + 5.0f < *surface - feetZ;
}

/// 0x0078f1f0: the area's AreaTable Flags 0x1, the zone's in its place unless
/// the area has 0x2.
[[nodiscard]] constexpr bool coldAir(uint32_t areaFlags, std::optional<uint32_t> zoneFlags) {
    const uint32_t flags = (areaFlags & 0x2u) == 0 && zoneFlags ? *zoneFlags : areaFlags;
    return (flags & 0x1u) != 0;
}

}  // namespace wowee::rendering::unit_breath
