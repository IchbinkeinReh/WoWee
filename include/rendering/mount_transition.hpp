#pragma once

// A kit's CharProc 16, the mount transition (0x007265c0 case 16,
// 0x006f9670): the spell's mount rises from the ground by its Birth
// animation where the rider will sit, the rider is carried onto it between
// its own animation's $STB and $STE events, and the unit is mounted as the
// effect ends. The AUMountTransitionObject (0x007fbce0, 0x007fbe00) is set up
// by 0x007fa6a0 and stepped each frame by 0x007fb7f0; the unit's matrix
// reads it (0x007193f0 carries the rider, 0x0071fbf0 places the mount).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

namespace wowee::rendering::mount_transition {

/// The events the transition reads, as words (0x00827780).
inline constexpr uint32_t kEventStartBegin = 0x42545324u;  ///< "$STB"
inline constexpr uint32_t kEventStartEnd = 0x45545324u;    ///< "$STE"
/// AnimationData Birth, which the mount plays (0x006f83d0's 0x7f).
inline constexpr uint32_t kAnimBirth = 127;
/// Spell.dbc Effect 6 (apply aura) with aura 78 (mounted): its misc value
/// the mount's creature (0x006f9670).
inline constexpr uint32_t kEffectApplyAura = 6;
inline constexpr uint32_t kAuraMounted = 78;

/// +0x78.
inline constexpr uint32_t kSetUp = 0x1;         ///< 0x007fa6a0 has run
inline constexpr uint32_t kArrived = 0x2;       ///< the rider is on, or the Birth ran out (0x007f9f60)
inline constexpr uint32_t kTouchedGround = 0x4; ///< the mount found the ground; it fades in

/// What 0x007fa6a0 reads of the two models: the rider's current sequence
/// and its $STB and $STE in it, the mount's Birth's $STB and $STE, where the
/// mount's $STB is put by the rider's matrix, the rider's place and facing.
/// An event a sequence lacks reads 0.
struct SetupInput {
    uint32_t riderSequenceMs = 0;  ///< its end less its start (0x008266b0)
    uint32_t riderStbMs = 0;
    uint32_t riderSteMs = 0;
    uint32_t birthStbMs = 0;
    uint32_t birthSteMs = 0;
    glm::vec3 mountStbWorld{0.0f};     ///< the mount's $STB (0 where it has none) by the rider's matrix
    glm::vec3 riderTranslation{0.0f};  ///< the rider's matrix's
    float riderFacing = 0.0f;
};

/// The AUMountTransitionObject's moving part.
struct State {
    uint32_t flags = 0;          ///< +0x78
    float riderProgress = 0.0f;  ///< +0x10: 0..1 between the rider's $STB and $STE
    float invRiderSpan = 0.0f;   ///< +0x14
    float riderStb = 0.0f;       ///< +0x18
    float riderSte = 0.0f;       ///< +0x1c
    glm::vec3 from{0.0f};        ///< +0x20: the mount's $STB in the world
    glm::vec3 travel{0.0f};      ///< +0x2c: from there to the rider
    float birthStb = 0.0f;       ///< +0x38
    float birthSte = 0.0f;       ///< +0x3c
    float invBirthSpan = 0.0f;   ///< +0x40
    float groundOffset = 0.0f;   ///< +0x44
    glm::vec3 position{0.0f};    ///< +0x48: the mount's place
    glm::vec3 normal{0.0f, 0.0f, 1.0f};  ///< +0x54: the ground under it, eased to
    uint32_t startMs = 0;        ///< +0x60
    float invRiderSequence = 0.0f;  ///< +0x64: one over the rider's sequence, the unit every time is in
    float facing = 0.0f;         ///< +0x74
    float fade = 0.0f;           ///< +0x7c
    uint32_t lastMs = 0;         ///< +0x80
};

/// 0x007fbe00: made where the unit stands, its ground normal, now.
inline State begin(uint32_t nowMs, const glm::vec3& unitNormal) {
    State s;
    s.normal = unitNormal;
    s.startMs = nowMs;
    s.lastMs = nowMs;
    return s;
}

/// 0x007fa6a0: the times in the rider's sequence's units; false - to be
/// tried again next frame - where the Birth's $STE is not after its $STB.
/// Every time, the mount's too, is over the rider's sequence's length.
inline bool setup(State& s, const SetupInput& in) {
    if (in.riderSequenceMs == 0) return false;  // no sequence to time it by
    const float inv = 1.0f / static_cast<float>(in.riderSequenceMs);
    s.invRiderSequence = inv;
    s.riderStb = static_cast<float>(in.riderStbMs) * inv;
    s.riderSte = static_cast<float>(in.riderSteMs) * inv;
    s.invRiderSpan = 1.0f / (s.riderSte - s.riderStb);
    s.from = in.mountStbWorld;
    s.travel = in.riderTranslation - in.mountStbWorld;
    s.birthStb = static_cast<float>(in.birthStbMs) * inv;
    s.birthSte = static_cast<float>(in.birthSteMs) * inv;
    const float span = s.birthSte - s.birthStb;
    s.invBirthSpan = span;
    if (!(span > 0.0f)) return false;
    s.invBirthSpan = 1.0f / span;
    s.facing = in.riderFacing;
    s.groundOffset = 0.0f;
    s.flags |= kSetUp;
    return true;
}

/// The ground found across a column (0x0077f310 down it, 0x00783a40 up
/// it): how far down from its top, 0..1, and the surface's normal.
struct GroundHit {
    float fraction = 0.0f;
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
};

/// 0x007fb7f0 after its setup: the rider's progress and facing, the mount
/// carried from its $STB toward the rider through the Birth's $STB..$STE -
/// sought on the ground within ten yards at first, less as it closes - then
/// at the rider; its ground normal eased a quarter each step; and, once it
/// has touched ground, faded in over half a second. `ground` is asked for
/// the column from `top` to `bottom`.
template <class Ground>
void step(State& s, uint32_t nowMs, float riderFacing, const glm::vec3& riderPosition, const glm::vec3& unitNormal,
          Ground&& ground) {
    const float dt = static_cast<float>(nowMs - s.lastMs) * 0.001f;
    s.lastMs = nowMs;
    const float t = static_cast<float>(nowMs - s.startMs) * s.invRiderSequence;
    if (s.riderStb > 0.0f && s.riderStb < t) {
        s.facing = riderFacing;
        s.riderProgress = (t - s.riderStb) * s.invRiderSpan;
        if (s.riderProgress > 1.0f) {
            s.flags |= kArrived;
            s.riderProgress = 1.0f;
        }
    }
    glm::vec3 target = unitNormal;
    if (t <= s.birthSte) {
        if (s.birthStb < t) {
            const float f = std::min((t - s.birthStb) * s.invBirthSpan, 1.0f);
            const float reach = std::sqrt(1.0f - f) * 10.0f;
            s.position = s.from + s.travel * f;
            const glm::vec3 top(s.position.x, s.position.y, s.position.z + reach);
            const glm::vec3 bottom(s.position.x, s.position.y, s.position.z - reach);
            const std::optional<GroundHit> hit = ground(top, bottom);
            if (hit) {
                s.flags |= kTouchedGround;
                s.groundOffset = reach - (hit->fraction * reach + hit->fraction * reach);
            }
            s.groundOffset = std::clamp(s.groundOffset, -reach, reach);
            s.position.z += s.groundOffset;
            if (hit && hit->normal.z > 0.35721236f) target = hit->normal;
        }
    } else {
        s.position = riderPosition;
    }
    s.normal += (target - s.normal) * 0.25f;
    s.normal /= std::sqrt(glm::dot(s.normal, s.normal));
    if (s.flags & kTouchedGround) s.fade = std::min(s.fade + dt + dt, 1.0f);
}

/// The size 0x0071fbf0 draws the mount at: the unit's model scale (its
/// vtable +0x7c, 0x0071c0e0), which multiplies in the mount display's +0x990
/// only while the unit rides - and it rides nothing while this runs. Not
/// positive reads 1.
inline float mountScale(float unitModelScale) {
    return unitModelScale > 0.0f ? unitModelScale : 1.0f;
}

/// The unit's ground normal (+0x9d8, (0,0,1) at first) eased toward the
/// one its movement stands on (0x00987490) wherever that is no steeper than
/// z 0.35721236: what is left of the way is 0.0018 to the power of the
/// frame's seconds (0x007193f0, 0x007197d0). Not normalised.
inline glm::vec3 easeGroundNormal(const glm::vec3& eased, const glm::vec3& ground, float seconds) {
    if (ground.z < 0.35721236f) return eased;
    const float keep = std::pow(0.0018f, std::max(seconds, 0.0f));
    return ground + (eased - ground) * keep;
}

/// 0x0082dd80's turn for a model at `facing` on a ground normal, by the
/// model's M2 global flags & 3: 3 tilts it the whole way, up along the
/// normal (0x00824a80); 1 only along its facing, its sides kept level; any
/// other, not at all. As Euler angles as the character renderer takes them
/// (roll about x, pitch about y, yaw about z; composed z, then y, then x).
inline glm::vec3 tiltedRotation(const glm::vec3& normal, float facing, uint32_t modelFlags) {
    const float len = std::sqrt(glm::dot(normal, normal));
    const glm::vec3 nn = len > 1e-6f ? normal / len : glm::vec3(0.0f, 0.0f, 1.0f);
    const float c = std::cos(-facing);
    const float sn = std::sin(-facing);
    const glm::vec3 n(c * nn.x - sn * nn.y, sn * nn.x + c * nn.y, nn.z);
    switch (modelFlags & 3u) {
        case 3u:
            // Rz Ry(b) Rx(a) (0,0,1) = (cos a sin b, -sin a, cos a cos b).
            return glm::vec3(std::asin(std::clamp(-n.y, -1.0f, 1.0f)), std::atan2(n.x, n.z), facing);
        case 1u:
            // Its side axis level, its forward the side crossed with the normal.
            return glm::vec3(0.0f, std::atan2(n.x, n.z), facing);
        default:
            return glm::vec3(0.0f, 0.0f, facing);
    }
}

/// The end of 0x0082dd80: how far a model's matrix is blended toward the
/// full tilt up its normal (0x00824a80), by the M2Sequence flags (+0xc) of
/// the sequence its body plays (0x008266b0, 0x0082ced0). Flags & 0xe of 2
/// or 4: the time it has played over half its length (0x00827000 times its
/// end), 1 where it plays at rate 0, 0 where backwards - with 4, one less
/// that; 8: the whole way; anything else, not at all. `timeMs` is the time
/// into the sequence, `durationMs` its length.
inline float fullTiltWeight(uint32_t sequenceFlags, float timeMs, uint32_t durationMs, float rate) {
    const uint32_t kind = sequenceFlags & 0xeu;
    if (kind == 8u) return 1.0f;
    if (kind != 2u && kind != 4u) return 0.0f;
    float w = 0.0f;
    if (rate == 0.0f) {
        w = 1.0f;
    } else if (rate > 0.0f) {
        // round(time / rate) over ((end - start) >> 1), the play's real span
        // being its length over its rate.
        const auto half = static_cast<uint32_t>(std::lround(static_cast<float>(durationMs) / rate)) >> 1;
        w = half != 0 ? static_cast<float>(std::lround(timeMs / rate)) / static_cast<float>(half) : 1.0f;
        w = std::clamp(w, 0.0f, 1.0f);
    }
    if (kind == 4u) w = 1.0f - w;
    return w > 0.0f ? w : 0.0f;
}

/// 0x007193f0: the rider is drawn tilted on its ground only while it is
/// carried - at progress 0 it has the plain matrix (0x00743330).
inline bool riderTilts(const State& s) { return s.riderProgress != 0.0f; }

/// 0x0073bbd0: the rider's animation ending once the transition has
/// arrived picks no next one - it holds its last frame.
inline bool riderHoldsLastFrame(const State& s) { return (s.flags & kArrived) != 0; }

/// 0x007fa930: how far the rider is lifted - toward the mount's seat
/// (attachment 0) from the mount's place, by its progress.
inline glm::vec3 riderLift(const State& s, const glm::vec3& seat, const glm::vec3& mountPlace) {
    return (seat - mountPlace) * s.riderProgress;
}

/// 0x0071fbf0's turn for the mount, as Euler angles as the character
/// renderer takes them (roll about x, pitch about y, yaw about z; composed
/// z, then y, then x): facing, and tilted so its up is the eased normal.
inline glm::vec3 mountRotation(const State& s) {
    const float c = std::cos(-s.facing);
    const float sn = std::sin(-s.facing);
    const glm::vec3 n(c * s.normal.x - sn * s.normal.y, sn * s.normal.x + c * s.normal.y, s.normal.z);
    const float roll = std::asin(std::clamp(-n.y, -1.0f, 1.0f));
    const float pitch = std::atan2(n.x, n.z);
    return glm::vec3(roll, pitch, s.facing);
}

}  // namespace wowee::rendering::mount_transition
