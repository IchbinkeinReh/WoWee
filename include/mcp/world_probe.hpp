#pragma once

// The ground and what stands in the way, out of the faces the character itself
// is moved against (CameraController::gatherWorldFacets): what is reported is
// what the client would walk on and run into, doodads, buildings and ground
// alike. On faces alone, so that it can be tried without a world.

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "rendering/collision_facets.hpp"

namespace wowee::mcp::probe {

using Facet = rendering::collision::Facet;

struct Hit {
    float distance = 0.0f;
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    Facet::Kind kind = Facet::Kind::None;
    uint32_t owner = 0;  ///< Facet::owner
};

/// The first face a ray runs into from its front, as a motion does.
/// `direction` is of length one.
std::optional<Hit> castRay(const std::vector<Facet>& facets, const glm::vec3& origin, const glm::vec3& direction,
                           float reach);

/// The same for a line of any length, its faces asked for a stretch at a time:
/// `gather` has those that reach into a box, and nothing where the world is not
/// loaded - which ends the line; `known` is how far it was looked at.
std::optional<Hit> castLine(const std::function<std::optional<std::vector<Facet>>(const glm::vec3& lo,
                                                                                  const glm::vec3& hi)>& gather,
                            const glm::vec3& from, const glm::vec3& to, float* known = nullptr);

struct Ground {
    float z = 0.0f;
    float slopeDegrees = 0.0f;
    Facet::Kind kind = Facet::Kind::None;
    uint32_t owner = 0;
};

/// The faces looking up over a point of the plane: the one to stand on for
/// who is at `feet` - the highest that can be stepped or climbed to, up to
/// `climb` over the feet, or failing that the lowest above.
std::optional<Ground> groundAt(const std::vector<Facet>& facets, float x, float y, float feet, float climb);

/// The same for a square of points, each face looked at once.
struct Grid {
    int side = 0;            ///< points along each edge, odd
    float step = 1.0f;
    glm::vec2 corner{0.0f};  ///< the point of row 0, column 0
    std::vector<std::optional<Ground>> points;

    [[nodiscard]] const std::optional<Ground>& at(int row, int column) const {
        return points[static_cast<size_t>(row) * static_cast<size_t>(side) + static_cast<size_t>(column)];
    }
    /// Rows run from north (+x) to south, columns from west (+y) to east, as
    /// a map lies.
    [[nodiscard]] glm::vec2 place(int row, int column) const {
        return corner - glm::vec2(static_cast<float>(row) * step, static_cast<float>(column) * step);
    }
};
Grid groundGrid(const std::vector<Facet>& facets, const glm::vec3& middle, float radius, float step, float climb);

/// How far one can go from `feet` along a level direction before something
/// stands in the way, the ground ends in a drop deeper than `drop`, or rises
/// past walking. Looked at knee and chest high, following the ground.
struct Way {
    float clear = 0.0f;
    enum class Why { Reach, Wall, Drop, Steep } why = Why::Reach;
    Facet::Kind kind = Facet::Kind::None;
    uint32_t owner = 0;
    glm::vec3 at{0.0f};  ///< where the wall is
};
Way wayAlong(const std::vector<Facet>& facets, const glm::vec3& feet, const glm::vec2& direction, float reach,
             float drop);

const char* kindName(Facet::Kind kind);

}  // namespace wowee::mcp::probe
