#pragma once

/**
 * xp_gain_line.hpp - the chat line SMSG_LOG_XPGAIN prints, as the client
 * chooses it (0x0050c720 -> 0x0050b300 / 0x0050b1d0).
 *
 * The packet gives the experience gained, whether it was a kill, and for a
 * kill the experience before the rested bonus and the group rate, then the
 * Recruit-a-Friend byte. From those the client picks one of the
 * COMBATLOG_XPGAIN_* strings: the rested and tired forms where the rest state's
 * Exhaustion row changes the experience, the group-bonus and raid-penalty forms
 * where the group rate moved it, and the plain ones otherwise. Only the choosing
 * is here; the words are the interface's.
 */

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace wowee {
namespace game {
namespace xp_gain_line {

struct Gain {
    bool victimIsUnit = false;  ///< the victim is a unit this client has (type mask bit 8)
    bool haveVictim = false;    ///< the victim's object exists at all
    std::string victimName;
    int32_t xp = 0;             ///< the experience gained
    uint8_t type = 0;           ///< 0 a kill, otherwise not
    int32_t restlessXp = 0;     ///< a kill's experience before the rested bonus
    float groupRate = 1.0f;     ///< a kill's group rate (1 for anything else)
    bool referAFriend = false;
    int restState = 0;          ///< PLAYER_BYTES_2's rest state
};

/// An Exhaustion.dbc row: whether there is one, its factor and its name.
struct ExhaustionRow { bool found = false; float factor = 1.0f; std::string name; };
using RowOf = std::function<ExhaustionRow(int id)>;

struct Line {
    const char* key = nullptr;
    const char* enUS = nullptr;
    std::vector<std::string> args;
};

/// x87's float-to-int truncation (_ftol2, 0x0088b9c0): toward zero, and the
/// "integer indefinite" for what does not fit.
inline int32_t ftol(float v) {
    if (!std::isfinite(v) || v >= 2147483648.0f || v < -2147483648.0f)
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(v);
}

/// "+N" for N >= 0, "N" for a negative N ("%s%d" with "+" or "").
inline std::string signedText(int32_t n) {
    return (n < 0 ? std::string() : std::string("+")) + std::to_string(n);
}

/// 0x0050b1d0: the plain forms. What the experience would have been alone is
/// the gain over the group rate; above it the group-bonus form, below it the
/// raid-penalty one, with the difference.
inline Line plain(const Gain& g) {
    const int32_t alone = ftol(static_cast<float>(g.xp) / g.groupRate);
    const std::string xp = std::to_string(g.xp);
    const std::string diff = std::to_string(std::abs(static_cast<int64_t>(g.xp) - alone));
    if (g.haveVictim && g.victimIsUnit) {
        if (alone == g.xp)
            return {"COMBATLOG_XPGAIN_FIRSTPERSON", "%s dies, you gain %d experience.", {g.victimName, xp}};
        if (g.xp > alone)
            return {"COMBATLOG_XPGAIN_FIRSTPERSON_GROUP",
                    "%s dies, you gain %d experience. (+%d group bonus)", {g.victimName, xp, diff}};
        return {"COMBATLOG_XPGAIN_FIRSTPERSON_RAID",
                "%s dies, you gain %d experience. (-%d raid penalty)", {g.victimName, xp, diff}};
    }
    if (alone == g.xp)
        return {"COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED", "You gain %d experience.", {xp}};
    if (g.xp > alone)
        return {"COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED_GROUP",
                "You gain %d experience. (+%d group bonus)", {xp, diff}};
    return {"COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED_RAID",
            "You gain %d experience. (-%d raid penalty)", {xp, diff}};
}

/// The rest-state forms by rest state (the table at 0x00ac7fb4; 0 has none,
/// 3 is the literal "NULL", 6 - Recruit-a-Friend - reuses the rested one).
struct RestForm { const char* key; const char* group; const char* raid; const char* enUS; const char* enGroup; const char* enRaid; };
inline const RestForm* restForm(int state) {
    static const RestForm bonus1{"COMBATLOG_XPGAIN_EXHAUSTION1", "COMBATLOG_XPGAIN_EXHAUSTION1_GROUP", "COMBATLOG_XPGAIN_EXHAUSTION1_RAID",
        "%s dies, you gain %d experience. (%s exp %s bonus)",
        "%s dies, you gain %d experience. (%s exp %s bonus, +%d group bonus)",
        "%s dies, you gain %d experience. (%s exp %s bonus, -%d raid penalty)"};
    static const RestForm bonus2{"COMBATLOG_XPGAIN_EXHAUSTION2", "COMBATLOG_XPGAIN_EXHAUSTION2_GROUP", "COMBATLOG_XPGAIN_EXHAUSTION2_RAID",
        "%s dies, you gain %d experience. (%s exp %s bonus)",
        "%s dies, you gain %d experience. (%s exp %s bonus, +%d group bonus)",
        "%s dies, you gain %d experience. (%s exp %s bonus, -%d raid penalty)"};
    static const RestForm null3{"NULL", "NULL_GROUP", "NULL_RAID", "", "", ""};
    static const RestForm pen4{"COMBATLOG_XPGAIN_EXHAUSTION4", "COMBATLOG_XPGAIN_EXHAUSTION4_GROUP", "COMBATLOG_XPGAIN_EXHAUSTION4_RAID",
        "%s dies, you gain %d experience. (%s exp %s penalty)",
        "%s dies, you gain %d experience. (%s exp %s penalty, +%d group bonus)",
        "%s dies, you gain %d experience. (%s exp %s penalty, -%d raid penalty)"};
    static const RestForm pen5{"COMBATLOG_XPGAIN_EXHAUSTION5", "COMBATLOG_XPGAIN_EXHAUSTION5_GROUP", "COMBATLOG_XPGAIN_EXHAUSTION5_RAID",
        "%s dies, you gain %d experience. (%s exp %s penalty)",
        "%s dies, you gain %d experience. (%s exp %s penalty, +%d group bonus)",
        "%s dies, you gain %d experience. (%s exp %s penalty, -%d raid penalty)"};
    switch (state) {
        case 1: case 6: return &bonus1;
        case 2: return &bonus2;
        case 3: return &null3;
        case 4: return &pen4;
        case 5: return &pen5;
        default: return nullptr;
    }
}

/// 0x0050b300 (the self-kill case, COMBATLOG_XPLOSS_*, is 0x0050b660 and not
/// here).
inline Line choose(const Gain& g, const RowOf& rowOf) {
    if (!g.haveVictim || g.type != 0) {
        // A Recruit-a-Friend quest or exploration gain: the bonus is two
        // thirds of the gain, rounded to nearest, with row 6's name.
        if (g.referAFriend && (g.type == 1 || g.type == 6)) {
            const int32_t bonus = static_cast<int32_t>(std::nearbyint(static_cast<float>(g.xp) * 0.6666667f));
            const ExhaustionRow row = rowOf(6);
            return {"COMBATLOG_XPGAIN_QUEST", "You gain %d experience. (%s exp %s bonus)",
                    {std::to_string(g.xp), signedText(bonus), row.name, std::to_string(bonus)}};
        }
        return plain(g);
    }
    // A kill: the rest state (Recruit-a-Friend counting as 6), and its row's
    // factor. A factor of 0 or 1 changes nothing, and the plain forms serve.
    const int state = g.referAFriend ? 6 : g.restState;
    const ExhaustionRow row = rowOf(state);
    const RestForm* form = restForm(state);
    if (!row.found || row.factor == 0.0f || row.factor == 1.0f || !form) return plain(g);
    const int32_t alone = ftol(static_cast<float>(g.restlessXp) / g.groupRate);
    const std::string rested = signedText(g.xp - g.restlessXp);
    std::vector<std::string> args{g.victimName, std::to_string(g.xp), rested, row.name};
    if (alone == g.restlessXp) return {form->key, form->enUS, args};
    args.push_back(std::to_string(std::abs(static_cast<int64_t>(g.restlessXp) - alone)));
    if (alone < g.restlessXp) return {form->group, form->enGroup, args};
    return {form->raid, form->enRaid, args};
}

} // namespace xp_gain_line
} // namespace game
} // namespace wowee
