#pragma once

// Where a rider is on a lift's or the Deeprun Tram's car after a step of its
// own. The client steps a passenger on its transport (0x00762e00, the fall's
// step 0x007618b0), against the car's collision faces as any other's; here the
// car has carried the rider to where it is now, and the step was done in the
// world against the car standing there, which is the same.

#include "rendering/movement_limits.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <optional>

namespace wowee::game::car_ride {

struct Step {
    glm::vec3 carried{0.0f};  ///< Where the car has its rider now.
    glm::vec3 own{0.0f};      ///< What it has moved itself since it was put.
    bool moves = false;       ///< By its keys, or in the air.
    bool rising = false;      ///< A jump going up.
    bool grounded = true;
    bool boarding = false;    ///< The ride's first step.
};

struct Place {
    glm::vec3 at{0.0f};
    bool onFloor = false;  ///< The car's floor holds it there.
};

/// `floorAt` is the height of the car's floor under a place, from a step above
/// it. A rider that does not move is left where it is (0x00762e00 moves a unit
/// that moves); one sunk into the floor stands on it, and so does one that
/// boards within a step above it.
template <typename FloorAt>
[[nodiscard]] Place place(const Step& step, FloorAt&& floorAt) {
    // As near as the world's coordinates and the car's agree.
    constexpr float kOnIt = 0.02f;
    Place result{step.carried, false};
    if (step.moves) result.at += step.own;
    if (step.rising) return result;
    if (const std::optional<float> floor = floorAt(result.at)) {
        const float above = result.at.z - *floor;
        const bool steps = step.boarding && step.grounded && above <= rendering::movement::kMaxStepUp;
        if (above <= kOnIt || steps) {
            // On it already and standing, it is left as it is: its place taken
            // out of the world each frame drifts.
            if (step.moves || std::abs(above) > kOnIt) result.at.z = *floor;
            result.onFloor = true;
        }
    }
    return result;
}

}  // namespace wowee::game::car_ride
