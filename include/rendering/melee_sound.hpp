#pragma once

// The sounds of a melee swing, as the client times and picks them.
//
// SMSG_ATTACKERSTATEUPDATE (0x00756800 case 0x14a) does not sound a hit. It
// keeps it on the attacker (0x00755050, +0xb98: attacker, target, hit info,
// damage, ..., victim state +0x54, melee spell +0x5c, blocked +0x60) after
// settling the one kept before (0x00756180 -> 0x00755a60), and the
// attacker's model sounds it as its attack animation passes its events
// (0x00756240):
//
//   $CSS  the swing's whoosh (0x004d0720): WeaponSwingSounds2 by the hand's
//         weapon's ItemSubClass WeaponSwingSize (light with none) and the
//         crit, at half volume for a miss - for a kept hit whose victim
//         state is not 0, 2 (dodge) or 6 (evade);
//   $CAH, $AH0..$AH3
//         the blow landing: the kept hit settled on its target (0x00755e40)
//         and let go.
//
// Settling (0x00755a60), for a hit with no melee spell or one that hit:
// a dodge, or no damage, is the attacker's "Combat Miss 1H/2H" whoosh by
// its main hand; a parry, block, evade, immunity or deflection nothing
// here; any other the impact (0x004d0660): WeaponImpactSounds by the
// attacker's weapon (its sound override subclass, else its subclass; the
// fist row unarmed) and its material's metal flag, the target's impact
// column and the crit, where the target is. On the target, after that
// (0x00755e40): a parry is the attacker's weapon on the target's weapon,
// a block on its shield (0x00746540 -> 0x004d0540: the impact table's
// columns 3-6 by the defender's item and its metal flag); a deflection
// unblocked "ShieldWoodImpact"; a full absorb or resist, or immunity,
// "AbsorbGetHit" - those three not again for three to five seconds once a
// blow lands on a target that plays no wound animation (0x00722180,
// noWound below). A parry or block of a target with nothing to defend
// with - an empty main hand parrying - is silent (0x00754ef0). Nothing at all - not even the settling - of a target already
// lootable (UNIT_DYNAMIC_FLAGS 0x1).
//
// A hit kept and never landed is settled - impact or miss, not the
// target's part - when the next swing's hit arrives or the attack stops
// (0x00756770). One whose attacker is not in the world lands at once.

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace wowee::rendering::melee_sound {

/// A swing kept on its attacker until its model lands it.
struct Hit {
    uint64_t attacker = 0;
    uint64_t target = 0;
    uint32_t hitInfo = 0;
    int32_t damage = 0;
    uint32_t victimState = 0;
    uint32_t meleeSpellId = 0;
    uint32_t blocked = 0;
};

constexpr uint32_t kHitOffhand = 0x4;
constexpr uint32_t kHitMiss = 0x10;
constexpr uint32_t kHitFullAbsorb = 0x20;
constexpr uint32_t kHitFullResist = 0x80;
constexpr uint32_t kHitCrit = 0x200;
constexpr uint32_t kHitNoDamageImpact = 0x1000000;

constexpr uint32_t kStateHit = 1, kStateDodge = 2, kStateParry = 3, kStateBlock = 5, kStateEvade = 6,
                   kStateImmune = 7, kStateDeflect = 8;

/// The model events (four characters, little-endian).
constexpr uint32_t kEventSwing = 0x53534324;  ///< $CSS
constexpr uint32_t kEventHit = 0x48414324;    ///< $CAH
constexpr bool isLandEvent(uint32_t id) {
    return id == kEventHit || id == 0x30484124 || id == 0x31484124 || id == 0x32484124 || id == 0x33484124;
}

/// An item a unit holds, as its virtual item info has it.
struct Item {
    uint32_t itemClass = 0;
    uint32_t subClass = 0;
    int32_t soundOverride = -1;  ///< Item.dbc Sound_override_subclassid
    uint32_t material = 0;
};
constexpr uint32_t kClassWeapon = 2, kClassArmor = 4;

/// The weapon subclasses swung two-handed (0x00714c80).
constexpr bool twoHanded(const std::optional<Item>& item) {
    if (!item || item->itemClass != kClassWeapon) return false;
    switch (item->subClass) {
    case 1: case 5: case 6: case 8: case 10: case 12: case 17: case 20: return true;
    default: return false;
    }
}

/// What settling a hit sounds (0x00755a60).
enum class Settle { None, Miss, Impact };
constexpr Settle settle(const Hit& hit) {
    if (hit.meleeSpellId != 0 && hit.victimState != kStateHit) return Settle::None;
    switch (hit.victimState) {
    case kStateDeflect: case kStateParry: case kStateEvade: case kStateBlock: case kStateImmune:
        return Settle::None;
    case kStateDodge: return Settle::Miss;
    default: break;
    }
    if (hit.damage == 0 && (hit.hitInfo & kHitNoDamageImpact) == 0) return Settle::Miss;
    return Settle::Impact;
}

/// Whether $CSS whooshes for the kept hit.
constexpr bool swingSounds(const Hit& hit) {
    return hit.victimState != 0 && hit.victimState != kStateDodge && hit.victimState != kStateEvade;
}

/// What the target's part plays after the settling (0x00755e40).
enum class Defend { None, Parry, Block, Deflect };
constexpr Defend defend(const Hit& hit) {
    if (hit.victimState == kStateParry) return Defend::Parry;
    if (hit.blocked != 0) return Defend::Block;
    if (hit.victimState == kStateDeflect) return Defend::Deflect;
    return Defend::None;
}
constexpr bool absorbs(const Hit& hit) {
    return (hit.hitInfo & (kHitFullAbsorb | kHitFullResist)) != 0 || hit.victimState == kStateImmune;
}
/// Whether a unit plays no wound animation (0x00722180): a vehicle busy
/// with a cast (0x007571c0 - its Vehicle.dbc Flags 0x10000 while it casts,
/// +0xa38 0x400 being set as a cast's precast kit animates it); else a
/// creature as its cache's type flags 0x8 have it; else - a player, or a
/// creature whose cache entry has not come - one knocked down (its
/// animation 121, Knockdown).
constexpr uint32_t kVehicleFlagCastNoWound = 0x10000;
constexpr uint32_t kCreatureTypeFlagNoWound = 0x8;
constexpr uint32_t kAnimKnockdown = 121;
constexpr bool noWound(bool vehicleCasting, std::optional<uint32_t> creatureTypeFlags, uint32_t animation) {
    if (vehicleCasting) return true;
    if (creatureTypeFlags) return (*creatureTypeFlags & kCreatureTypeFlagNoWound) != 0;
    return animation == kAnimKnockdown;
}

/// How long a no-wound target's landing holds off the next one's parry,
/// block and absorb: three seconds and a draw of up to two more
/// (0x00755ed3: 3000 plus 0x00464580's 32 bits scaled to [0, 2000)).
constexpr uint32_t reactionHoldMs(uint32_t random32) {
    return 3000u + static_cast<uint32_t>((static_cast<uint64_t>(random32) * 2000u) >> 32);
}

/// A creature's impact column by its CreatureSoundData CreatureImpactType
/// (0x007463e0, 0x00a37170): flesh, stone, wood, ethereal.
constexpr uint32_t creatureImpactColumn(uint32_t impactType) {
    constexpr uint32_t kColumn[4] = {0, 8, 7, 9};
    return impactType < 4 ? kColumn[impactType] : 0;
}
/// A player's by its chest's Material flags (0x007634f0): plate, chain,
/// else flesh.
constexpr uint32_t playerImpactColumn(uint32_t materialFlags) {
    if ((materialFlags & 2) != 0) return 2;
    return (materialFlags & 4) != 0 ? 1 : 0;
}
/// The column a weapon striking a defender's item sounds by (0x004d0540):
/// on a weapon 5 metal / 6 wood, on a shield 3 metal / 4 wood.
constexpr uint32_t defenceColumn(const std::optional<Item>& item, bool metal) {
    if (item && item->itemClass == kClassWeapon) return metal ? 5 : 6;
    if (item && item->itemClass == kClassArmor) return metal ? 3 : 4;
    return 6;
}

/// WeaponImpactSounds and WeaponSwingSounds2, as 0x004d0380 and
/// 0x004d04e0 lay them out.
class Tables {
public:
    void addImpact(uint32_t subclass, uint32_t parryType, const std::array<uint32_t, 10>& normal,
                   const std::array<uint32_t, 10>& crit) {
        if (parryType > 1) return;
        auto& row = impacts_[subclass];
        for (uint32_t c = 0; c < 10; ++c) row[c][parryType] = {normal[c], crit[c]};
    }
    void addSwing(uint32_t size, bool crit, uint32_t sound) {
        if (size < 3) swings_[size][crit ? 1 : 0] = sound;
    }
    /// 0 for none.
    [[nodiscard]] uint32_t impact(uint32_t subclass, uint32_t column, bool metal, bool crit) const {
        const auto it = impacts_.find(subclass);
        if (it == impacts_.end() || column >= 10) return 0;
        const auto& pair = it->second[column][metal ? 1 : 0];
        return crit ? pair[1] : pair[0];
    }
    [[nodiscard]] uint32_t swing(uint32_t size, bool crit) const { return size < 3 ? swings_[size][crit ? 1 : 0] : 0; }

private:
    std::unordered_map<uint32_t, std::array<std::array<std::array<uint32_t, 2>, 2>, 10>> impacts_;
    std::array<std::array<uint32_t, 2>, 3> swings_{};
};

/// The impact table's row for a weapon: its sound override subclass where
/// it has one, else its subclass; the fist weapons' with none.
constexpr uint32_t impactSubclass(const std::optional<Item>& weapon, uint32_t unarmedSubclass) {
    if (!weapon) return unarmedSubclass;
    return weapon->soundOverride >= 0 ? static_cast<uint32_t>(weapon->soundOverride) : weapon->subClass;
}

}  // namespace wowee::rendering::melee_sound
