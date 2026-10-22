#pragma once

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <glm/glm.hpp>

namespace wowee::rendering::movement {

// The retail client rejects ground steeper than 50 degrees (0x0075d1c0 tests
// the face normal against 0.64278764) and steps up one yard. Keep these limits
// shared by terrain, WMO, and M2 collision paths so release builds cannot
// silently diverge by surface type.
//
// The step: when the move sweep (0x007620f0) runs into a face too steep to
// walk, 0x00761b00 sweeps the character up by the step height (0x006e9520),
// forward, and back down, and climbs only if it lands on walkable ground. The
// step height is the moving object's +0xd0, set by 0x006e9570 to the object's
// scale (at least 1) over its display scale - 1.0 for a player at normal size
// (a creature not moved by the player gets 2.0). It was taken as 0.6 here,
// from nowhere in the client, so a bed 0.93 high was neither a step nor - as a
// building's doodad - a wall, and the character walked into it.
inline constexpr float kMaxWalkableSlopeDegrees = 50.0f;
inline constexpr float kMinWalkableNormalZ = 0.642787635f; // cos(50 degrees)
inline constexpr float kMaxStepUp = 1.0f;

inline bool isWalkableNormal(float normalZ) {
    return normalZ >= kMinWalkableNormalZ;
}

/// The upward component of the heightfield's normal at a point, from four
/// samples around it.
///
/// The terrain query answers a height and nothing else, so the slope limit that
/// governs WMO and M2 floors was never applied to the ground itself - the
/// constant for it existed and was only ever used as the fallback limit for
/// those other two. A mountain of any steepness was therefore walkable, held
/// back by nothing but the per-step height budget, which a smooth heightfield
/// never trips. This recovers the missing normal by finite difference.
///
/// sample() returns the height at a point, or nothing where there is no ground
/// - at a hole, or off the loaded tiles. A missing neighbour makes the slope
/// unknowable rather than steep, so the answer is 1 and the caller lets it
/// pass: refusing on absent data would stop the player at every tile edge.
template <typename SampleFn>
glm::vec3 heightfieldNormal(SampleFn&& sample, float x, float y, float spacing) {
    const auto west  = sample(x - spacing, y);
    const auto east  = sample(x + spacing, y);
    const auto south = sample(x, y - spacing);
    const auto north = sample(x, y + spacing);
    if (!west || !east || !south || !north) return {0.0f, 0.0f, 1.0f};

    const float dzdx = (*east - *west) / (2.0f * spacing);
    const float dzdy = (*north - *south) / (2.0f * spacing);
    return glm::vec3(-dzdx, -dzdy, 1.0f) / std::sqrt(dzdx * dzdx + dzdy * dzdy + 1.0f);
}

template <typename SampleFn>
float heightfieldNormalZ(SampleFn&& sample, float x, float y, float spacing) {
    return heightfieldNormal(sample, x, y, spacing).z;
}

/// Where a step taken in flight ends, when it would end inside the ground.
///
/// Flight has no use for the step-up budget that keeps a walker on the ground:
/// a flying mount at speed rises more than 0.6 yards a frame up any steep
/// slope, so the floor pick refused the hillside ahead as out of reach, and
/// nothing else holds the heightfield up in flight. The mount flew into the
/// hill and on through it.
///
/// So the step goes as far as the surface, and what is left of it slides along
/// the surface with the part driving into the slope taken out: a gentle rise
/// is skimmed up, a steep one climbed slowly, and a sheer face stops the step.
/// Flying straight down ends on the ground, which is how a landing starts.
///
/// Only a step that crosses the surface is touched. One that starts under it is
/// in a cave or a tunnel, or somewhere else this cannot vouch for, and ground
/// that answers nothing - a hole, a tile not loaded - is no ground.
template <typename SampleFn>
glm::vec3 flightStepAgainstGround(SampleFn&& sample, const glm::vec3& from,
                                  const glm::vec3& to, float normalSpacing) {
    const auto endGround = sample(to.x, to.y);
    if (!endGround || to.z >= *endGround) return to;
    // Feet on the ground read a hair either side of it.
    constexpr float kContactSlack = 0.25f;
    const auto startGround = sample(from.x, from.y);
    if (!startGround || from.z < *startGround - kContactSlack) return to;

    // Where along the step the feet meet the surface: from is above it (or
    // touching), to is below.
    const glm::vec3 step = to - from;
    float above = 0.0f;
    float below = 1.0f;
    for (int i = 0; i < 10; ++i) {
        const float mid = 0.5f * (above + below);
        const glm::vec3 p = from + step * mid;
        const auto ground = sample(p.x, p.y);
        if (!ground || p.z >= *ground) above = mid;
        else below = mid;
    }
    const glm::vec3 contact = from + step * above;

    glm::vec3 rest = step * (1.0f - above);
    const glm::vec3 n = heightfieldNormal(sample, contact.x, contact.y, normalSpacing);
    const float into = glm::dot(rest, n);
    if (into < 0.0f) rest -= into * n;

    glm::vec3 end = contact + rest;
    if (const auto ground = sample(end.x, end.y); ground && end.z < *ground) end.z = *ground;
    return end;
}

inline bool isReachableStep(float deltaZ) {
    return deltaZ >= -0.25f && deltaZ <= kMaxStepUp;
}

/// Whether the outdoor heightfield at this spot is a roof rather than a floor.
///
/// Inside an interior WMO group - Undercity's halls, a building's rooms - the
/// heightfield overhead is meaningless, and letting it stand as a floor
/// candidate kicks the player up to the surface whenever the WMO floor query
/// finds nothing underfoot.
///
/// But being "inside" is decided by bounding-box containment, and an
/// underground WMO's interior box reaches up through the ground above it. So
/// standing on the hillside over a cave counts as inside, and discarding the
/// terrain there leaves the WMO as the only candidate - whose nearest surface
/// below is the cave's ceiling. The player drops through the hill they are
/// walking on and stands on the roof of the room underneath.
///
/// Hence the height test as well as the containment one: ground higher than the
/// player could step onto cannot be the ground they are standing on, which is
/// the whole of what the veto ever meant. Undercity's surface sits ~113m above
/// its halls and is still refused; a hillside at the feet is kept.
/// Whether this ground is too far above the feet to be the ground underfoot.
///
/// The one question behind both rules below: terrain the player could not step
/// onto is not terrain they are standing on.
inline bool terrainOutOfReach(float terrainZ, float feetZ, float stepUpBudget) {
    return terrainZ > feetZ + stepUpBudget + 0.5f;
}

inline bool terrainIsOverheadRoof(bool insideInteriorWmo, float terrainZ,
                                  float feetZ, float stepUpBudget) {
    return insideInteriorWmo && terrainOutOfReach(terrainZ, feetZ, stepUpBudget);
}

/// The height a floor pick should measure "nearest" from.
///
/// Nearest to the feet is right while the player is standing: it is what lets a
/// step onto a ledge win once the feet are level with it, and what stops a
/// stray frame on the storey above latching the pick up there.
///
/// It is wrong the moment the feet are under the floor they are on. At the
/// Undercity elevator ramp the feet sat 2.81 below the landing - still a
/// candidate - and 2.39 above the hall floor five yards further down, so
/// measured from the feet the lower one was nearer by 0.42 and the player
/// dropped through the ramp. Below the last ground by more than a step is
/// falling, whatever the grounded flag says, so anchor where the airborne case
/// already anchors and the floor underfoot wins by its true margin.
///
/// This cannot haul anyone up onto a floor they really walked off: once they
/// are past its edge it is not a candidate over their feet at all.
inline bool standingOnLastGround(bool grounded, float feetZ, float lastGroundZ) {
    return grounded && feetZ > lastGroundZ - kMaxStepUp;
}

inline float floorArbitrationAnchor(bool grounded, float feetZ, float lastGroundZ) {
    return standingOnLastGround(grounded, feetZ, lastGroundZ) ? feetZ : lastGroundZ;
}

/// How far above the feet a floor found off-centre, under the body's edge,
/// may be and still be stood on.
///
/// On the ground, a step's height: walking against a low box or a pallet
/// steps up onto it. In the air, hardly at all: an edge that the feet are
/// already below is the side of the thing, not its top, and the body lands
/// on it only by coming down onto it from above. With the step's height in
/// the air too, a jump beside a crate in the Goldshire inn's cellar came
/// down on the crate's lid a yard up (50.795 over a floor at 49.79): its
/// edge was in reach of the foot ring the moment the feet dropped past it.
/// Rising, not even that: the feet have not come down onto anything yet.
inline constexpr float kAirborneEdgeTolerance = 0.1f;
inline float edgeFloorCeiling(bool grounded, float feetZ, float stepUp, float verticalVelocity = 0.0f) {
    if (grounded) return feetZ + stepUp;
    if (verticalVelocity > 0.0f) return feetZ;
    return feetZ + std::min(stepUp, kAirborneEdgeTolerance);
}

/// At a tunnel seam, whether the WMO floor should be taken over the terrain.
///
/// Take it only when the terrain is not ground the player could be standing
/// on. That is what a tunnel mouth looks like from inside it: the heightfield
/// overhead is the hillside over the tunnel, and letting it win would lift the
/// player back out rather than let them walk in.
///
/// The seam used to prefer the WMO floor whatever it was, and that also fires
/// where a ramp merely passes under the heightfield beside it. The Orgrimmar
/// valley entrance runs its ramp about 1.3 yards below the ground it meets, so
/// a player standing on that ground - terrain right at their feet - was pulled
/// down onto the ramp and ended up inside the hillside, walking through the
/// terrain with the ramp overhead.
///
/// Measured against the feet rather than against the gap between the two
/// surfaces: 1.3 yards is more than a step, so a gap test calls the Orgrimmar
/// ramp a tunnel. Where the player is standing answers it and the gap does not.
inline bool wmoFloorIsWayIn(float terrainZ, float feetZ, float stepUpBudget) {
    return terrainOutOfReach(terrainZ, feetZ, stepUpBudget);
}

/// Where a teleport puts the feet: the floor at or just under the position
/// the server gave, never the ground the player stood on before.
///
/// The client takes a teleport's position as it is. MSG_MOVE_TELEPORT_ACK
/// (0x00732450 -> 0x0072d2d0) queues a move event 0x2c (0x006eca00) whose
/// handler (0x006ecf80) calls 0x0098ba90: the position, the facing, the fall's
/// start (+0x4c..+0x54) all become the new one and the fall time is zeroed. No
/// height of the old ground survives it; the next collision pass looks for the
/// floor from where the player now is.
///
/// Here the old ground did survive (lastGroundZ), and the floor probe reaches
/// up from it: teleported from the Goldshire inn's hall (55.99) into its
/// cellar (50.16), the probe started over the hall floor, found it, and put
/// the player back upstairs. A storey overhead is out of reach of this window,
/// and so is a heightfield lying over a cellar; only a floor the feet could
/// step onto from the server's height is taken.
inline constexpr float kTeleportFloorReachUp = kMaxStepUp + 0.5f;
/// How far under the server's height a floor is still taken as the landing.
/// Further down, the server's height is kept and the player falls from it.
///
/// The server's height is its own floor there (its vmaps), so a floor far
/// under it is one the server does not stand the player on: 16 yards took
/// Darnassus' terrain (1328.87) under the temple terrace (1341.39) while
/// the terrace was not loaded yet, and the player stood 12.5 yards down.
inline constexpr float kTeleportFloorReachDown = 2.0f;

inline std::optional<float> teleportLandingFloor(std::initializer_list<std::optional<float>> floors,
                                                 float serverZ) {
    std::optional<float> best;
    for (const std::optional<float>& h : floors) {
        if (!h || *h > serverZ + kTeleportFloorReachUp || *h < serverZ - kTeleportFloorReachDown) continue;
        if (!best || *h > *best) best = h;
    }
    return best;
}

} // namespace wowee::rendering::movement
