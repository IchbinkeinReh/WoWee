#pragma once

/// The client's ground movement (0x007620f0) and the step-up it tries at a
/// face too steep to walk (0x00761b00), ported on the sweep
/// (collision_sweep.hpp).
///
/// A walk moves the body along the ground by a distance over a span of
/// milliseconds. Each sweep stops at the first facet in the way, and what
/// is left of the move goes on from there:
///
///  - a walkable facet (0x0075d1c0: a player's ground less steep than 50
///    degrees) lifts the rest of the move onto it (0x0075e0c0), as far up
///    as the step height allows;
///  - a facet too steep is tried as a step (0x00761b00): the body is swept
///    up by what is left of the step height, across by a third of a yard or
///    more, and down again; if it lands on walkable ground, or on nothing,
///    or would fall clear past the edge (0x007619c0), the unit is marked as
///    stepping (+0x44 0x4000000, from the height it stood at, +0x88) and
///    the rest of the move climbs the face like walkable ground until the
///    step height is used up. Otherwise the move slides along the face
///    (0x0075e250);
///  - with the time used up and nothing hit, the body is swept down by
///    1.849399 times the move (the pyramid's slope) to keep it on the
///    ground going down a slope or a stair; finding nothing, or only a face
///    too steep it is not stepping on, it falls (0x00988370).
///
/// The client gives up after more than five sweeps that each take less than
/// a millisecond of the move.

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "rendering/collision_facets.hpp"
#include "rendering/collision_sweep.hpp"

namespace wowee::rendering::collision {

/// 0x0075d1c0's limits: a player's ground, and anyone else's.
inline constexpr float kWalkableNormalZ = 0.64278764f;
inline constexpr float kWalkableNormalZOther = 0.17364818f;
/// 0x00761b00: how far across a step is tried, in step heights (tan 50).
inline constexpr float kStepAcross = 1.1917536f;
/// 0x007620f0: how far down the body is kept on the ground, in move lengths.
inline constexpr float kStepDownPerMove = kPyramidRise;
/// 0x0075e250: what a slide pushes off a face by.
inline constexpr float kSlidePushOff = 0.001f;

/// The parts of a moving unit (CMovement) the ground movement reads and
/// writes, by their offsets in the client.
struct Mover {
    glm::vec3 pos{0.0f};              ///< +0x10, the feet
    float halfWidth = kPlayerHalfWidth;   ///< +0xc8
    float height = kPlayerHeight;     ///< +0xcc
    float stepHeight = 1.0f;          ///< +0xd0 (0x006e9570: 1 for a player at normal size)
    bool player = true;               ///< 0x00716710: moved by this client
    bool stepping = false;            ///< +0x44 0x4000000
    float stepBaseZ = 0.0f;           ///< +0x88, where stepping began
    bool falling = false;             ///< +0x44 0x1000
    float fallStartZ = 0.0f;          ///< +0x84
};

/// The facets round a box (the gather 0x0075f0a0 makes for a sweep).
using FacetGather = std::function<void(const glm::vec3& boxMin, const glm::vec3& boxMax,
                                       std::vector<Facet>& out)>;

/// A sweep against what is gathered round its path, keeping the facets so
/// that the facet it reports can be read after.
class Sweeper {
public:
    explicit Sweeper(FacetGather gather) : gather_(std::move(gather)) {}
    SweepResult run(const Mover& m, const glm::vec3& dir, float dist);
    [[nodiscard]] const Facet& facet(int index) const { return facets_[static_cast<size_t>(index)]; }
    /// The body planes of the last sweep, for its contacts.
    [[nodiscard]] const Body& body() const { return body_; }

private:
    FacetGather gather_;
    std::vector<Facet> facets_;
    Body body_;
};

/// 0x006e8fc0: the step height (2 for a unit this client does not move).
float stepHeightOf(const Mover& m);
/// 0x006e9520: what is left of it, less what stepping has climbed already.
float stepBudget(const Mover& m);
/// 0x0075b690: a face too steep to walk.
inline bool tooSteep(const glm::vec3& n) { return n.z <= kWalkableNormalZ; }

/// 0x0075de80: what the body's touching faces together push along, when
/// all of them are the pyramid's (none of the column's): one face's normal,
/// two faces' sum scaled to unit length, three the one of them lying along
/// an axis on its own, all four nothing. False when a column face touches.
bool contactPush(const Body& body, const std::vector<int>& contacts, glm::vec3& out);

/// 0x0075e0c0: how a walkable face lifts the rest of a move: the climb, and
/// `remaining` cut back where the step height does not reach.
glm::vec3 liftOnto(const Mover& m, const glm::vec3& dir, float& remaining, glm::vec3 n,
                   bool usePush, const glm::vec3& push);

/// 0x0075e250: the sideways move along a face too steep to climb: the rest
/// of the move turned along it, no longer across the ground than
/// `horizontalLeft`. Nothing for a face overhead.
glm::vec3 slideAlong(const glm::vec3& dir, float remaining, float horizontalLeft, const glm::vec3& n);

struct WalkResult {
    bool fell = false;       ///< 0x00988370 was called: the unit now falls
    bool stopped = false;    ///< the move was cut short (0x009881d0)
    int groundFacet = -1;    ///< the facet it ended on, in the last sweep's set
    glm::vec3 groundNormal{0.0f, 0.0f, 1.0f};
};

/// 0x007620f0: walk `dist` yards along the ground direction `dir` (x, y)
/// over `ms` milliseconds.
WalkResult walk(Mover& m, const glm::vec2& dir, float dist, uint32_t ms, Sweeper& sweeper);

/// 0x00761b00: try the face with normal `n`, met walking along `dir`, as a
/// step. False only when a sweep could not be made at all.
bool tryStepUp(Mover& m, const glm::vec2& dir, const glm::vec3& n, const Facet& met, Sweeper& sweeper);

/// 0x00988370: the unit starts to fall from where it is.
void startFall(Mover& m);

}  // namespace wowee::rendering::collision
