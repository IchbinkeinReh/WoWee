#pragma once

/// The action slots that hold macros of the numbers this client once gave the
/// character's (macro_list::slotId), put right once the macros are known.

#include "game/macro_list.hpp"

#include <cstddef>
#include <cstdint>

namespace wowee::game {

template <typename Handler>
void settleMacroSlots(Handler& handler) {
    const auto& bar = handler.getActionBar();
    for (size_t i = 0; i < bar.size(); ++i) {
        if (bar[i].type != bar[i].MACRO) continue;
        const uint32_t id = macro_list::slotId(bar[i].id, [&](uint32_t m) { return handler.macroExists(m); });
        if (id != bar[i].id) handler.setActionBarSlot(static_cast<int>(i), bar[i].MACRO, id);
    }
}

}  // namespace wowee::game
