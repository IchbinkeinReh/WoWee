#pragma once

#include <string>
#include <vector>

namespace wowee::game {

/// What the interface hears of a spell gone from the book, in order
/// (0x0053fad0): the stance bar's, where the spell stood in its list; for one
/// that stood in a list of companions, that the lists are in order again
/// (COMPANION_UPDATE without arguments, 0x0053cec0: where any is left in them)
/// and COMPANION_UNLEARNED; then the book's (0x0053ca70).
[[nodiscard]] inline std::vector<std::string> removedSpellEvents(bool stance, bool companion, bool companionsLeft) {
    std::vector<std::string> events;
    if (stance) events.emplace_back("UPDATE_SHAPESHIFT_FORMS");
    if (companion) {
        if (companionsLeft) events.emplace_back("COMPANION_UPDATE");
        events.emplace_back("COMPANION_UNLEARNED");
    }
    events.emplace_back("SPELLS_CHANGED");
    return events;
}

/// And of a companion learned, as its creature is known (0x00542030, 0x0053ebc0).
[[nodiscard]] inline std::vector<std::string> learnedCompanionEvents() {
    return {"COMPANION_UPDATE", "COMPANION_LEARNED"};
}

}  // namespace wowee::game
