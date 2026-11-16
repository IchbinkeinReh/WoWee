#pragma once

#include <cstdint>
#include <optional>

namespace wowee {
namespace game {
namespace unit_combat {

/// UNIT_COMBAT's third argument, from the hit's amount and its hit flags
/// (0x005132c0). A hit that did nothing says why - ABSORB (0x20), else BLOCK
/// (0x2000), else RESIST (0x80); one that landed says how - CRITICAL (0x200),
/// else GLANCING (0x10000), else CRUSHING (0x20000). So a fully absorbed
/// swing is a WOUND of 0 with the ABSORB flag, not an ABSORB of its amount.
inline const char* flag(int32_t amount, uint32_t hitInfo) {
    if (amount < 1) {
        if (hitInfo & 0x20u) return "ABSORB";
        if (hitInfo & 0x2000u) return "BLOCK";
        if (hitInfo & 0x80u) return "RESIST";
        return "";
    }
    if (hitInfo & 0x200u) return "CRITICAL";
    if (hitInfo & 0x10000u) return "GLANCING";
    if (hitInfo & 0x20000u) return "CRUSHING";
    return "";
}

/// A swing's action by the victim's state (0x005133b0, table 0x00ac8050);
/// null for a state past the table, which raises nothing.
inline const char* meleeAction(uint32_t victimState) {
    static constexpr const char* kByState[] = {
        "MISS", "WOUND", "DODGE", "PARRY", "INTERRUPT", "BLOCK", "EVADE", "IMMUNE", "DEFLECT",
    };
    return victimState < 9 ? kByState[victimState] : nullptr;
}

/// A spell's miss by its miss reason (0x00513410, table 0x00adbfac); null
/// past the table. Both IMMUNE reasons read IMMUNE.
inline const char* missAction(uint32_t missInfo) {
    static constexpr const char* kByReason[] = {
        "NONE", "MISS", "RESIST", "DODGE", "PARRY", "BLOCK",
        "EVADE", "IMMUNE", "IMMUNE", "DEFLECT", "ABSORB", "REFLECT",
    };
    return missInfo < 12 ? kByReason[missInfo] : nullptr;
}

/// The school a miss or a heal reports: the mask of the Resistances row
/// flagged physical (0x006337b0, index set at 0x00633330), which is row 0.
inline constexpr int32_t kPhysicalSchoolMask = 1;

/// The school mask of each environmental damage type (table 0x00a2d394,
/// read by 0x00755270): fatigue, drowning and falling physical, lava and
/// fire fire, slime nature, the void shadow.
inline uint32_t environmentSchoolMask(uint32_t type) {
    static constexpr uint32_t kSchoolBit[7] = {0, 0, 0, 2, 3, 2, 5};
    return 1u << (type < 7 ? kSchoolBit[type] : 0u);
}

/// Whether a spell's log raises UNIT_COMBAT: the client has its record and
/// its Attributes lack 0x100, the hidden-from-log bit (0x006d3840,
/// 0x006d3c30, 0x007531f0, 0x006d3090 test the record they looked up).
inline bool spellRaises(std::optional<uint32_t> attributes) {
    return attributes && (*attributes & 0x100u) == 0;
}

/// Hit flags built from a spell or periodic log's amounts, as 0x006d3840 and
/// 0x007531f0 build them for 0x005132c0: absorbed 0x20, resisted 0x80,
/// blocked 0x2000, critical 0x200.
inline uint32_t hitInfoFromLog(uint32_t absorbed, uint32_t resisted, uint32_t blocked, bool crit) {
    uint32_t h = 0;
    if (crit) h |= 0x200u;
    if (absorbed != 0) h |= 0x20u;
    if (blocked != 0) h |= 0x2000u;
    if (resisted != 0) h |= 0x80u;
    return h;
}

}  // namespace unit_combat
}  // namespace game
}  // namespace wowee
