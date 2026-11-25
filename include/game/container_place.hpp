#pragma once

namespace wowee::game {

/// Where a container's slot is, as the interface counts them: 0 the backpack, 1
/// to 4 the bags worn; its slots from one.
struct ContainerPlace {
    enum class Where { Nowhere, Backpack, Bag, Elsewhere };
    Where where = Where::Nowhere;
    int bag = 0;    ///< of the four, from zero
    int slot = 0;   ///< from zero
};

[[nodiscard]] constexpr ContainerPlace containerPlace(int container, int slot) {
    using Where = ContainerPlace::Where;
    if (slot < 1) return {};
    if (container == 0) return {Where::Backpack, 0, slot - 1};
    if (container >= 1 && container <= 4) return {Where::Bag, container - 1, slot - 1};
    return {Where::Elsewhere, 0, slot - 1};
}

}  // namespace wowee::game
