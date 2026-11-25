#pragma once

#include <cstdint>

namespace wowee::game {

/// The enchant a spell's effect puts on an item, and the one it would take the
/// place of (0x005210d0): in the permanent slot for ENCHANT_ITEM (53), in the
/// temporary one for ENCHANT_ITEM_TEMPORARY (54). `asks` where there is one.
struct ReplacedEnchant {
    uint32_t existing = 0;
    uint32_t incoming = 0;
    [[nodiscard]] constexpr bool asks() const { return existing != 0 && incoming != 0; }
};

[[nodiscard]] constexpr ReplacedEnchant replacedEnchant(uint32_t effect, uint32_t miscValue, uint32_t permanent,
                                                        uint32_t temporary) {
    if (effect == 53) return {permanent, miscValue};
    if (effect == 54) return {temporary, miscValue};
    return {};
}

}  // namespace wowee::game
