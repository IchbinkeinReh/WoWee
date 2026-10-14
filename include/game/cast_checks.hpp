#pragma once

/// What 0x00739650 asks of the player before a spell is cast (0x0080cce0
/// calls it after the reach, 0x00809610): the map, the history, the power,
/// the pet, the caster's state - stunned, silenced, mounted, sitting,
/// swimming, moving, shapeshifted - its auras, reputation and area. Each
/// refusal is a SpellCastResult said through 0x00808200, or a game error
/// (0x005216f0). Arithmetic only: the caller reads the world.

#include <cstddef>
#include <cstdint>

namespace wowee::game::cast_check {

/// SpellCastResult codes 0x00739650 answers with.
inline constexpr uint8_t kAffectingCombat = 0x01;
inline constexpr uint8_t kAlreadyHaveCharm = 0x06;
inline constexpr uint8_t kAlreadyHaveSummon = 0x07;
inline constexpr uint8_t kCasterAurastate = 0x16;
inline constexpr uint8_t kCharmed = 0x18;
inline constexpr uint8_t kConfused = 0x1a;
inline constexpr uint8_t kDontReport = 0x1b;
inline constexpr uint8_t kFleeing = 0x22;
inline constexpr uint8_t kIncorrectArea = 0x27;
inline constexpr uint8_t kMoving = 0x33;
inline constexpr uint8_t kNotHere = 0x3c;
inline constexpr uint8_t kNotMounted = 0x40;
inline constexpr uint8_t kNotReady = 0x43;
inline constexpr uint8_t kNotShapeshift = 0x44;
inline constexpr uint8_t kNotStanding = 0x45;
inline constexpr uint8_t kNotUnsheathed = 0x48;
inline constexpr uint8_t kNoMountsAllowed = 0x53;
inline constexpr uint8_t kNoPet = 0x54;
inline constexpr uint8_t kNoPower = 0x55;
inline constexpr uint8_t kOnlyAbovewater = 0x58;
inline constexpr uint8_t kOnlyDaytime = 0x59;
inline constexpr uint8_t kOnlyIndoors = 0x5a;
inline constexpr uint8_t kOnlyMounted = 0x5b;
inline constexpr uint8_t kOnlyNighttime = 0x5c;
inline constexpr uint8_t kOnlyOutdoors = 0x5d;
inline constexpr uint8_t kOnlyShapeshift = 0x5e;
inline constexpr uint8_t kOnlyStealthed = 0x5f;
inline constexpr uint8_t kOnlyUnderwater = 0x60;
inline constexpr uint8_t kPacified = 0x62;
inline constexpr uint8_t kRequiresArea = 0x65;
inline constexpr uint8_t kSilenced = 0x68;
inline constexpr uint8_t kSpellInProgress = 0x69;
inline constexpr uint8_t kStunned = 0x6c;
inline constexpr uint8_t kOnlyBattlegrounds = 0x8e;
inline constexpr uint8_t kPreventedByMechanic = 0x93;
inline constexpr uint8_t kReputation = 0x95;
inline constexpr uint8_t kNotInArena = 0x97;
inline constexpr uint8_t kNotInRaidInstance = 0xa7;
inline constexpr uint8_t kOnlyInArena = 0xa8;

/// Spell.dbc as 0x00739650 and the functions it calls read it (the record's
/// word index in brackets).
struct Spell {
    uint32_t attr = 0;     ///< [4]
    uint32_t attrEx = 0;   ///< [5]
    uint32_t attrEx2 = 0;  ///< [6]
    uint32_t attrEx3 = 0;  ///< [7]
    uint32_t attrEx4 = 0;  ///< [8]
    uint32_t attrEx5 = 0;  ///< [9]
    uint32_t attrEx6 = 0;  ///< [10]
    uint32_t attrEx7 = 0;  ///< [11]
    uint32_t dispel = 0;   ///< [2]
    uint32_t mechanic = 0; ///< [3]
    uint32_t schoolMask = 0;
    uint32_t stances[2] = {0, 0};     ///< [12] (0x007fe850)
    uint32_t stancesNot[2] = {0, 0};  ///< [14] (0x007fe890)
    uint32_t targets = 0;             ///< [16]
    uint32_t casterAuraState = 0;     ///< [20]
    uint32_t casterAuraStateNot = 0;  ///< [22]
    uint32_t casterAuraSpell = 0;     ///< [24]
    uint32_t excludeCasterAuraSpell = 0;  ///< [26]
    int32_t recoveryTime = 0;          ///< [29]
    int32_t categoryRecoveryTime = 0;  ///< [30]
    uint32_t interruptFlags = 0;       ///< [31]
    uint32_t auraInterruptFlags = 0;   ///< [32]
    uint32_t channelInterruptFlags = 0;  ///< [33]
    uint32_t effect[3] = {0, 0, 0};          ///< [71]
    uint32_t effectMechanic[3] = {0, 0, 0};  ///< [83]
    uint32_t implicitA[3] = {0, 0, 0};       ///< [86]
    uint32_t implicitB[3] = {0, 0, 0};       ///< [89]
    uint32_t aura[3] = {0, 0, 0};            ///< [95] EffectApplyAuraName
    uint32_t misc[3] = {0, 0, 0};            ///< [110] EffectMiscValue
    uint32_t trigger[3] = {0, 0, 0};         ///< [116] EffectTriggerSpell
    uint32_t preventionType = 0;  ///< PreventionType
    uint32_t minFactionId = 0;
    uint32_t minReputation = 0;
    uint32_t areaGroupId = 0;
};

/// 0x007fdfa0: whether the spell cast clears the aura's hold - AttributesEx
/// 0x8000, at an aura without Attributes 0x20000000, through one of its
/// APPLY_AURA (6) effects: an immunity (38) to the aura's effect `k`'s type,
/// a school immunity (39, 267) to its schools where its AttributesEx2 has no
/// 0x4000000, a dispel immunity (41) to its dispel type, or a mechanic
/// immunity (77) to its mechanic or its effect's.
[[nodiscard]] constexpr bool castClears(const Spell& cast, const Spell& aura, int k) {
    if ((cast.attrEx & 0x8000u) == 0 || (aura.attr & 0x20000000u) != 0) return false;
    for (int i = 0; i < 3; ++i) {
        if (cast.effect[i] != 6) continue;
        const uint32_t m = cast.misc[i];
        switch (cast.aura[i]) {
            case 38: if (m == aura.aura[k]) return true; break;
            case 39: case 267:
                if ((aura.attrEx2 & 0x4000000u) == 0 && (m & aura.schoolMask) != 0) return true;
                break;
            case 41: if (m == aura.dispel) return true; break;
            case 77: if (m == aura.mechanic || m == aura.effectMechanic[k]) return true; break;
            default: break;
        }
    }
    return false;
}

/// 0x00804430: the caster's auras of type `type` (`auras(fn)` hands each
/// aura's spell to `fn`, which answers false to stop): whether there are any
/// and the spell clears every one of them. At the first it does not, false,
/// with `mechanic` that aura's effect's mechanic, else its own, where it has
/// one.
template <typename Auras>
[[nodiscard]] constexpr bool aurasLet(const Spell& cast, Auras&& auras, uint32_t type, uint32_t& mechanic) {
    bool found = false;
    bool held = false;
    auras([&](const Spell& a) {
        for (int k = 0; k < 3; ++k) {
            if (a.aura[k] != type) continue;
            found = true;
            if (castClears(cast, a, k)) continue;
            if (a.effectMechanic[k] != 0) mechanic = a.effectMechanic[k];
            else if (a.mechanic != 0) mechanic = a.mechanic;
            held = true;
            return false;
        }
        return true;
    });
    return found && !held;
}

/// What a state's aura walk answered: let, or the mechanic it was held by.
struct Hold {
    bool lets = false;
    uint32_t mechanic = 0;
};
/// 0x008046c0 (charm: 6, 177, 2), 0x00804590 (stun: 12), 0x008045c0
/// (silence: 27, 60), 0x00804610 (pacify: 25, 60), 0x00804660 (fear: 7),
/// 0x00804690 (confuse: 5): each type in turn until one lets it.
template <typename Auras, size_t N>
[[nodiscard]] constexpr Hold hold(const Spell& cast, Auras&& auras, const uint32_t (&types)[N]) {
    Hold h;
    for (const uint32_t t : types) {
        if (aurasLet(cast, auras, t, h.mechanic)) {
            h.lets = true;
            return h;
        }
    }
    return h;
}

/// A game error 0x00739650 puts up instead of a cast failure (0x005216f0).
enum class GameError : uint8_t { None, SpellAlreadyKnown, TameFailedTooMany };

/// The caster and its world as 0x00739650 asks about them.
struct Caster {
    int mapType = -1;           ///< Map.dbc InstanceType of the map, -1 none
    bool notReady = false;      ///< 0x00809000: the spell's history runs
    bool enoughPower = true;    ///< 0x008017e0
    bool learnKnown[3] = {false, false, false};  ///< effect 36's spell known (0x007260e0, 0x00726160)
    bool hasCharm = false;      ///< UNIT_FIELD_CHARM
    bool hasSummon = false;     ///< UNIT_FIELD_SUMMON
    bool alive = true;          ///< UNIT_FIELD_HEALTH (+0x48) over 0
    uint8_t playerBytes = 0;    ///< PLAYER_FIELD_BYTES byte 0 (+0x1064)
    bool charmedByOther = false;  ///< UNIT_FIELD_CHARMEDBY, not the active player
    uint32_t unitFlags = 0;     ///< UNIT_FIELD_FLAGS
    Hold charm, stun, silence, pacify, fear, confuse;
    uint8_t sheath = 0;         ///< +0xb5c, UNIT_FIELD_BYTES_2 byte 0
    bool mounted = false;       ///< 0x0051a230: a mount (+0x9c0) not being left
    bool mountableCreature = false;  ///< 0x00715f70: creature type flag 0x800
    bool wouldDismount = false; ///< 0x0071b0c0
    uint8_t standState = 0;     ///< vtable +0x138: UNIT_FIELD_BYTES_1 byte 0
    bool wouldStand = false;    ///< 0x0071b090: autoStand
    int daytime = -1;           ///< 0x004c9850: 05:30 to 21:00; -1 unknown
    uint32_t moveFlags = 0;     ///< the movement's flags (+0xd8 +0x44)
    bool hasCastTime = false;   ///< 0x007ff180
    uint8_t form = 0;           ///< 0x0071af70: UNIT_FIELD_BYTES_2 byte 3
    bool formRow = false;       ///< a SpellShapeshiftForm row for it
    uint32_t formFlags = 0;     ///< its Flags (+0xc)
    bool ignoresForm = false;   ///< 0x007283a0: an aura 275 for the spell
    bool wouldUnshift = false;  ///< 0x0071b130: autoUnshift
    uint8_t visFlags = 0;       ///< UNIT_FIELD_BYTES_1 byte 2; 0x2 stealthed
    bool ignoresAuraState = false;  ///< 0x0072aaf0: an aura 262 for the spell
    uint32_t auraState = 0;     ///< UNIT_FIELD_AURASTATE (0x00716160)
    bool hasCasterAuraSpell = false;   ///< 0x007282a0 (after 0x00802850)
    bool hasExcludedAuraSpell = false;
    int32_t reputation = 0;     ///< 0x005d05b0 for MinFactionId
    /// 0x00519ca0 / 0x007199e0: the area's AreaGroup answer - -1 unknown, 0
    /// in it, else 1 (INCORRECT_AREA) or 2 (REQUIRES_AREA, `areaArg`).
    int area = -1;
    uint32_t areaArg = 0;
    int outdoors = -1;          ///< 0x0071b7f0 (0x0077fbf0); -1 unknown
    int flyable = -1;           ///< 0x007261b0; -1 unknown
};

/// 0x00a2d2fc: the reputation a MinReputation rank starts at.
inline constexpr int32_t kRankStart[8] = {-42000, -6000, -3000, 0, 3000, 9000, 21000, 42000};

struct Result {
    uint8_t code = 0;       ///< 0 cast; else SpellCastResult
    uint32_t arg = 0;       ///< PREVENTED_BY_MECHANIC's mechanic, REQUIRES_AREA's area
    GameError error = GameError::None;
    uint32_t errorSpell = 0;  ///< SPELL_ALREADY_KNOWN's spell
};

/// 0x0072bac0: the form. One the spell may not be cast in (StancesNot) is
/// refused without an aura 275 for it; one it names (Stances) is fine; else
/// out of form a spell naming forms wants one (unless AttributesEx2
/// 0x80000), and in one (a stance, flag 1, is none - 0x00721ca0) a spell
/// flagged not in forms (Attributes 0x10000), or any in a form flagged
/// 0x400, is refused, again without an aura 275. 0 where it may go.
[[nodiscard]] constexpr uint8_t formFailure(const Spell& s, const Caster& c) {
    const int f = static_cast<int>(c.form) - 1;
    const auto bit = [&](const uint32_t (&m)[2]) { return f >= 0 && (m[f >> 5] & (1u << (f & 31))) != 0; };
    if (bit(s.stancesNot)) return c.ignoresForm ? 0 : kNotShapeshift;
    if (bit(s.stances)) return 0;
    const bool inForm = c.form != 0 && (!c.formRow || (c.formFlags & 0x1u) == 0);
    if (!inForm) {
        if ((s.attrEx2 & 0x80000u) != 0) return 0;
    } else if ((s.attr & 0x10000u) != 0 || (c.formRow && (c.formFlags & 0x400u) != 0)) {
        return c.ignoresForm ? 0 : kNotShapeshift;
    }
    return (s.stances[0] != 0 || s.stances[1] != 0) && !c.ignoresForm ? kOnlyShapeshift : 0;
}

/// 0x00739650 for the player, its branches in order, with no item (the
/// cast's param_4).
[[nodiscard]] constexpr Result check(const Spell& s, const Caster& c) {
    const auto fail = [](uint8_t code, uint32_t arg = 0) { return Result{code, arg, GameError::None, 0}; };
    // Arena maps (Map.dbc InstanceType 4): 0x00719980 - not a spell with a
    // cooldown over 15 minutes or AttributesEx4 0x10000, unless 0x20000.
    if (c.mapType >= 0) {
        if (c.mapType == 4 && (s.attrEx4 & 0x20000u) == 0 &&
            (s.recoveryTime > 900000 || s.categoryRecoveryTime > 900000 || (s.attrEx4 & 0x10000u) != 0))
            return fail(kNotInArena);
        if (c.mapType != 4 && (s.attrEx6 & 0x2u) != 0) return fail(kOnlyInArena);
    }
    if (c.notReady) return fail(kNotReady);
    if (!c.enoughPower) return fail(kNoPower);
    for (int i = 0; i < 3; ++i) {
        // LEARN_SPELL of a spell known, or one it is superseded by.
        if (s.effect[i] == 36 && s.trigger[i] != 0 && c.learnKnown[i])
            return {0, 0, GameError::SpellAlreadyKnown, s.trigger[i]};
        // SUMMON_PET, TAME_CREATURE, MOD_CHARM, MOD_POSSESS: none charmed or
        // summoned, unless AttributesEx 0x1.
        if ((s.effect[i] == 56 || s.effect[i] == 55 || s.aura[i] == 6 || s.aura[i] == 2) &&
            (c.hasCharm || c.hasSummon) && (s.attrEx & 0x1u) == 0)
            return fail(c.hasCharm ? kAlreadyHaveCharm : kAlreadyHaveSummon);
        // TARGET_UNIT_PET: a pet.
        if ((s.implicitA[i] == 5 || s.implicitB[i] == 5) && !c.hasCharm && !c.hasSummon) return fail(kNoPet);
    }
    const auto held = [&](const Hold& h, uint8_t code) {
        return h.mechanic != 0 ? fail(kPreventedByMechanic, h.mechanic) : fail(code);
    };
    if (c.alive) {
        if ((c.playerBytes & 0x4u) != 0) return fail(kSpellInProgress);
        if (c.charmedByOther && !c.charm.lets) return held(c.charm, kCharmed);
        if ((c.unitFlags & 0x40000u) != 0 && (s.attrEx5 & 0x8u) == 0 && (s.attrEx7 & 0x100000u) == 0 &&
            !c.stun.lets)
            return held(c.stun, kStunned);
        if (s.preventionType == 1 && (c.unitFlags & 0x2000u) != 0 && !c.silence.lets)
            return held(c.silence, kSilenced);
        if (s.preventionType == 2 && (c.unitFlags & 0x20000u) != 0 && !c.pacify.lets)
            return held(c.pacify, kPacified);
        if ((c.unitFlags & 0x800000u) != 0 && (s.attrEx5 & 0x20000u) == 0 && (s.attrEx7 & 0x100000u) == 0 &&
            !c.fear.lets)
            return held(c.fear, kFleeing);
        if ((c.unitFlags & 0x400000u) != 0 && (s.attrEx5 & 0x40000u) == 0 && (s.attrEx7 & 0x100000u) == 0 &&
            !c.confuse.lets)
            return held(c.confuse, kConfused);
    }
    const bool channeled = (s.attrEx & 0x44u) != 0;
    // AURA_INTERRUPT_FLAG_NOT_SHEATHED (0x200), the channel's as well.
    if (c.sheath == 0 && ((s.auraInterruptFlags & 0x200u) != 0 ||
                          (channeled && (s.channelInterruptFlags & 0x200u) != 0)))
        return fail(kNotUnsheathed);
    // NOT_MOUNTED (0x40).
    if (!c.mounted && ((s.auraInterruptFlags & 0x40u) != 0 || (channeled && (s.channelInterruptFlags & 0x40u) != 0)))
        return fail(kOnlyMounted);
    if ((s.attr & 0x1000000u) == 0 && c.mounted && !c.mountableCreature && !c.wouldDismount)
        return fail(kNotMounted);
    if ((s.attr & 0x8000000u) == 0 && c.standState != 0 && !c.wouldStand) return fail(kNotStanding);
    if (c.daytime >= 0) {
        if ((s.attr & 0x1000u) != 0 && c.daytime == 0) return fail(kOnlyDaytime);
        if ((s.attr & 0x2000u) != 0 && c.daytime == 1) return fail(kOnlyNighttime);
    }
    const bool swimming = (c.moveFlags & 0x200000u) != 0;
    // NOT_UNDERWATER (0x100), NOT_ABOVEWATER (0x80).
    if (!swimming && ((s.auraInterruptFlags & 0x100u) != 0 || (channeled && (s.channelInterruptFlags & 0x100u) != 0)))
        return fail(kOnlyUnderwater);
    if (swimming && ((s.auraInterruptFlags & 0x80u) != 0 || (channeled && (s.channelInterruptFlags & 0x80u) != 0)))
        return fail(kOnlyAbovewater);
    // On the move (0xc0100f: the four directions, falling, rising,
    // sinking), not a spell at a place: a channel broken by moving or
    // turning; or one broken by movement (InterruptFlags 0x1), not
    // AttributesEx2 0x20, with a cast time or an aura broken by moving.
    const bool moving = (c.moveFlags & 0xc0100fu) != 0;
    if (moving && (s.targets & 0x60u) == 0) {
        if (channeled && (s.channelInterruptFlags & 0x18u) != 0) return fail(kMoving);
        if ((s.interruptFlags & 0x1u) != 0 && (s.attrEx2 & 0x20u) == 0 &&
            (c.hasCastTime || (s.auraInterruptFlags & 0x18u) != 0))
            return fail(kMoving);
    }
    // 0x0072bac0, unless 0x0071b130 would leave the form for it.
    if (const uint8_t code = formFailure(s, c); code != 0 && (code == kOnlyShapeshift || !c.wouldUnshift))
        return fail(code);
    // 0x0072bc20: Attributes 0x20000 only stealthed, without an aura 275.
    if ((s.attr & 0x20000u) != 0 && (c.visFlags & 0x2u) == 0 && !c.ignoresForm) return fail(kOnlyStealthed);
    if ((s.attr & 0x10000000u) != 0 && (c.unitFlags & 0x80000u) != 0 && !c.ignoresAuraState)
        return fail(kAffectingCombat);
    const auto hasState = [&](uint32_t st) { return (c.auraState & (1u << ((st - 1) & 31))) != 0; };
    if (s.casterAuraState != 0 && !hasState(s.casterAuraState) && !c.ignoresAuraState) return fail(kCasterAurastate);
    if (s.casterAuraStateNot != 0 && hasState(s.casterAuraStateNot) && !c.ignoresAuraState)
        return fail(kCasterAurastate);
    if (s.casterAuraSpell != 0 && !c.hasCasterAuraSpell && !c.ignoresAuraState) return fail(kCasterAurastate);
    if (s.excludeCasterAuraSpell != 0 && c.hasExcludedAuraSpell && !c.ignoresAuraState)
        return fail(kCasterAurastate);
    if (s.minFactionId != 0 && s.minReputation < 8 && c.reputation < kRankStart[s.minReputation])
        return fail(kReputation);
    if (static_cast<int32_t>(s.areaGroupId) > 0 && c.area > 0)
        return c.area == 2 ? fail(kRequiresArea, c.areaArg) : fail(kIncorrectArea);
    // Attributes 0xc000 (0x0071b7f0): indoors, one only for outdoors
    // (0x8000) is ONLY_OUTDOORS, NO_MOUNTS_ALLOWED for a mount (aura 78,
    // 0x007fde50); outdoors, one only for indoors (0x4000) ONLY_INDOORS.
    if ((s.attr & 0xc000u) != 0 && c.outdoors >= 0) {
        if (c.outdoors == 0) {
            if ((s.attr & 0x8000u) != 0)
                return fail(s.aura[0] == 78 || s.aura[1] == 78 || s.aura[2] == 78 ? kNoMountsAllowed : kOnlyOutdoors);
        } else if ((s.attr & 0x4000u) != 0) {
            return fail(kOnlyIndoors);
        }
    }
    // AttributesEx4 0x4000000: where flying is (0x007261b0).
    if ((s.attrEx4 & 0x4000000u) != 0 && c.flyable == 0) return fail(kNotHere);
    if (c.mapType >= 0) {
        if ((s.attrEx3 & 0x800u) != 0 && c.mapType != 3) return fail(kOnlyBattlegrounds);
        if ((s.attrEx6 & 0x800u) != 0 && c.mapType == 2) return fail(kNotInRaidInstance);
    }
    // AttributesEx2 0x10000 (taming): not with PLAYER_FIELD_BYTES 0x1.
    if ((s.attrEx2 & 0x10000u) != 0 && (c.playerBytes & 0x1u) != 0)
        return {kDontReport, 0, GameError::TameFailedTooMany, 0};
    return {};
}

/// 0x0080cce0 -> 0x00805f60: a press of the spell already pending - its
/// cast running - is dropped without a word while it is not ready
/// (0x00809000) or waits on the next swing (Attributes 0x4 | 0x400). Any
/// other press goes on to the checks and is sent: neither 0x0080cce0 nor
/// 0x0080ac90 asks after the running cast.
[[nodiscard]] constexpr bool dropsPendingPress(bool samePending, bool notReady, uint32_t attributes) {
    return samePending && (notReady || (attributes & 0x404u) != 0);
}

/// 0x00809af0 fails the pending cast its count names (0x008051d0), not the
/// running one: a spell sent during a cast and refused SPELL_IN_PROGRESS, or
/// any other spell's failure, leaves the running cast as it was.
[[nodiscard]] constexpr bool failureEndsRunningCast(bool casting, uint8_t result, bool sameSpell) {
    return !casting || (result != kSpellInProgress && sameSpell);
}

}  // namespace wowee::game::cast_check
