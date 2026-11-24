#pragma once

/// The liquid a point of the loaded world is in (liquid_place.hpp has the
/// client's rule).

#include <optional>

#include <glm/glm.hpp>

#include "rendering/liquid_place.hpp"

namespace wowee::rendering {

class TerrainManager;
class WaterRenderer;
class WMORenderer;

namespace liquid_query {

/// What is asked, any of it null.
struct World {
    const WaterRenderer* water = nullptr;
    const WMORenderer* buildings = nullptr;
    const TerrainManager* terrain = nullptr;
};

/// The liquid the camera is in (0x00790920). With `above`, the one it is in or
/// is no more than that over the surface of.
[[nodiscard]] std::optional<liquid_place::Liquid> atCamera(const World& world, const glm::vec3& eye,
                                                           float above = 0.0f);

/// The liquid a unit with its feet there is in (0x007a1bc0, 0x007c23f0). With
/// `above`, the one it is in or is no more than that over the surface of.
[[nodiscard]] std::optional<liquid_place::Liquid> atUnit(const World& world, const glm::vec3& feet,
                                                         float above = 0.0f);

/// atUnit where there is a surface to play on: none in a group that is liquid
/// throughout.
[[nodiscard]] std::optional<float> surfaceOverUnit(const World& world, const glm::vec3& feet, float above = 0.0f);

}  // namespace liquid_query
}  // namespace wowee::rendering
