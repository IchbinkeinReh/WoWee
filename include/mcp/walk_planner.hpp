#pragma once

// A way on foot, over the faces the character is moved against.
//
// walk_to presses the keys a player would; which way to press them, round a
// house and over a bridge, is worked out here: A* over a yard's grid of the
// ground as far as it is loaded, a step taken only where nothing stands at the
// knee or the chest, the rise can be walked and the drop survived. What it
// finds is a suggestion. Whether the client lets the character through is
// found out by walking.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "mcp/world_probe.hpp"

namespace wowee::mcp {

struct ToolContext;

struct WalkPlan {
    std::vector<glm::vec3> points;  ///< without the start
    bool reaches = false;
    float left = 0.0f;              ///< yards from its end to the goal
    size_t visited = 0;
};

struct WalkLimits {
    float cell = 1.0f;
    size_t nodes = 6000;
    float climb = 1.1f;
    float drop = 5.0f;
    float bodyRadius = 0.5f;
    float bodyHeight = 2.0f;  ///< a rider's is more than that of one on foot
    float arrive = 1.5f;      ///< the goal is reached within this
    /// Places the character was stuck before: nothing near them is walked on.
    std::vector<glm::vec3> avoid;
    float avoidRadius = 1.0f;
};

/// Faces by where they lie, so that a question looks at a few of them.
class FacetIndex {
public:
    explicit FacetIndex(std::vector<probe::Facet> facets, float cell = 2.0f);

    [[nodiscard]] std::optional<probe::Ground> groundAt(float x, float y, float feet, float climb) const;
    /// Whether a level line is free for a body of this width, from the knee
    /// to the head.
    [[nodiscard]] bool clear(const glm::vec3& feet, const glm::vec3& to, float bodyRadius, float bodyHeight) const;
    [[nodiscard]] bool empty() const { return facets_.empty(); }

private:
    [[nodiscard]] int64_t key(int cx, int cy) const;
    void gather(const glm::vec2& lo, const glm::vec2& hi, std::vector<probe::Facet>& out) const;

    std::vector<probe::Facet> facets_;
    float cell_;
    std::unordered_map<int64_t, std::vector<uint32_t>> cells_;
};

WalkPlan planWalk(const FacetIndex& world, const glm::vec3& from, const glm::vec3& to, const WalkLimits& limits = {});

/// From tools_terrain.cpp, on the game's thread: the faces in reach gathered
/// and the way planned, and the faces in a box round a place.
std::vector<probe::Facet> facetsForWalk(ToolContext& context, const glm::vec3& from, const glm::vec3& to);
WalkPlan planWalk(ToolContext& context, const glm::vec3& from, const glm::vec3& to);
std::vector<probe::Facet> facetsAround(ToolContext& context, const glm::vec3& middle, float radius, float below,
                                       float above);

}  // namespace wowee::mcp
