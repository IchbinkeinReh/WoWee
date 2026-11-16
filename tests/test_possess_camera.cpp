// Possession and the camera's pivot, as the client has them: a possessed
// unit moves without its master (0x00729010, 0x006e2880 -> 0x004f6f50), and
// the camera pivots at the followed model's Breath attachment under its
// collision height (0x00604e00, 0x00718080).
#include <catch_amalgamated.hpp>

#include "game/vehicle_mover.hpp"
#include "rendering/camera_pivot.hpp"

using Catch::Matchers::WithinAbs;
namespace cp = wowee::rendering::camera_pivot;
using wowee::game::vehicle::moverIsPossessed;

namespace {
constexpr uint64_t kPlayer = 0x0000000000000011ull;
constexpr uint64_t kEye = 0xF1300010B5050C9Dull;  // Eye of Kilrogg, as the server named it
constexpr uint64_t kVehicle = 0xF150001234000056ull;
}  // namespace

TEST_CASE("a unit with no vehicle and the player in no seat is possessed", "[possess]") {
    // Eye of Kilrogg: the player stays where it stood.
    CHECK(moverIsPossessed(kEye, kPlayer, 0, false));
    // A vehicle the player drives carries it along.
    CHECK_FALSE(moverIsPossessed(kVehicle, kPlayer, 312, true));
    // A vehicle row alone keeps the vehicle's way, seat or none.
    CHECK_FALSE(moverIsPossessed(kVehicle, kPlayer, 312, false));
    // A seat alone too: the player rides whatever moves.
    CHECK_FALSE(moverIsPossessed(kEye, kPlayer, 0, true));
    // The player itself, or no mover at all, is no possession.
    CHECK_FALSE(moverIsPossessed(kPlayer, kPlayer, 0, false));
    CHECK_FALSE(moverIsPossessed(0, kPlayer, 0, false));
}

TEST_CASE("the pivot is the Breath attachment plus 3.5/36 at the model's scale", "[camera]") {
    // Blood elf male: Breath at 1.8726, scale 1.
    CHECK_THAT(cp::modelPivot(1.8726436f, 0.0f, 2.0312779f, 1.0f), WithinAbs(1.969866f, 1e-4));
    // Metamorphosis' DemonForm.m2 at its display's 0.5: Breath at 4.3451.
    CHECK_THAT(cp::modelPivot(4.3450861f, 0.0f, 2.0312779f, 0.5f), WithinAbs(2.221154f, 1e-4));
    // No Breath: nine tenths of the collision box.
    CHECK_THAT(cp::modelPivot(std::nullopt, 0.0f, 2.0f, 1.5f), WithinAbs(2.7f, 1e-5));
}

TEST_CASE("the ceiling is the collision height less a sixth", "[camera]") {
    // A player by its own height, however tall.
    CHECK_THAT(cp::pivotCeiling(2.0312779f, true), WithinAbs(1.864611f, 1e-5));
    CHECK_THAT(cp::pivotCeiling(4.0f, true), WithinAbs(3.833333f, 1e-5));
    // Any other unit at most 73/36.
    CHECK_THAT(cp::pivotCeiling(4.0f, false), WithinAbs(1.861111f, 1e-5));
    CHECK_THAT(cp::pivotCeiling(1.0f, false), WithinAbs(0.833333f, 1e-5));
}

TEST_CASE("the pivot is held between 0.8333 and the ceiling", "[camera]") {
    // A blood elf's Breath is above its ceiling: the ceiling.
    CHECK_THAT(cp::clampPivot(1.969866f, cp::pivotCeiling(2.0312779f, true)), WithinAbs(1.864611f, 1e-5));
    // The demon at its display's 0.5 (collision height 2.031 x 0.5): held
    // to its own ceiling, not to the elf's head it had before.
    const float demonCeiling = cp::pivotCeiling(2.0312779f * 0.5f, true);
    CHECK_THAT(cp::clampPivot(2.221154f, demonCeiling), WithinAbs(0.848972f, 1e-5));
    // Below the floor the floor, even where the ceiling is lower.
    CHECK_THAT(cp::clampPivot(0.5f, 0.6f), WithinAbs(cp::kMinPivot, 1e-6));
    // A gnome: Breath 0.7796 + 3.5/36 under 1.0556 - 1/6.
    CHECK_THAT(cp::clampPivot(cp::modelPivot(0.7796491f, 0.0f, 1.0555556f, 1.0f), cp::pivotCeiling(1.0555556f, true)),
               WithinAbs(0.876871f, 1e-5));
    // No ceiling: 15.
    CHECK_THAT(cp::clampPivot(20.0f, 0.0f), WithinAbs(15.0f, 1e-6));
}
