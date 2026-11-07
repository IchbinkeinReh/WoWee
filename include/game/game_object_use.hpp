#pragma once

// Using a game object, as the client decides it: what may be used and what
// a lock asks for (GameObject_C.cpp).
//
// A right click on an object (0x0070f680) asks the object's type handler
// whether it can be used (0x00711470) and says the refusal when not, uses it
// when so (0x007116c0), and then reports the use (CMSG_GAMEOBJ_REPORT_USE)
// whatever the use did. Using it opens it by the lock's walk (0x0070f160):
// a key the player carries, a known spell of the lock's type strong enough
// for it, or the spell the lock names, cast at the object - or a plain
// CMSG_GAMEOBJ_USE when the object has no lock or nothing opens it but the
// object itself. When the walk finds a lock it cannot open, the use says what
// it wanted from the lock's first slot.

#include <cstdint>

namespace wowee::game::go_use {

/// GAMEOBJECT_FLAGS 0x2 (GO_FLAG_LOCKED).
constexpr uint32_t kFlagLocked = 0x2;

/// The game errors the use says (the table at 0x00ac8af0).
namespace err {
constexpr uint16_t kPlayerDead = 0x87;
constexpr uint16_t kClientLockedOut = 0x88;
constexpr uint16_t kChestInUse = 0xe8;
constexpr uint16_t kCantOpen = 0xe9;
constexpr uint16_t kUseLocked = 0xea;
constexpr uint16_t kDoorLocked = 0xeb;
constexpr uint16_t kButtonLocked = 0xec;
constexpr uint16_t kLockedWithItem = 0xed;
constexpr uint16_t kLockedWithSpell = 0xee;
constexpr uint16_t kLockedWithSpellKnown = 0xef;
constexpr uint16_t kTooFar = 0xf0;
constexpr uint16_t kCantImmune = 0x181;
constexpr uint16_t kGenericStunned = 0x1b2;
constexpr uint16_t kNotWhileMounted = 0x1c0;
constexpr uint16_t kNotWhileShapeshifted = 0x1c1;
constexpr uint16_t kNotInCombat = 0x1c2;
constexpr uint16_t kPreventedByMechanic = 0x200;
}  // namespace err

/// The data fields 0x00746190 is asked for, by the id the client gives
/// each: the lock (4), "usable mounted" (0x5b), "not in combat" (0x3e) and
/// "not while immune" (0x39).
enum class Field : uint8_t { Lock, Mounted, NoCombat, NoImmune };

/// Which of a type's 24 data fields holds that field (the table at
/// 0x00a36f34), -1 where the type has none.
[[nodiscard]] constexpr int dataIndex(uint32_t goType, Field field) {
    switch (field) {
        case Field::Lock:
            switch (goType) {
                case 0: case 1: return 1;    // door, button
                case 2: case 3: case 6: case 10: case 12: case 13: case 24: case 26: return 0;
                case 25: return 4;           // fishing hole
                default: return -1;
            }
        case Field::Mounted:
            switch (goType) {
                case 2: return 8;
                case 9: return 3;
                case 10: return 17;
                case 22: return 3;
                default: return -1;
            }
        case Field::NoCombat:
            return goType == 3 ? 12 : -1;
        case Field::NoImmune:
            switch (goType) {
                case 0: return 3;
                case 1: return 4;
                case 2: return 5;
                case 10: return 11;
                case 24: return 5;
                case 26: return 3;
                default: return -1;
            }
    }
    return -1;
}

/// The type handler's reach and the error a locked object refuses with
/// (the constructors 0x00714250 picks: 0x007124b0 and those built on it).
struct Handler {
    float reach = 5.0f;
    uint16_t lockedError = err::kUseLocked;
};
[[nodiscard]] constexpr Handler handlerFor(uint32_t goType) {
    constexpr float kTalk = 5.5555553f;  // 0x40b1c71c
    switch (goType) {
        case 0: return {5.0f, err::kDoorLocked};
        case 1: return {5.0f, err::kButtonLocked};
        case 2: case 9: case 23: case 24: case 26: case 27: return {kTalk, err::kUseLocked};
        case 4: return {10.0f, err::kUseLocked};
        case 7: case 32: return {3.0f, err::kUseLocked};
        case 12: return {0.0f, err::kUseLocked};
        case 17: return {100.0f, err::kUseLocked};
        default: return {5.0f, err::kUseLocked};
    }
}

/// 0x0070ef90: whether a lock slot's Action fits the object as it is - its
/// GAMEOBJECT_BYTES_1 state and its flags. Action 4 wants state 2; actions
/// 0, 1 and 3 want state 1; none but 4 is done in state 2; 0 wants the
/// object unlocked and 1 locked; 2 wants state 0.
[[nodiscard]] constexpr bool actionFits(uint32_t action, uint32_t state, uint32_t flags) {
    if (action == 4) return state == 2;
    if (state == 2) return false;
    if ((action == 0 || action == 1 || action == 3) && state != 1) return false;
    if (action == 0) return (flags & kFlagLocked) == 0;
    if (action == 1) return (flags & kFlagLocked) != 0;
    if (action == 2) return state == 0;
    return true;
}

/// A Lock.dbc row: Type, Index, Skill and Action for each of eight slots.
struct LockRow {
    uint32_t type[8] = {};
    uint32_t index[8] = {};
    uint32_t skill[8] = {};
    uint32_t action[8] = {};
};

/// SPELL_EFFECT_OPEN_LOCK.
constexpr uint32_t kEffectOpenLock = 0x21;

/// What the walk found.
struct Opening {
    bool locked = false;    ///< some slot wanted something and nothing opened it
    uint32_t spell = 0;     ///< the spell it opens with, or the last one tried
    int32_t value = 0;      ///< that spell's effect value
    int32_t required = 0;   ///< what the slot wanted of it
    uint32_t keyItem = 0;   ///< the key carried, when a key opens it
    int slot = -1;          ///< the slot that opened it
};

/// 0x0070f160 over a lock row, for an object in `state` with `flags` and
/// `level`. `known` is the player's spells in order, `targeting` the spell
/// on the cursor (0x00d3f4e4, 0 for none), tried after each known spell.
/// `effects(id, e, m)` fills a spell's three effects and misc values,
/// answering false for no spell; `value(id, k)` is effect k's value
/// (0x008016c0); `carries(item)` whether the player has the item, and
/// `keySpell(item)` the item's own OPEN_LOCK spell (0x00707c60), 0 for none
/// or an item not yet described.
template <class Known, class Effects, class Value, class Carries, class KeySpell>
[[nodiscard]] Opening evaluate(const LockRow& lock, uint32_t state, uint32_t flags, int32_t level,
                               const Known& known, uint32_t targeting, Effects&& effects,
                               Value&& value, Carries&& carries, KeySpell&& keySpell) {
    Opening o;
    bool wants = false;
    // A spell of the lock's type: true where it opens the slot.
    const auto tries = [&](uint32_t spellId, int i) {
        uint32_t e[3] = {}, m[3] = {};
        if (!effects(spellId, e, m)) return false;
        for (int k = 0; k < 3; ++k) {
            if (e[k] != kEffectOpenLock || m[k] != lock.index[i]) continue;
            const int32_t required = lock.skill[i] != 0 ? static_cast<int32_t>(lock.skill[i]) : level * 5;
            o.spell = spellId;
            o.value = value(spellId, k);
            o.required = required;
            if (required <= o.value) return true;
        }
        return false;
    };
    for (int i = 0; i < 8; ++i) {
        const uint32_t type = lock.type[i];
        if (type == 2) {
            wants = true;
            if (!actionFits(lock.action[i], state, flags)) continue;
            for (const uint32_t spellId : known) {
                if (tries(spellId, i) || (targeting != 0 && tries(targeting, i))) {
                    o.slot = i;
                    return o;
                }
            }
        } else if (type == 3) {
            wants = true;
            if (!actionFits(lock.action[i], state, flags)) continue;
            uint32_t e[3] = {}, m[3] = {};
            if (!effects(lock.index[i], e, m)) continue;
            for (int k = 0; k < 3; ++k) {
                if (e[k] == kEffectOpenLock) {
                    o.spell = lock.index[i];
                    o.slot = i;
                    return o;
                }
            }
        } else if (type == 1) {
            wants = true;
            if (!actionFits(lock.action[i], state, flags) || !carries(lock.index[i])) continue;
            o.spell = keySpell(lock.index[i]);
            o.keyItem = lock.index[i];
            o.slot = i;
            return o;
        }
    }
    o.locked = wants;
    return o;
}

/// The refusal 0x007116c0 says for a lock it cannot open, from the row's
/// first slot: 0 for none (an action that does not fit, unless the object
/// is a chest in state 0 - ERR_CHEST_IN_USE).
[[nodiscard]] constexpr uint16_t lockedRefusal(const LockRow& lock, uint32_t goType, uint32_t state,
                                               uint32_t flags, bool spellFound) {
    if (!actionFits(lock.action[0], state, flags))
        return goType == 3 && state == 0 ? err::kChestInUse : 0;
    switch (lock.type[0]) {
        case 1: return err::kLockedWithItem;
        case 2: return spellFound ? err::kLockedWithSpellKnown : err::kLockedWithSpell;
        case 3: return err::kLockedWithSpell;
        default: return err::kCantOpen;
    }
}

/// 0x007ff770 and 0x008016c0 for the player: an effect's least value, the
/// base points plus one and the points per level over the spell's level -
/// which for the player is its skill in the spell's line over five
/// (0x006dc330, the level times five where the spell is on no line), capped
/// at MaxLevel times five, less BaseLevel - before the spell's modifiers.
[[nodiscard]] constexpr float effectValue(int32_t basePoints, float pointsPerLevel, int32_t casterValue,
                                          int32_t baseLevel, int32_t maxLevel) {
    int32_t v = casterValue;
    if (maxLevel > 0 && maxLevel * 5 <= v) v = maxLevel * 5;
    if (v < 0) v = 0;
    int32_t level = v / 5;
    if (baseLevel > 0) level -= baseLevel;
    if (level < 0) level = 0;
    return static_cast<float>(basePoints + 1) + pointsPerLevel * static_cast<float>(level);
}

}  // namespace wowee::game::go_use
