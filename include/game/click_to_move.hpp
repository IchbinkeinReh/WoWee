#pragma once

// Click-to-move: the mover sent somewhere by a click and steered there by the
// client, with the same movement packets the keys send. One state for the
// whole client (Unit_C statics from 0x00ca11a8 to 0x00ca1270), driven each
// frame by 0x007317a0. Everything here is in the client's own world frame
// (server coordinates, facing as atan2(dy, dx)); the caller converts.
//
// Pure: the table, the reach per kind and the frame step, so they can be
// tested without a world. GameHandler (game_handler_click_to_move.cpp) feeds
// it the mover and acts on what it answers.

#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace wowee {
namespace game {
namespace click_to_move {

inline constexpr float kPi = 3.1415927f;
inline constexpr float kTwoPi = 6.2831855f;

/// What the mover was sent to do (0x00ca11f4). 13 is nothing.
enum class Kind : uint8_t {
    Kind0 = 0,           // face the target through a cast (Attributes 0x400000; 0x00806700)
    Kind1 = 1,           // face the channel's unit (AttributesEx 0x4000; 0x0072bc70, 0x0073a520)
    Face = 2,            // turn to a facing and stop (0x0072b660, 0x0072e430)
    Follow = 3,          // /follow (0x0072b4a0 case 3)
    Move = 4,            // to a place on the ground (0x0072b6d0)
    NpcInteract = 5,     // talk to a unit (0x006ddbb0)
    Loot = 6,            // loot a corpse (0x006dad90)
    ObjectInteract = 7,  // use a game object (0x0070f680 -> 0x0072b730)
    FaceDirection = 8,   // a click on nothing: run that way (0x0072f040)
    Skin = 9,            // skin, mine or gather a corpse (0x006d6f40)
    AttackPosition = 10, // close in to melee a unit where it stood (0x0072ee20)
    AttackGuid = 11,     // close in to shoot a unit (0x0072ee20 with autoRangedCombat)
    None = 13,
};

/// 0x00717b60: a unit the player may loot now - dead (UNIT_FIELD_HEALTH
/// below 1 as a signed number), its loot not held back to a later time
/// (+0xac0, zero for none, the time it was seen dying: never later than now
/// where the client sets it) and UNIT_DYNFLAG_LOOTABLE.
inline bool lootableNow(int32_t health, uint32_t lootFromMs, uint32_t nowMs, uint32_t dynamicFlags) {
    if (health >= 1) return false;
    if (lootFromMs != 0 && static_cast<int32_t>(nowMs - lootFromMs) < 0) return false;
    return (dynamicFlags & 0x1u) != 0;
}

/// 0x0071bc50: whether `at` is before a unit at `from` facing `facing`
/// (server radians): the way to it and the way it faces not apart by more
/// than a right angle.
inline bool inFrontHalf(const glm::vec3& from, float facing, const glm::vec3& at) {
    const glm::vec3 d = at - from;
    return std::cos(facing) * d.x + std::sin(facing) * d.y >= 0.0f;
}

/// A row of the table at 0x00adaab8 (12 bytes a kind): whether the kind
/// goes anywhere at all, the square of the distance past which it gives up
/// (0 for none), whether a mover that stops getting anywhere is stopped, and
/// whether the kind has no arrival.
struct Row {
    bool used = false;          // +0
    float maxDistSq = 0.0f;     // +4
    bool stuckCheck = false;    // +8
    bool noArrival = false;     // +9
};

inline constexpr std::array<Row, 12> kRows = {{
    {false, 0.0f, false, false},    // 0
    {false, 0.0f, false, false},    // 1
    {false, 0.0f, false, false},    // 2 Face
    {true, 900.0f, false, false},   // 3 Follow
    {true, 0.0f, true, false},      // 4 Move
    {true, 6400.0f, true, false},   // 5 NpcInteract
    {true, 6400.0f, true, false},   // 6 Loot
    {true, 6400.0f, true, false},   // 7 ObjectInteract
    {true, 0.0f, true, true},       // 8 FaceDirection
    {true, 6400.0f, true, false},   // 9 Skin
    {true, 6400.0f, true, false},   // 10 AttackPosition
    {true, 0.0f, true, false},      // 11 AttackGuid
}};

inline Row row(Kind k) {
    const auto i = static_cast<size_t>(k);
    return i < kRows.size() ? kRows[i] : Row{};
}

/// 0x00715c70: a kind with a limit is not started at or past it (the
/// caller says ERR_AUTOFOLLOW_TOO_FAR, game error 0x139). A straight-line
/// distance, height included.
inline bool startsInRange(Kind k, const glm::vec3& mover, const glm::vec3& at) {
    const Row r = row(k);
    if (!r.used || r.maxDistSq == 0.0f) return true;
    const glm::vec3 d = at - mover;
    return glm::dot(d, d) < r.maxDistSq;
}

/// 0x004f5f40: the distance two units interact at - both combat reaches
/// and four thirds of a yard, never under five yards.
inline float meleeReach(float combatReachA, float combatReachB) {
    const float r = combatReachA + combatReachB + 1.3333334f;
    return r < 5.0f ? 5.0f : r;
}

/// What 0x0071b3b0 needs of the kind's subject.
struct ReachInput {
    /// The subject unit's combat reach, when it is there (5, 6, 10).
    std::optional<float> targetCombatReach;
    /// The mover's own combat reach (6, 10).
    float moverCombatReach = 0.0f;
    /// A spell's or an object's reach, when there is one (9, 11): the
    /// skinning spell's or the ranged attack's maximum range.
    std::optional<float> spellRange;
    /// 0x00ca11e8: the object reach the last object approach left behind
    /// (7). 0x0072b730 sets it only after the approach has started, so a
    /// start reads the one before.
    float objectReach = 0.0f;
};

/// 0x0071b3b0: the square of how near the mover must come before it has
/// arrived. Nothing when the kind cannot start (9 and 11 without a range, an
/// unknown kind).
inline std::optional<float> reachSq(Kind k, const ReachInput& in) {
    const Row r = row(k);
    if (!r.used || r.noArrival) return 0.0f;
    switch (k) {
    case Kind::Follow:
        return 9.0f;    // 0x00adaaa0
    case Kind::Move:
        return 0.25f;   // 0x00adaaac
    case Kind::NpcInteract: {
        // Half the unit's combat reach and two yards; with the unit gone,
        // (25/9)^2 (0x00ca1270, set from 0x00adaa9c at start-up).
        if (!in.targetCombatReach) return (25.0f / 9.0f) * (25.0f / 9.0f);
        const float d = *in.targetCombatReach * 0.5f + 2.0f;
        return d * d;
    }
    case Kind::Loot:
    case Kind::AttackPosition: {
        if (!in.targetCombatReach) return 25.0f;
        const float d = meleeReach(*in.targetCombatReach, in.moverCombatReach) - 1.3333334f;
        return d * d;
    }
    case Kind::ObjectInteract:
        return in.objectReach * in.objectReach;
    case Kind::Skin:
    case Kind::AttackGuid: {
        if (!in.spellRange) return std::nullopt;
        const float d = *in.spellRange * 0.9f;
        return d * d;
    }
    default:
        return std::nullopt;
    }
}

/// An angle into [0, 2pi) (0x004c5090).
inline float wrapTwoPi(float a) {
    a = std::fmod(a, kTwoPi);
    if (a < 0.0f) a += kTwoPi;
    return a;
}

/// 0x004f5130: the facing from one place to another, flat.
inline float facingTo(const glm::vec3& from, const glm::vec3& to) {
    const float dy = to.y - from.y;
    const float dx = to.x - from.x;
    if (std::fabs(dx) < 2.3841858e-07f) return dy < 0.0f ? 1.5f * kPi : 0.5f * kPi;
    if (std::fabs(dy) < 2.3841858e-07f) return to.x < from.x ? kPi : 0.0f;
    return std::atan2(dy, dx);
}

/// 0x00ca11dc before the first turn: pi, the sentinel 0x00731650 replaces.
inline constexpr float kTurnRateUnset = kPi;

/// 0x00731650's rate for a turn, chosen once on its first frame: 800 degrees
/// a second for a turn of up to 120 degrees either way, 1200 past that.
inline float turnRateFor(float diff) {
    const float deg = (diff <= 2.0943952f || diff >= 4.1887903f) ? 800.0f : 1200.0f;
    return deg * 0.017453292f;
}

struct Turn {
    bool turning = false;   // false: already facing it
    float facing = 0.0f;    // the facing after this frame's step
};

/// 0x00731650: turn the facing toward `desired` by at most `maxStep`, the
/// short way round. `turnRate` is 0x00ca11dc: the sentinel is replaced by
/// the rate for this turn unless the mover turns at its own rate
/// (`fullSpeedTurning`, flags2 0x8, which uses `unitTurnRate`).
inline Turn turnToward(float facing, float desired, float maxStep, float& turnRate,
                       bool fullSpeedTurning, float unitTurnRate) {
    const float diff = wrapTwoPi(desired - facing);
    if (std::fabs(diff) < 0.001f || std::fabs(diff - kTwoPi) < 0.001f) return {false, facing};
    if (fullSpeedTurning) {
        turnRate = unitTurnRate;
    } else if (turnRate == kTurnRateUnset) {
        turnRate = turnRateFor(diff);
    }
    const float amount = diff > kPi ? kTwoPi - diff : diff;
    const float step = amount < maxStep ? amount : maxStep;
    const float turned = diff <= kPi ? facing + step : facing - step;
    return {true, wrapTwoPi(turned)};
}

/// The state 0x00727400 starts and 0x007272c0 ends.
struct State {
    Kind kind = Kind::None;          // 0x00ca11f4
    uint64_t guid = 0;               // 0x00ca11f8
    glm::vec3 dest{0.0f};            // 0x00ca1264
    float facing = 0.0f;             // 0x00ca11ec
    float reachSq = 0.0f;            // 0x00ca11e0
    float reach = 0.0f;              // 0x00ca11e4
    float turnRate = kTurnRateUnset; // 0x00ca11dc
    uint32_t flags = 0;              // 0x00ca1200: 2 once facing it
    int stuck = 0;                   // 0x00ca11d4
    glm::vec3 lastPos{0.0f};         // 0x00ca1258
    uint32_t startMs = 0;            // 0x00ca11f0 at the start, for the marker
};

inline constexpr uint32_t kFlagFacing = 2;
/// 0x00ca1200 4: a swimmer's or flyer's pitch has come to where it wanted.
inline constexpr uint32_t kFlagPitched = 4;

/// 0x00727400's part that is not checks: the state for a start.
inline State start(Kind k, uint64_t guid, const glm::vec3& dest, float facing, float reachSqValue,
                   const glm::vec3& moverPos, bool fullSpeedTurning, float unitTurnRate, uint32_t nowMs) {
    State s;
    s.kind = k;
    s.guid = guid;
    s.dest = dest;
    s.facing = facing;
    s.reachSq = reachSqValue;
    s.reach = std::sqrt(reachSqValue);
    s.turnRate = fullSpeedTurning ? unitTurnRate : kTurnRateUnset;
    s.lastPos = moverPos;
    s.startMs = nowMs;
    return s;
}

/// What 0x007317a0 reads of the world this frame.
struct StepIn {
    glm::vec3 pos{0.0f};       // the mover
    float facing = 0.0f;       // its facing, [0, 2pi)
    float dtSeconds = 0.0f;    // since the last step
    /// The subject's place, for the kinds that follow one (not 2, 4, 8, 10);
    /// nothing when it is gone, or a unit that is dead (Loot and Skin
    /// excepted) or on a taxi.
    std::optional<glm::vec3> subject;
    /// Swimming or flying (0x2200000): distances count height. Can fly
    /// (0x1000000) counts it for the limit as well.
    bool swimOrFly = false;
    bool canFly = false;
    /// Steered with the mouse this frame (0x004f7290 above nothing): the
    /// facing it gives is kept rather than the turn toward the subject.
    bool mouseSteering = false;
    bool movingAny = false;          // flags 0xf
    bool movingForwardBack = false;  // flags 0x3
    bool fullSpeedTurning = false;   // flags2 0x8
    float unitTurnRate = kPi;
    /// 0x00987570: the speed the mover moves at now; the arrival for Move
    /// widens with it.
    float currentSpeed = 7.0f;
    /// The run speed, or the walk speed in walk mode (0x7cc & 0x100).
    float groundSpeed = 7.0f;
    /// 0x005fa9e0 and the rest: alive, not rooted, on no transport, not
    /// stood dead, no spline, a seat that lets it.
    bool mayMove = true;
    /// The mover's pitch (+0x14c), radians up.
    float pitch = 0.0f;
    /// In a liquid (0x0077f1e0, its flag 0x20): how far its surface is over
    /// the mover's feet. And three quarters of the mover's height, the depth
    /// a swimmer floats at (+0x854 * 0.75).
    std::optional<float> liquidAbove;
    float swimDepth = 0.0f;
};

enum class Outcome {
    Continue,
    Stop,     // 0x007272c0(0, 1): given up
    Arrived,  // 0x0072b3a0
};

struct StepOut {
    Outcome outcome = Outcome::Continue;
    std::optional<float> facing;     // the turn this frame (0x00731600)
    std::optional<bool> forward;     // start (true) or stop (false) moving forward
    std::optional<float> pitch;      // a swimmer's or flyer's pitch this frame (0x0072ed40)
    bool takeOff = false;            // one that can fly takes to the air (0x006ef230(t, 1))
};

/// 0x0071b580: the pitch from the mover to a place - straight up or down
/// where the place is right over or under it.
inline float pitchTo(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 d = to - from;
    const float len = std::sqrt(glm::dot(d, d));
    if (len <= 0.0f || (std::fabs(d.x / len) < 0.001f && std::fabs(d.y / len) < 0.001f))
        return to.z <= from.z ? -1.5707964f : 1.5707964f;
    return std::asin(std::clamp(d.z / len, -1.0f, 1.0f));
}

struct PitchStep {
    bool pitching = false;          // the pitch still moving (0x0072f1f0's answer)
    std::optional<float> pitch;     // the pitch to set
    bool farOff = false;            // more than 22.5 degrees from it after the step
};

/// 0x0072f1f0: a swimmer at the surface with its place above levels out (a
/// pitch of 0) and is done; otherwise the pitch steps toward `desired` by at
/// most `maxStep`, and is done once within a thousandth of it.
inline PitchStep stepPitch(float targetZ, float moverZ, float desired, float maxStep, float pitch,
                           std::optional<float> liquidAbove, float swimDepth) {
    PitchStep out;
    if (moverZ < targetZ && liquidAbove && *liquidAbove > 0.0f && std::fabs(*liquidAbove - swimDepth) < 0.001f) {
        if (pitch != 0.0f) out.pitch = 0.0f;
        return out;
    }
    if (std::fabs(desired - pitch) < 0.001f) return out;
    float p = pitch;
    if (desired <= p) {
        p -= maxStep;
        if (!(desired < p)) p = desired;
    } else {
        p += maxStep;
        if (!(p < desired)) p = desired;
    }
    out.pitching = true;
    out.pitch = p;
    out.farOff = std::fabs(p - desired) > 0.3926991f;
    return out;
}

/// 0x0072f040's pitch for a direction clicked into while swimming or
/// flying: straight up or down past 60 degrees, level under 10, else the
/// direction's own (0x00714c10, 0x0088d9b0).
inline float pitchOfDirection(const glm::vec3& unitDir) {
    const float s = std::fabs(unitDir.z);
    if (s > 0.86602f) return unitDir.z < 0.0f ? -1.5707964f : 1.5707964f;
    if (s > 0.17364f) return std::asin(std::clamp(unitDir.z, -1.0f, 1.0f));
    return 0.0f;
}

/// The square of 0x00adaa74: less than this moved in a frame is not moving.
inline constexpr float kStuckMoveSq = 7.716049367445521e-06f;

/// 0x007317a0.
inline StepOut step(State& s, const StepIn& in) {
    StepOut out;
    const Row r = row(s.kind);
    bool hasPlace = true;  // local_7
    glm::vec3 target(0.0f);
    switch (s.kind) {
    case Kind::Face:
    case Kind::FaceDirection:
        hasPlace = false;
        break;
    case Kind::Move:
    case Kind::AttackPosition:
        target = s.dest;
        break;
    default:
        if (!in.subject) {
            out.outcome = Outcome::Stop;
            return out;
        }
        target = *in.subject;
        break;
    }
    const glm::vec3 prev = s.lastPos;
    s.lastPos = in.pos;
    const glm::vec3 d = in.pos - target;
    const float dist3Sq = d.x * d.x + d.y * d.y + d.z * d.z;
    const float distSq = (in.swimOrFly || in.canFly) ? dist3Sq : d.x * d.x + d.y * d.y;
    if (r.used && r.maxDistSq != 0.0f && r.maxDistSq <= distSq) {
        out.outcome = Outcome::Stop;
        return out;
    }
    bool stuckNow = false;
    if (r.used && (s.flags & kFlagFacing) != 0) {
        switch (s.kind) {
        case Kind::Face: case Kind::Follow: case Kind::NpcInteract: case Kind::FaceDirection:
            break;
        default: {
            // Getting no nearer: further from it than the frame before.
            const glm::vec3 p = prev - target;
            if (glm::dot(p, p) < dist3Sq) {
                stuckNow = true;
                if (++s.stuck > 4) {
                    out.outcome = Outcome::Stop;
                    return out;
                }
            }
        }
        }
    }
    const float toward = wrapTwoPi(hasPlace ? facingTo(in.pos, target) : s.facing);  // local_20
    float desired = toward;
    if (in.mouseSteering && in.movingAny && (s.flags & kFlagFacing) != 0) desired = in.facing;
    const float maxStep = std::fmod(s.turnRate * in.dtSeconds, 6.283185307179586f);
    const Turn turn = turnToward(in.facing, desired, maxStep, s.turnRate, in.fullSpeedTurning, in.unitTurnRate);
    float facingNow = in.facing;
    if (turn.turning) {
        out.facing = turn.facing;
        facingNow = turn.facing;
    } else {
        if (s.kind == Kind::Face) {
            out.outcome = Outcome::Stop;
            return out;
        }
        s.flags |= kFlagFacing;
    }
    // A swimmer or flyer going to a place pitches toward it (0x0072f1f0) at
    // pi a second - or, steered with the mouse once there, keeps the pitch
    // it is given - unless it is all but there.
    PitchStep pitch;
    if (in.swimOrFly && hasPlace && kStuckMoveSq < dist3Sq) {
        const bool keepMouse = in.mouseSteering && in.movingForwardBack && (s.flags & kFlagPitched) != 0;
        const float desiredPitch = keepMouse ? in.pitch : pitchTo(in.pos, target);
        const float maxPitchStep = std::fmod(3.1415927f * in.dtSeconds, 6.283185307179586f);
        pitch = stepPitch(target.z, in.pos.z, desiredPitch, maxPitchStep, in.pitch, in.liquidAbove, in.swimDepth);
        out.pitch = pitch.pitch;
        if (!pitch.pitching) s.flags |= kFlagPitched;
    }
    if (!r.used) return out;
    if (in.mouseSteering && (s.flags & kFlagFacing) != 0) {
        // Steered round until the subject is behind: given up.
        const float off = wrapTwoPi(toward - facingNow);
        if (off > 2.7925267f && off < 3.8397243f) {
            out.outcome = Outcome::Stop;
            return out;
        }
    }
    if (in.movingForwardBack && !turn.turning && !pitch.pitching && r.stuckCheck) {
        const glm::vec3 m = in.pos - prev;
        if (glm::dot(m, m) < kStuckMoveSq) {
            stuckNow = true;
            if (++s.stuck > 4) {
                out.outcome = Outcome::Stop;
                return out;
            }
        }
    }
    if (!r.noArrival) {
        float reach = s.reach;
        if (s.kind == Kind::Move) {
            const float f = in.currentSpeed * 0.14285715f;
            reach *= (f >= 1.0f ? f : 1.0f);
        }
        if (distSq <= reach * reach) {
            out.outcome = Outcome::Arrived;
            return out;
        }
    }
    if (!stuckNow) s.stuck = 0;
    // One that can fly, on the ground, going to a place more than half a
    // yard off and steeper than 45 degrees, takes to the air (0x006ef230)
    // and does nothing else this frame.
    if (!pitch.pitching && hasPlace && !in.swimOrFly && in.canFly) {
        const glm::vec3 v = target - in.pos;
        const float lenSq = glm::dot(v, v);
        if (2.0f < lenSq && 0.70710677f < std::fabs(v.z / std::sqrt(lenSq))) {
            out.takeOff = true;
            return out;
        }
    }
    // Pitching still, more than 22.5 degrees off, it does not go forward.
    const bool forward = (!pitch.pitching || !pitch.farOff) && in.mayMove;
    if (forward != in.movingForwardBack) {
        if (!forward) {
            out.forward = false;
        } else if (s.kind != Kind::Follow) {
            out.forward = true;
        } else {
            float f = in.groundSpeed * 0.14285715f;
            if (f < 1.0f) f = 1.0f;
            const float startSq = f * 4.5f * f * 4.5f;
            if (startSq <= dist3Sq) out.forward = true;
        }
    }
    return out;
}

/// The pending actions 0x0072b3a0 leaves for 0x0071b1e0 to take once the
/// mover has stopped.
struct Pending {
    uint64_t attack = 0;   // 0x00ca11c8 (10, 11)
    uint64_t loot = 0;     // 0x00ca11c0 (6)
    uint64_t object = 0;   // 0x00ca11b8 (7)
    uint64_t skin = 0;     // 0x00ca11b0 (9)
    uint64_t npc = 0;      // 0x00ca11a8 (5)
};

/// 0x0072b3a0: what arriving leaves behind. False for the kinds whose
/// arrival only stops the mover (Follow and the rest), true for those that
/// end the click-to-move (facing the subject, stopping).
inline bool arrive(Kind k, uint64_t guid, Pending& p) {
    switch (k) {
    case Kind::Move: return true;
    case Kind::NpcInteract: p.npc = guid; return true;
    case Kind::Loot: p.loot = guid; return true;
    case Kind::ObjectInteract: p.object = guid; return true;
    case Kind::Skin: p.skin = guid; return true;
    case Kind::AttackPosition:
    case Kind::AttackGuid: p.attack = guid; return true;
    default: return false;
    }
}

/// 0x0071b1e0 takes one pending action a stop, in this order.
enum class Action { None, Attack, Loot, Object, Skin, Npc };
struct Taken {
    Action action = Action::None;
    uint64_t guid = 0;
};
inline Taken take(Pending& p) {
    const auto one = [](uint64_t& g, Action a) {
        Taken t{a, g};
        g = 0;
        return t;
    };
    if (p.attack) return one(p.attack, Action::Attack);
    if (p.loot) return one(p.loot, Action::Loot);
    if (p.object) return one(p.object, Action::Object);
    if (p.skin) return one(p.skin, Action::Skin);
    if (p.npc) return one(p.npc, Action::Npc);
    return {};
}

/// 0x007275c0: the marker on a Move's or AttackPosition's place, a ring
/// whose radius swings between 0.5 and 0.75 yards (0x00adac0c, 0x00adac10)
/// once every 500 ms (0x00adac14).
inline float markerRadius(uint32_t nowMs) {
    const float t = static_cast<float>(static_cast<int>(nowMs % 500u)) * 6.2831855f / 500.0f;
    return (std::sin(t) + 1.0f) * 0.5f * (0.75f - 0.5f) + 0.5f;
}

/// 0x0072f040: the facing of a direction (a click on nothing).
inline float facingOfDirection(const glm::vec3& dir) {
    return wrapTwoPi(facingTo(glm::vec3(0.0f), dir));
}

}  // namespace click_to_move
}  // namespace game
}  // namespace wowee
