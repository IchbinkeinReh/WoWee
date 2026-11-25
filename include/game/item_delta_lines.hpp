#pragma once

#include "game/item_stat_summary.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace wowee::game::item_stats {

/// The lines below ITEM_DELTA_DESCRIPTION in the comparison tooltip (0x0062d930):
/// what changes with the item in the place of the worn one, the damage per second
/// first, then the stats and the sockets. `text` is a GlobalStrings text.
[[nodiscard]] inline std::vector<std::string> deltaLines(const Summary& delta,
                                                         const std::function<std::string(const std::string&)>& text) {
    std::vector<std::string> lines;
    const auto line = [&](bool more, const std::string& amount, const std::string& name) {
        lines.push_back((more ? "|cff00ff00+" : "|cffff2020-") + amount + "|r " + name);
    };
    if (std::fabs(delta.dps) > 1.1920929e-07f) {
        char amount[32];
        std::snprintf(amount, sizeof(amount), "%.1f", static_cast<double>(std::fabs(delta.dps)));
        line(delta.dps > 0.0f, amount, text("ITEM_MOD_DAMAGE_PER_SECOND_SHORT"));
    }
    // The two attack powers ITEM_MOD folds in, and the sockets, are not among these.
    for (int i = 0; i < kFirstSocket; ++i) {
        const int32_t v = delta.v[static_cast<size_t>(i)];
        if (v != 0 && i != 0x31 && i != 0x32) line(v > 0, std::to_string(v > 0 ? v : -v), text(key(i)));
    }
    constexpr const char* kSockets[4] = {"Meta", "Red", "Yellow", "Blue"};
    for (int i = 0; i < 4; ++i) {
        const int32_t v = delta.v[static_cast<size_t>(kFirstSocket + i)];
        if (v == 0) continue;
        line(v > 0, std::to_string(v > 0 ? v : -v),
             std::string("|TInterface\\ItemSocketingFrame\\UI-EmptySocket-") + kSockets[i] + ".blp:12|t  " +
                 text(key(kFirstSocket + i)));
    }
    return lines;
}

}  // namespace wowee::game::item_stats
