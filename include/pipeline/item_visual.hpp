#pragma once

#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"

#include <cstdint>
#include <span>

namespace wowee::pipeline {

/// The ItemVisuals row an item shows, of its display and its enchants in their order: for an
/// item object all of them (0x00708040), for what a player is seen to hold the permanent and
/// the temporary one (0x00759050), for a link the one and the gems (0x00597fc0). 0 for none.
inline uint32_t itemVisualOf(AssetManager& assets, uint32_t displayId, std::span<const uint32_t> enchants) {
    const auto loaded = [&](const char* name) {
        auto dbc = assets.loadDBC(name);
        return dbc && dbc->isLoaded() ? dbc : nullptr;
    };
    const auto displays = loaded("ItemDisplayInfo.dbc");
    const auto visuals = loaded("ItemVisuals.dbc");
    const auto sie = loaded("SpellItemEnchantment.dbc");
    const auto* layouts = getActiveDBCLayout();
    return itemVisualOf(displays ? displayItemVisual(*displays, displayId) : 0u, enchants, sie.get(), visuals.get(),
                        layouts ? layouts->getLayout("SpellItemEnchantment") : nullptr);
}

}  // namespace wowee::pipeline
