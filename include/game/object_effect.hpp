#pragma once

// A model's object effects (ObjectEffect.cpp): the sounds a vehicle, a lift or
// a gunship makes by the state it is in. CreatureDisplayInfo's (or
// GameObjectDisplayInfo's) ObjectEffectPackageID names a package; its
// ObjectEffectPackageElem rows give, for each state, an ObjectEffectGroup;
// the group's ObjectEffect rows are what that state sounds of (0x006f66b0).
//
// Entering a state (0x006f7270) plays the group's TriggerType 1 effects once
// and starts its TriggerType 2 ones looping, each loop shared by every state
// that asks for it; leaving it (0x006f61d0) plays the TriggerType 3 effects
// once and lets go of its loops, which end when nothing holds them. A state's
// entry or exit is only acted on when it changes.
//
// The states come from the object: a unit's movement flags (0x00721300), the
// animations it plays (0x0071e5b0 through the table at 0x006f17f0), a
// transport's route state (0x0070b390).

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace wowee::game::object_effect {

inline constexpr uint32_t kTriggerOnEnter = 1;
inline constexpr uint32_t kTriggerLoop = 2;
inline constexpr uint32_t kTriggerOnLeave = 3;
/// EffectRecType 1: EffectRecID is a SoundEntries row.
inline constexpr uint32_t kRecSound = 1;

/// An ObjectEffect row: ID, Name, Group, TriggerType, EventType,
/// EffectRecType, EffectRecID, Attachment, Offset xyz, Modifier.
struct Effect {
    uint32_t id = 0;
    uint32_t group = 0;
    uint32_t trigger = 0;
    uint32_t recType = 0;
    uint32_t recId = 0;
    int32_t attachment = -1;
    std::array<float, 3> offset{};
    uint32_t modifier = 0;
};

/// An ObjectEffectModifier row: InputType, MapType, OutputType and the
/// mapping's four values (0x006f1790).
struct Modifier {
    uint32_t input = 0;
    uint32_t map = 0;
    uint32_t output = 0;
    std::array<float, 4> param{};
};

/// 0x006f1790 with InputType 1 (the object's speed, its vtable +0xf8) through
/// MapType 1 (0x006f1720: Param[2] up to Param[0], Param[3] from Param[1],
/// straight between) to OutputType 1 (the sound's pitch, 0x004cfb50). Nothing
/// for any other type: MapType 0 and OutputType 0 do nothing.
inline bool modifiedPitch(const Modifier& m, float speed, float& pitch) {
    if (m.input != 1 || m.map != 1 || m.output != 1) return false;
    const auto& p = m.param;
    if (speed <= p[0]) pitch = p[2];
    else if (speed >= p[1]) pitch = p[3];
    else pitch = (p[3] - p[2]) * ((speed - p[0]) / (p[1] - p[0])) + p[2];
    return true;
}

/// The tables, as 0x006f66b0 arranges them.
struct Tables {
    /// package -> (state, group) in file order
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> package;
    /// group -> its effects in file order
    std::unordered_map<uint32_t, std::vector<Effect>> group;
    std::unordered_map<uint32_t, Modifier> modifier;

    std::vector<uint32_t> groupsOf(uint32_t packageId, uint32_t state) const {
        std::vector<uint32_t> out;
        auto it = package.find(packageId);
        if (it == package.end()) return out;
        for (const auto& [s, g] : it->second)
            if (s == state) out.push_back(g);
        return out;
    }
};

/// What an object is to do with its sounds.
struct Action {
    enum class Kind : uint8_t { PlayOnce, StartLoop, StopLoop } kind = Kind::PlayOnce;
    const Effect* effect = nullptr;
};

inline constexpr size_t kStateCount = 128;
using States = std::bitset<kStateCount>;

/// One object's effects: the states it is in and the loops it holds.
class Runner {
public:
    /// Move to `wanted` among the states in `scope` (the ones this source
    /// decides): leave those no longer wanted, then enter the new ones.
    void apply(const Tables& t, uint32_t packageId, const States& wanted, const States& scope,
               std::vector<Action>& out) {
        for (size_t s = 0; s < kStateCount; ++s)
            if (scope[s] && states_[s] && !wanted[s]) leave(t, packageId, static_cast<uint32_t>(s), out);
        for (size_t s = 0; s < kStateCount; ++s)
            if (scope[s] && !states_[s] && wanted[s]) enter(t, packageId, static_cast<uint32_t>(s), out);
    }
    /// 0x006f7270(state, 1, 0).
    void enter(const Tables& t, uint32_t packageId, uint32_t state, std::vector<Action>& out) {
        if (state >= kStateCount || states_[state]) return;
        states_[state] = true;
        for (uint32_t g : t.groupsOf(packageId, state)) {
            auto it = t.group.find(g);
            if (it == t.group.end()) continue;
            for (const Effect& e : it->second)
                if (e.trigger == kTriggerOnEnter) out.push_back({Action::Kind::PlayOnce, &e});
            for (const Effect& e : it->second) {
                if (e.trigger != kTriggerLoop) continue;
                if (holds_[e.id]++ == 0) out.push_back({Action::Kind::StartLoop, &e});
            }
        }
    }
    /// 0x006f61d0(state, 1).
    void leave(const Tables& t, uint32_t packageId, uint32_t state, std::vector<Action>& out) {
        if (state >= kStateCount || !states_[state]) return;
        states_[state] = false;
        for (uint32_t g : t.groupsOf(packageId, state)) {
            auto it = t.group.find(g);
            if (it == t.group.end()) continue;
            for (const Effect& e : it->second)
                if (e.trigger == kTriggerOnLeave) out.push_back({Action::Kind::PlayOnce, &e});
            for (const Effect& e : it->second) {
                if (e.trigger != kTriggerLoop) continue;
                auto h = holds_.find(e.id);
                if (h == holds_.end() || h->second == 0) continue;
                if (--h->second == 0) {
                    holds_.erase(h);
                    out.push_back({Action::Kind::StopLoop, &e});
                }
            }
        }
    }
    const States& states() const { return states_; }

private:
    States states_;
    std::map<uint32_t, uint32_t> holds_;
};

/// 0x00721300's states from a unit's movement flags, and the states it
/// decides. `airborne` is 0x0071c8b0 (more than twice its height off the
/// ground); a unit neither airborne nor near the ground is neither.
inline void movementStates(uint32_t flags, bool airborne, bool nearGround, States& wanted, States& scope) {
    for (uint32_t s : {6u, 7u, 0x25u, 0x26u, 0x2fu, 0x30u}) scope[s] = true;
    for (uint32_t s = 0x31; s <= 0x42; ++s) scope[s] = true;
    wanted[(flags & 0xfu) == 0 ? 0x26 : 0x25] = true;
    if (flags & 0x400000u) wanted[6] = true;
    else if (flags & 0x800000u) wanted[7] = true;
    wanted[(flags & 0x30u) == 0 ? 0x30 : 0x2f] = true;
    uint32_t base = 0;
    if (flags & 0x2000000u) base = 0x3d;        // flying
    else if (airborne) base = 0x31;
    else if (nearGround) base = 0x37;
    if (base == 0) return;
    wanted[base] = true;
    if (flags & 1u) wanted[base + 1] = true;    // forward
    if (flags & 2u) wanted[base + 2] = true;    // backward
    if (flags & 0xcu) {
        wanted[base + 5] = true;                // strafing
        if (flags & 4u) wanted[base + 3] = true;
        if (flags & 8u) wanted[base + 4] = true;
    }
}

/// The table at 0x006f17f0 (filled at 0x006f1840): the state an animation
/// puts a model in, 0 for none. The second column is read for variation 1 of
/// the sequence (0x008266b0's +0x94, passed as 0x006f17f0's second
/// argument); only Stand and anim 41 have anything in it.
inline uint32_t animationState(uint32_t animationId, uint32_t variation) {
    struct Row { uint16_t anim; uint8_t state, altState; };
    static constexpr Row kRows[] = {
        {0, 19, 41}, {1, 15, 0}, {4, 13, 0}, {5, 12, 0}, {6, 81, 0}, {8, 17, 0}, {9, 14, 0},
        {10, 39, 0}, {11, 20, 0}, {12, 21, 0}, {13, 22, 0}, {16, 40, 0}, {25, 18, 0}, {37, 23, 0},
        {38, 36, 0}, {39, 24, 0}, {40, 25, 0}, {41, 26, 42}, {42, 27, 0}, {43, 28, 0}, {44, 29, 0},
        {45, 30, 0}, {53, 16, 0}, {92, 32, 0}, {93, 31, 0}, {96, 45, 0}, {97, 43, 0}, {98, 44, 0},
        {107, 8, 0}, {108, 11, 0}, {111, 9, 0}, {112, 10, 0}, {135, 33, 0}, {143, 34, 0},
        {146, 73, 0}, {147, 74, 0}, {148, 71, 0}, {149, 72, 0}, {150, 75, 0}, {151, 76, 0},
        {153, 77, 0}, {154, 78, 0}, {155, 79, 0}, {156, 80, 0}, {162, 68, 0}, {163, 69, 0},
        {164, 70, 0}, {187, 35, 0}, {193, 46, 0}, {226, 67, 0},
    };
    for (const Row& r : kRows)
        if (r.anim == animationId) return variation == 1 ? r.altState : r.state;
    return 0;
}

/// The states the animation table can name: the ones 0x0071e5b0 decides.
inline States animationScope() {
    States s;
    for (uint32_t a = 0; a < 0x1fa; ++a) {
        if (uint32_t st = animationState(a, 0)) s[st] = true;
        if (uint32_t st = animationState(a, 1)) s[st] = true;
    }
    return s;
}

/// A moving object's state among 2..5 - waiting, speeding up, moving,
/// slowing down - entered and the others left in the client's order: a
/// transport's route state (0x0070b390: 0, 0xa2, 0xa3, 0xa4) and a doodad's
/// $EWT, $EAC, $EMV and $EDC (0x007bd5a0) say the same four.
inline void moveState(Runner& r, const Tables& t, uint32_t packageId, uint32_t state, std::vector<Action>& out) {
    static constexpr uint32_t kLeave[4][3] = {{3, 4, 5}, {2, 4, 5}, {3, 2, 5}, {3, 4, 2}};
    if (state < 2 || state > 5) return;
    r.enter(t, packageId, state, out);
    for (uint32_t s : kLeave[state - 2]) r.leave(t, packageId, s, out);
}
/// The route state 0x007f82b0 hands 0x0070b390, as a state.
inline uint32_t routeMoveState(uint32_t routeState) {
    switch (routeState) {
    case 0: return 2;
    case 0xa2: return 3;
    case 0xa3: return 4;
    case 0xa4: return 5;
    default: return 0;
    }
}

/// What a unit answers as its modifier input (InputType 1, the object's
/// vtable +0xf8): CGUnit_C and CGPlayer_C both leave it the base's
/// 0x006f1700, -1 - so a unit's loop sits at the mapping's low end.
inline constexpr float kUnitModifierInput = -1.0f;

/// Death (15) or 0x51 among the animations' states takes the unit out of
/// every movement state (0x0071e5b0).
inline bool endsMovementStates(uint32_t state) { return state == 15 || state == 0x51; }

}  // namespace wowee::game::object_effect
