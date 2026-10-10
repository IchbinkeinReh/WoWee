#pragma once

/// Which units the client names over their heads and which it gives a
/// nameplate, and what the names say: PlayerName.cpp (0x007e5640), the unit's
/// name question (0x00729c70), the plate's (0x0072b060) and the cvars both
/// read (0x007e6150, 0x00511xxx). Arithmetic only; the drawing is the HUD's.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace wowee::game::unit_names {

/// 0x007e6150: each UnitName* cvar is one bit of the mask the name question is
/// asked with (0x00d380a0). Note there is no bit read for the friendly
/// guardian: 0x00729c70 answers every guardian with the enemy one's.
enum Bit : uint32_t {
    kOwn = 0x1,
    kNpc = 0x2,
    kPlayerGuild = 0x4,
    kPlayerPvpTitle = 0x8,
    kEnemyPlayer = 0x10,
    kEnemyPet = 0x20,
    kEnemyTotem = 0x40,
    kFriendlyPlayer = 0x80,
    kFriendlyPet = 0x100,
    kFriendlyTotem = 0x200,
    kNonCombatCreature = 0x400,
    kEnemyGuardian = 0x1000,
    kFriendlyGuardian = 0x2000,
};

struct NameCVar {
    const char* name;
    uint32_t bit;
    const char* defaultValue;
};

/// 0x007e6150, in its order: the guild, title and player and pet rows default
/// on ("1", 0x009e1464), the rest off ("0", 0x009e14a0).
inline constexpr NameCVar kNameCVars[] = {
    {"UnitNameOwn", kOwn, "0"},
    {"UnitNameNPC", kNpc, "0"},
    {"UnitNamePlayerGuild", kPlayerGuild, "1"},
    {"UnitNamePlayerPVPTitle", kPlayerPvpTitle, "1"},
    {"UnitNameEnemyPlayerName", kEnemyPlayer, "1"},
    {"UnitNameEnemyPetName", kEnemyPet, "1"},
    {"UnitNameEnemyGuardianName", kEnemyGuardian, "0"},
    {"UnitNameEnemyTotemName", kEnemyTotem, "0"},
    {"UnitNameFriendlyPlayerName", kFriendlyPlayer, "1"},
    {"UnitNameFriendlyPetName", kFriendlyPet, "1"},
    {"UnitNameFriendlyGuardianName", kFriendlyGuardian, "0"},
    {"UnitNameFriendlyTotemName", kFriendlyTotem, "0"},
    {"UnitNameNonCombatCreatureName", kNonCombatCreature, "0"},
};

/// The mask from the cvars, `value(name, default)` answering each one's
/// stored value.
template <typename Lookup>
uint32_t nameMask(Lookup&& value) {
    uint32_t mask = 0;
    for (const NameCVar& c : kNameCVars) {
        const std::string v = value(c.name, c.defaultValue);
        if (!v.empty() && v != "0") mask |= c.bit;
    }
    return mask;
}

/// The nameplate cvars and their defaults (registered beside the other
/// interface cvars, 0x00511xxx): both the enemy and friend switches off, every
/// pet, guardian and totem row on, overlap allowed.
struct PlateCVar {
    const char* name;
    const char* defaultValue;
};
inline constexpr PlateCVar kPlateCVars[] = {
    {"nameplateShowEnemies", "0"},
    {"nameplateShowEnemyPets", "1"},
    {"nameplateShowEnemyGuardians", "1"},
    {"nameplateShowEnemyTotems", "1"},
    {"nameplateShowFriends", "0"},
    {"nameplateShowFriendlyPets", "1"},
    {"nameplateShowFriendlyGuardians", "1"},
    {"nameplateShowFriendlyTotems", "1"},
    {"nameplateAllowOverlap", "1"},
};

/// Creature types (CreatureType.dbc) the questions single out.
inline constexpr uint32_t kCreatureTypeCritter = 8;
inline constexpr uint32_t kCreatureTypeTotem = 11;
inline constexpr uint32_t kCreatureTypeNonCombatPet = 12;

/// What both questions read off a unit, other than the player.
struct UnitFacts {
    bool isPlayer = false;
    /// The unit has UNIT_FLAG_NOT_SELECTABLE (0x0071a390: no name, no plate).
    bool notSelectable = false;
    /// The player may attack it (0x00729740).
    bool attackable = false;
    /// 0x00729b30: not attackable and of the player's own faction group, or
    /// either has none. What decides "friendly" for a player.
    bool friendlyGroup = false;
    /// UNIT_FIELD_BYTES_1 byte 2 bit 2: creeping (stealthed).
    bool creeping = false;
    /// Its creature type (0x0071f300).
    uint32_t creatureType = 0;
    /// The owner is a player: UNIT_FIELD_CHARMEDBY, else UNIT_FIELD_CREATEDBY
    /// (0x004f5f20), names one.
    bool ownerIsPlayer = false;
    bool charmedBy = false;
    bool summonedBy = false;
    bool createdBy = false;
    /// UNIT_FIELD_FLAGS 0x200.
    bool immuneToNpc = false;
    /// Its charmer charms it back and is the player or friendly to them
    /// (0x00729c70's pet test).
    bool charmerFriendly = false;
    /// Health 1 or more, and no UNIT_DYNFLAG_DEAD.
    bool alive = true;
};

/// 0x0071b600: made by someone (UNIT_FIELD_CREATEDBY) and neither charmed nor
/// summoned - what the client calls a guardian here.
constexpr bool isGuardian(const UnitFacts& u) {
    return u.createdBy && !u.charmedBy && !u.summonedBy;
}

/// 0x00729c70: whether the unit's name is drawn over it, given the mask. The
/// plate question goes first: a unit with a plate has its name on the plate
/// (param_1[0x30e], +0xc38) and none over its head. Then the target is always
/// named; the player answers to UnitNameOwn alone (see ownNameShown).
constexpr bool nameShown(const UnitFacts& u, uint32_t mask, bool isTarget, bool hasPlate) {
    if (u.notSelectable || hasPlate) return false;
    if (isTarget) return true;
    if (u.isPlayer) {
        if (u.friendlyGroup) return (mask & kFriendlyPlayer) != 0;
        return (mask & kEnemyPlayer) != 0 && !u.creeping;
    }
    const bool hostile = u.attackable;
    const bool critterLike =
        u.creatureType == kCreatureTypeCritter || u.creatureType == kCreatureTypeNonCombatPet;
    if (!u.ownerIsPlayer) {
        if (u.creeping && hostile) return false;
        return (mask & (critterLike ? kNonCombatCreature : kNpc)) != 0;
    }
    if (hostile && u.creeping) return false;
    if ((u.createdBy && !u.summonedBy && u.immuneToNpc) || critterLike) {
        return (mask & kNonCombatCreature) != 0;
    }
    if (u.creatureType == kCreatureTypeTotem) return (mask & (hostile ? kEnemyTotem : kFriendlyTotem)) != 0;
    if (isGuardian(u)) return (mask & kEnemyGuardian) != 0;
    const bool friendlyPet = u.friendlyGroup || u.charmerFriendly;
    return (mask & (friendlyPet ? kFriendlyPet : kEnemyPet)) != 0;
}

/// 0x00729c70 for the player itself (0x0071ffc0): UnitNameOwn.
constexpr bool ownNameShown(uint32_t mask) { return (mask & kOwn) != 0; }

/// The nameplate switches as stored.
struct PlateSwitches {
    bool enemies = false;
    bool friends = false;
    bool enemyPets = true, enemyGuardians = true, enemyTotems = true;
    bool friendlyPets = true, friendlyGuardians = true, friendlyTotems = true;
};

/// 0x0072b060 short of the range and the screen: whether a unit gets a plate.
constexpr bool plateShown(const UnitFacts& u, const PlateSwitches& s) {
    if (!u.alive || u.notSelectable) return false;
    // A player is friendly by 0x00729b30, anything else by not being
    // attackable.
    const bool friendly = u.isPlayer ? u.friendlyGroup : !u.attackable;
    if (!friendly && u.creeping) return false;
    if (!s.enemies && !friendly) return false;
    if (!s.friends && friendly) return false;
    if (u.creatureType == kCreatureTypeCritter || u.creatureType == kCreatureTypeNonCombatPet) return false;
    if (u.ownerIsPlayer) {
        if (u.createdBy && !u.summonedBy && u.immuneToNpc) return false;
        bool on;
        if (u.creatureType == kCreatureTypeTotem) {
            on = friendly ? s.friendlyTotems : s.enemyTotems;
        } else if (isGuardian(u)) {
            on = friendly ? s.friendlyGuardians : s.enemyGuardians;
        } else {
            on = friendly ? s.friendlyPets : s.enemyPets;
        }
        if (!on) return false;
    }
    // A hostile player the player may not attack has none.
    return !u.isPlayer || friendly || u.attackable;
}

/// 0x007e5420 and 0x007e5640: the name's text height in yards - a fifth of a
/// yard, and for a model taller than 4 its height times 0.375 times that.
constexpr float textHeight(float modelHeight) {
    const float scale = modelHeight > 4.0f ? modelHeight * 0.25f * 1.5f : 1.0f;
    return scale * 0.2f;
}

/// 0x007e52a0: the raid icon over a name fades in from the camera - 0x22 within
/// 4 yards, 0xff past 8, a ramp of 55.25 a yard between.
constexpr int raidIconAlpha(float cameraDistance) {
    if (!(cameraDistance > 4.0f)) return 0x22;
    if (cameraDistance > 8.0f) return 0xff;
    return static_cast<int>((cameraDistance - 4.0f) * 55.25f + 34.0f + 0.5f);
}

/// 0x006dc8d0-0x006dc9d0: the tags a player's name starts with, by
/// PLAYER_FLAGS - <AFK> (0x2, or the player's own AFK state), <DND> (0x4), <GM>
/// (0x8 unless also a developer) and <Dev> (0x8000).
inline std::string playerNamePrefix(uint32_t playerFlags, const std::string& afk,
                                    const std::string& dnd, const std::string& gm) {
    std::string out;
    if (playerFlags & 0x2) out += afk;
    if (playerFlags & 0x4) out += dnd;
    if ((playerFlags & 0x8) && !(playerFlags & 0x8000)) out += gm;
    if (playerFlags & 0x8000) out += "<Dev>";
    return out;
}

/// 0x0061e830: the line under the name of a unit something made - "<%s's
/// Pet>" and the like, the %s its owner (UNIT_FIELD_CHARMEDBY, else
/// CREATEDBY; that unit's own owner where it has one). The text is the
/// interface's UNITNAME_SUMMON_TITLE1 to 12 (FrameScript_GetText,
/// 0x00819d40); these are enUS GlobalStrings', for want of an interface.
inline constexpr int kSummonTitleCount = 12;
inline constexpr const char* kSummonTitlesEnUS[] = {
    nullptr,          "%s's Pet",       "%s's Guardian", "%s's Minion",   "%s's Totem",
    "%s's Companion", "%s's Runeblade", "%s's Construct", "%s's Opponent", "%s's Vehicle",
    "%s's Mount",     "%s's Lightwell", "%s's Butler",
};
/// The global the title's text is, "UNITNAME_SUMMON_TITLE<n>"; empty for none.
inline std::string summonTitleKey(int title) {
    if (title <= 0 || title > kSummonTitleCount) return {};
    return "UNITNAME_SUMMON_TITLE" + std::to_string(title);
}
inline constexpr uint32_t kEffectSummon = 28;
inline constexpr uint32_t kCreatureTypeBeast = 1;

/// Which title: the SummonProperties Title (+0xc) of the creating spell's
/// (UNIT_CREATED_BY_SPELL) first SUMMON effect's EffectMiscValueB, where
/// there is such a row and its Title is not -1 - 0 meaning none; otherwise a
/// beast's is Pet (1) and anything else's Minion (3).
constexpr int summonTitle(std::optional<int32_t> propertiesTitle, uint32_t creatureType) {
    if (propertiesTitle && *propertiesTitle != -1) return *propertiesTitle;
    return creatureType == kCreatureTypeBeast ? 1 : 3;
}

/// A localized format with the owner put in for its %s (or %1$s), as the
/// client's sprintf does; a "%%" is a percent sign. Nothing else in it is
/// taken as a conversion, so a string from the interface cannot ask for
/// arguments there are not.
inline std::string formatOwner(const std::string& format, const std::string& owner) {
    std::string out;
    bool placed = false;
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%' || i + 1 >= format.size()) {
            out += format[i];
            continue;
        }
        if (format[i + 1] == '%') {
            out += '%';
            ++i;
        } else if (format[i + 1] == 's' && !placed) {
            out += owner;
            placed = true;
            ++i;
        } else if (format.compare(i + 1, 3, "1$s") == 0 && !placed) {
            out += owner;
            placed = true;
            i += 3;
        } else {
            out += format[i];
        }
    }
    return out;
}

/// The title's text for the owner, empty for none: the interface's string
/// where it has one, else enUS.
inline std::string summonTitleText(int title, const std::string& owner, const std::string& localized = {}) {
    if (title <= 0 || title > kSummonTitleCount) return {};
    return formatOwner(localized.empty() ? kSummonTitlesEnUS[title] : localized, owner);
}

/// 0x0072a000 and 0x0072d4f0: a unit under an aura whose spell applies aura
/// 279 (CLONE_CASTER, Mirror Image's) on an effect the aura has - its flags'
/// bit for that effect index - is named as that aura's caster: the caster's
/// own name while it is about, else the name cached for its guid. 0 when
/// the unit has no such aura. `auraIdsOf(spellId, k)` is the spell's
/// EffectApplyAuraName k, 0 for none or an unknown spell.
inline constexpr uint32_t kAuraCloneCaster = 279;
template <class Auras, class AuraIdsOf>
uint64_t cloneCasterGuid(const Auras& auras, AuraIdsOf auraIdsOf) {
    for (const auto& a : auras) {
        if (a.spellId == 0) continue;
        for (int k = 0; k < 3; ++k) {
            if ((a.flags & (1u << k)) == 0) continue;
            if (auraIdsOf(a.spellId, k) == kAuraCloneCaster) return a.casterGuid;
        }
    }
    return 0;
}

/// 0x0072d4f0: whose name text a unit shows - its Mirror Image caster's,
/// the whole of it, while that caster is about (the function calls itself
/// for the caster, and looks for no image there); its own otherwise.
inline uint64_t nameTextOwner(uint64_t unitGuid, uint64_t cloneCaster, bool casterPresent) {
    return cloneCaster != 0 && casterPresent ? cloneCaster : unitGuid;
}

/// 0x00519df0: whether a plate shows its threat flash, by threatWarning
/// (default 3): 0 never, 1 in a dungeon or raid map (Map.dbc InstanceType 1
/// or 2), 2 in a party or raid, 3 always.
constexpr bool threatWarningOn(int mode, bool inDungeon, bool inGroup) {
    switch (mode) {
        case 1: return inDungeon;
        case 2: return inGroup;
        case 3: return true;
        default: return false;
    }
}

/// 0x0098e9f0: the plate's UI-TargetingFrame-Flash, tinted by the player's
/// threat status on the unit - 1 yellow, 2 orange, 3 red (0x00ad2d70 by the
/// status plus one, the table GetThreatStatusColor reads, 0x00511fe0); none
/// at 0 or off the list. ARGB.
constexpr uint32_t plateThreatColor(int status) {
    switch (status) {
        case 1: return 0xffffff77u;
        case 2: return 0xffff9900u;
        case 3: return 0xffff0000u;
        default: return 0;
    }
}

/// The plate name's colour: red for 5 s after the unit is hurt (0x0098e5b0
/// from 0x0073f330 and AddCombatLogEntry), else yellow under the pointer's
/// glow (0x0098e910), else white (0x0098e980). ARGB.
inline constexpr float kPlateHurtSeconds = 5.0f;
constexpr uint32_t plateNameColor(bool hurt, bool glow) {
    if (hurt) return 0xffff0000u;
    return glow ? 0xffffff00u : 0xffffffffu;
}

/// FOREIGN_SERVER_LABEL: 0x0072d4f0 ends another player's name with it
/// when the name query gave a realm - one from another server in a
/// battleground. This is enUS GlobalStrings', for want of an interface.
inline constexpr const char* kForeignServerLabelEnUS = " (*)";

}  // namespace wowee::game::unit_names
