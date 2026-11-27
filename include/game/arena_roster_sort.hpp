#pragma once

/// The arena team roster's order (0x005a2e80, 0x005a2bd0, 0x005a3490): a list
/// of all seven columns, the one clicked last in front, each with its own
/// direction; rows compare by the first column that tells them apart.

#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include "game/client_compare.hpp"

namespace wowee::game::arena_roster_sort {

/// 0x005a3490's names, in its order: name class played won seasonplayed
/// seasonwon rating.
enum Column : int { Name = 0, Class, Played, Won, SeasonPlayed, SeasonWon, Rating };

/// 0x00c0f7fc: seven (column, reversed) pairs; 0x005a40e0 lays them out in
/// column order, none reversed.
struct Order {
    std::array<std::pair<int, bool>, 7> keys{{{Name, false}, {Class, false}, {Played, false}, {Won, false},
                                             {SeasonPlayed, false}, {SeasonWon, false}, {Rating, false}}};
};

/// 0x005a3490: the column a name stands for, compared without case; any other
/// name is the name column.
[[nodiscard]] inline int columnFor(const std::string& name) {
    static constexpr const char* kNames[] = {"name", "class", "played", "won", "seasonplayed", "seasonwon", "rating"};
    for (int i = 0; i < 7; ++i)
        if (equalsCapitals(name, kNames[i])) return i;
    return Name;
}

/// 0x005a2e80: the clicked column to the front with the direction it had; the
/// front one clicked again turns round.
inline void click(Order& order, int column) {
    size_t at = 0;
    while (at < order.keys.size() && order.keys[at].first != column) ++at;
    if (at == order.keys.size()) return;
    bool reversed = order.keys[at].second;
    if (at == 0) {
        reversed = !reversed;
    } else {
        for (size_t i = at; i > 0; --i) order.keys[i] = order.keys[i - 1];
    }
    order.keys[0] = {column, reversed};
}

/// One column's comparison of two rows, before its direction (0x005a2bd0): a
/// name by bytes (0x0076e760, strncmp), a class by its name without case
/// (0x0076ec80; nothing when either has none), a number from the highest.
/// Negative: a first.
[[nodiscard]] inline int compareNumbers(uint32_t a, uint32_t b) {
    if (a == b) return 0;
    return b < a ? -1 : 1;
}

}  // namespace wowee::game::arena_roster_sort
