#pragma once

#include <optional>

namespace wowee::game {

/// How many quests a day there are (PLAYER_FIELD_DAILY_QUESTS_1 on).
constexpr int kDailyQuests = 25;

/// What is said as the daily quests done change (0x006d9f20): how many are left,
/// none said where none is done and that was said before - the day's reset, whose
/// fields go one after the other.
struct DailyQuestLine {
    bool noneDoneSaid = false;
    [[nodiscard]] std::optional<int> changed(int done) {
        if (done == 0 && noneDoneSaid) return std::nullopt;
        noneDoneSaid = done == 0;
        return kDailyQuests - done;
    }
};

}  // namespace wowee::game
