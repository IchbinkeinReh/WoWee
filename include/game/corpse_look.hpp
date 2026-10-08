#pragma once

/// What a CORPSE object's fields say it looks like, as CGCorpse_C reads
/// them (0x00705670 for its model, 0x00705b20 for its dressing).

#include "core/character_paths.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace wowee::game {

/// CORPSE_FIELD_FLAGS: bones (0x00705670), helm hidden and cloak hidden
/// (0x00705b20).
constexpr uint32_t kCorpseFlagBones = 0x01;
constexpr uint32_t kCorpseFlagHideHelm = 0x08;
constexpr uint32_t kCorpseFlagHideCloak = 0x10;
/// CORPSE_FIELD_DYNAMIC_FLAGS 1: lootable, drawn with the loot sparkle
/// (0x00705900, 0x007061e0).
constexpr uint32_t kCorpseDynamicFlagLootable = 0x01;

struct CorpseLook {
    uint32_t flags = 0;
    uint32_t displayId = 0;
    uint8_t race = 0;
    uint8_t gender = 0;
    uint8_t skin = 0;
    uint8_t face = 0;
    uint8_t hairStyle = 0;
    uint8_t hairColor = 0;
    uint8_t facialHair = 0;
    /// The items the character component puts on, by equipment slot, with
    /// their inventory types; 0 where nothing is drawn.
    std::array<uint32_t, 19> displayIds{};
    std::array<uint8_t, 19> inventoryTypes{};
    /// CORPSE_FIELD_DYNAMIC_FLAGS.
    uint32_t dynamicFlags = 0;
    /// CORPSE_FIELD_GUILD: whose emblem a guild tabard carries (0x007059a0).
    uint32_t guildId = 0;

    [[nodiscard]] bool bones() const { return (flags & kCorpseFlagBones) != 0; }
    [[nodiscard]] bool lootable() const { return (dynamicFlags & kCorpseDynamicFlagLootable) != 0; }
    /// The same four bytes PLAYER_BYTES carries: skin, face, hair style,
    /// hair colour.
    [[nodiscard]] uint32_t appearanceBytes() const {
        return static_cast<uint32_t>(skin) | (static_cast<uint32_t>(face) << 8) |
               (static_cast<uint32_t>(hairStyle) << 16) | (static_cast<uint32_t>(hairColor) << 24);
    }
};

/// 0x00705b20 reads BYTES_1 bytes 1-3 (+0x61..+0x63) as race, sex and
/// skin, BYTES_2 (+0x64..+0x67) as face, hair style, hair colour and
/// facial hair. Each item field is a display id with the inventory type in
/// its top byte (masked with 0xffffff). Of the 19 slots it draws:
/// - not the head with flag 8, nor the back with flag 0x10;
/// - not the ranged slot (17), ever;
/// - the main and off hand (15, 16) only as an item object whose guid is
///   the field - which a display id never is - so not those either.
inline CorpseLook corpseLook(uint32_t displayId, const std::array<uint32_t, 19>& items, uint32_t bytes1,
                             uint32_t bytes2, uint32_t flags) {
    CorpseLook look;
    look.flags = flags;
    look.displayId = displayId;
    look.race = static_cast<uint8_t>(bytes1 >> 8);
    look.gender = static_cast<uint8_t>(bytes1 >> 16);
    look.skin = static_cast<uint8_t>(bytes1 >> 24);
    look.face = static_cast<uint8_t>(bytes2);
    look.hairStyle = static_cast<uint8_t>(bytes2 >> 8);
    look.hairColor = static_cast<uint8_t>(bytes2 >> 16);
    look.facialHair = static_cast<uint8_t>(bytes2 >> 24);
    for (size_t slot = 0; slot < items.size(); ++slot) {
        if (slot == 15 || slot == 16 || slot == 17) continue;
        if (slot == 0 && (flags & kCorpseFlagHideHelm) != 0) continue;
        if (slot == 14 && (flags & kCorpseFlagHideCloak) != 0) continue;
        look.displayIds[slot] = items[slot] & 0xFFFFFFu;
        look.inventoryTypes[slot] = static_cast<uint8_t>(items[slot] >> 24);
    }
    return look;
}

/// 0x00705b20: a corpse lies in Dead (6), or Drowned (132) when the liquid
/// over it (the area's level, +0x80 with flag 0x20 at +0x7c, 0x0077f1e0)
/// is more than two thirds of a yard above it.
constexpr uint32_t kCorpseAnimDead = 6;
constexpr uint32_t kCorpseAnimDrowned = 132;
constexpr uint32_t corpsePoseAnimation(std::optional<float> liquidLevel, float z) {
    if (liquidLevel && *liquidLevel - z > 0.6666667f) return kCorpseAnimDrowned;
    return kCorpseAnimDead;
}

/// 0x00705670: bones are the race's death skeleton - ChrRaces +0x2c, the
/// race's file string, then "Male", "Female" or "NOSEX" by sex.
inline std::string corpseBonesModelPath(uint8_t race, uint8_t gender) {
    const char* sex = gender == 0 ? "Male" : gender == 1 ? "Female" : "NOSEX";
    return std::string("World\\Generic\\PassiveDoodads\\DeathSkeletons\\") + core::raceModelFolder(race) + sex +
           "DeathSkeleton.m2";
}

}  // namespace wowee::game
