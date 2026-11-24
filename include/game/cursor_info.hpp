#pragma once

/// What GetCursorInfo (0x00515200) says of a spell on the cursor: a companion
/// by its place among its kind, anything else by its place in the book it is
/// in, the player's first.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee::game::cursor_info {

/// Where a spell is in a list, from 1 and looked for from its end (0x0053b4e0,
/// 0x0053d990); 0 where it is not.
[[nodiscard]] inline int placeOf(const std::vector<uint32_t>& list, uint32_t spell) {
    for (size_t i = list.size(); spell != 0 && i > 0; --i)
        if (list[i - 1] == spell) return static_cast<int>(i);
    return 0;
}

struct Spell {
    const char* kind = "spell";    ///< "spell" or "companion"
    int place = 0;
    const char* list = "spell";    ///< "spell" or "pet", "CRITTER" or "MOUNT"
};

[[nodiscard]] inline Spell spell(uint32_t id, const std::vector<uint32_t>& critters,
                                 const std::vector<uint32_t>& mounts, const std::vector<uint32_t>& book,
                                 const std::vector<uint32_t>& petBook) {
    if (const int at = placeOf(critters, id)) return {"companion", at, "CRITTER"};
    if (const int at = placeOf(mounts, id)) return {"companion", at, "MOUNT"};
    if (const int at = placeOf(book, id)) return {"spell", at, "spell"};
    if (const int at = placeOf(petBook, id)) return {"spell", at, "pet"};
    return {};
}

}  // namespace wowee::game::cursor_info
