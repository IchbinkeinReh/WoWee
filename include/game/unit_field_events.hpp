#pragma once

/// The events a unit's fields raise as their values change, for every token that
/// names the unit (0x00614190 -> 0x0060c240 -> 0x0060bf10: the event of a field
/// is the one of its number), and those its watchers raise besides.

#include "game/power_prediction.hpp"

#include <cstdint>
#include <vector>

namespace wowee::game {

/// Where the fields are, 0xFFFF for one the expansion has not.
struct UnitEventFields {
    uint16_t bytes0 = 0xFFFF, health = 0xFFFF, power1 = 0xFFFF, maxHealth = 0xFFFF, maxPower1 = 0xFFFF;
    uint16_t level = 0xFFFF, faction = 0xFFFF, flags = 0xFFFF, flags2 = 0xFFFF, dynamic = 0xFFFF;
};

/// A field's event, null for one that has none or is raised elsewhere.
[[nodiscard]] inline const char* unitFieldEvent(const UnitEventFields& f, uint16_t key) {
    if (key == 0xFFFF) return nullptr;
    if (key == f.bytes0) return "UNIT_DISPLAYPOWER";
    if (key == f.health) return "UNIT_HEALTH";
    if (key == f.maxHealth) return "UNIT_MAXHEALTH";
    if (key == f.level) return "UNIT_LEVEL";
    if (key == f.faction) return "UNIT_FACTION";
    if (key == f.flags || key == f.flags2) return "UNIT_FLAGS";
    if (key == f.dynamic) return "UNIT_DYNAMIC_FLAGS";
    if (f.power1 != 0xFFFF && key >= f.power1 && key < f.power1 + 7) return power_prediction::kPowerEvents[key - f.power1];
    if (f.maxPower1 != 0xFFFF && key >= f.maxPower1 && key < f.maxPower1 + 7)
        return power_prediction::kMaxPowerEvents[key - f.maxPower1];
    return nullptr;
}

/// What a unit's reaction and state hang on.
struct UnitSigns {
    uint32_t flags = 0;      ///< UNIT_FIELD_FLAGS
    uint32_t dynamic = 0;    ///< UNIT_DYNAMIC_FLAGS
    uint8_t pvp = 0;         ///< UNIT_FIELD_BYTES_2's second byte
    uint8_t powerType = 0;   ///< UNIT_FIELD_BYTES_0's fourth
};

/// The events of the watchers: UNIT_FACTION as its attackability (flags 0x180,
/// 0x0073c330), its PvP flags (0x00724d90) or its being tapped (0x4, 0x00740570)
/// changes, UNIT_HEALTH and its power's as it feigns death or stops (0x20).
[[nodiscard]] inline std::vector<const char*> unitSignEvents(const UnitSigns& was, const UnitSigns& is) {
    std::vector<const char*> events;
    if (((was.flags ^ is.flags) & 0x180u) != 0) events.push_back("UNIT_FACTION");
    if (was.pvp != is.pvp) events.push_back("UNIT_FACTION");
    if (((was.dynamic ^ is.dynamic) & 0x20u) != 0) {
        events.push_back("UNIT_HEALTH");
        if (is.powerType < 7 && power_prediction::kPowerEvents[is.powerType])
            events.push_back(power_prediction::kPowerEvents[is.powerType]);
    }
    if (((was.dynamic ^ is.dynamic) & 0x4u) != 0) events.push_back("UNIT_FACTION");
    return events;
}

}  // namespace wowee::game
