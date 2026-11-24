#include "rendering/liquid_query.hpp"

#include <vector>

#include "pipeline/wmo_doodad_light.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/wmo_renderer.hpp"

namespace wowee::rendering::liquid_query {

namespace {

using liquid_place::Liquid;
using GroupRef = minimap_indoor::GroupRef;

std::optional<float> groundAt(const World& world, const glm::vec3& at) {
    if (!world.terrain || world.terrain->isHoleAt(at.x, at.y)) return std::nullopt;
    return world.terrain->getHeightAt(at.x, at.y);
}

/// `at`: where the place's liquid is measured, `point` itself or `above` under
/// it.
std::optional<Liquid> inPlace(const World& world, const std::optional<GroupRef>& in, const glm::vec3& point,
                              const glm::vec3& at, const std::optional<float>& groundLimit) {
    return liquid_place::liquidOf(
        in,
        [&](const GroupRef& ref) -> std::optional<Liquid> {
            if (world.buildings) {
                if (const uint32_t type = world.buildings->filledLiquidType(ref); type != 0) {
                    return Liquid{liquid_place::kFilledLevel, type, true};
                }
            }
            return world.water->buildingLiquidAt(ref.instanceId, ref.group, at);
        },
        [&] {
            std::vector<GroupRef> groups;
            if (world.buildings) world.buildings->exteriorGroupsHolding(point, groups);
            return groups;
        },
        [&] { return world.water->groundLiquidAt(at, groundLimit); });
}

bool mayBe(const World& world, const glm::vec3& at) {
    return world.water &&
           liquid_place::mayBeInLiquid(world.water->getWaterHeightAt(at.x, at.y), at.z,
                                       world.buildings && world.buildings->filledGroupsLoaded());
}

}  // namespace

std::optional<Liquid> atCamera(const World& world, const glm::vec3& eye, float above) {
    const glm::vec3 at(eye.x, eye.y, eye.z - above);
    if (!mayBe(world, at)) return std::nullopt;
    const auto ground = groundAt(world, eye);
    std::optional<GroupRef> in;
    if (world.buildings) in = world.buildings->cameraGroupAt(eye, ground);
    // The ground's liquid only over the ground (0x007a0b00's last argument, 1).
    auto limit = ground;
    if (limit) *limit -= above;
    return inPlace(world, in, eye, at, limit);
}

std::optional<Liquid> atUnit(const World& world, const glm::vec3& feet, float above) {
    const glm::vec3 at(feet.x, feet.y, feet.z - above);
    if (!mayBe(world, at)) return std::nullopt;
    namespace wl = pipeline::wmo_doodad_light;
    std::optional<GroupRef> in;
    if (world.buildings) {
        // In a building's group when the world object's floor search finds
        // one under the feet and the ground is not nearer (0x007c2f80 from a
        // tenth above them, 0x007c28f0).
        const float startZ = feet.z + wl::kUnitFloorAbove;
        const auto links =
            world.buildings->linksAlong(glm::vec3(feet.x, feet.y, startZ), startZ - wl::kFloorReach);
        if (links.collides && !wl::terrainNearer(startZ, groundAt(world, feet), startZ - links.collides->dist)) {
            in = links.collides->ref;
        }
    }
    // The ground's wherever the ground is (0x007c23f0 asks 0x007a0820 without
    // it): one put on the bed, a hand under what the height is read as there,
    // was in no water and walked it.
    return inPlace(world, in, feet, at, std::nullopt);
}

std::optional<float> surfaceOverUnit(const World& world, const glm::vec3& feet, float above) {
    const auto l = atUnit(world, feet, above);
    if (!l || l->filled()) return std::nullopt;
    return l->level;
}

}  // namespace wowee::rendering::liquid_query
