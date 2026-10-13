#pragma once

/// How one unit regards another, as the client decides it (Unit_C.cpp):
/// 0x007251c0, 0 hated to 7 exalted - UnitReaction less one. Between two
/// units a player controls, their players' duel, their being one player,
/// or the player's group; both free-for-all is hostile. Asked by what the
/// player controls of a unit whose faction the player has a standing with,
/// the server's forced reaction, else at war hostile and anything else
/// friendly. Otherwise the asker's FactionTemplate of the unit (0x0071f770):
/// asked of what the player controls, a contested guard of a contested
/// player, a forced reaction, the standing; else template against template
/// (0x00715440). Arithmetic only: the units and the standings are the
/// caller's.

#include <array>
#include <cstdint>

namespace wowee::game::reaction {

inline constexpr int kHostile = 1;
inline constexpr int kNeutral = 3;
inline constexpr int kFriendly = 4;

/// A FactionTemplate.dbc row as the client keeps it (by id, 0x00ad38a4).
struct FactionTemplate {
    uint32_t id = 0;
    uint32_t faction = 0;                ///< +0x4, Faction.dbc
    uint32_t flags = 0;                  ///< +0x8: 0x1000 contested guard, 0x2000 hostile by default
    uint32_t factionGroup = 0;           ///< +0xc
    uint32_t friendGroup = 0;            ///< +0x10
    uint32_t enemyGroup = 0;             ///< +0x14
    std::array<uint32_t, 4> enemies{};   ///< +0x18, up to the first 0
    std::array<uint32_t, 4> friends{};   ///< +0x28, likewise
};

namespace detail {
constexpr bool names(const std::array<uint32_t, 4>& list, uint32_t faction) {
    for (const uint32_t f : list) {
        if (f == 0) break;
        if (f == faction) return true;
    }
    return false;
}
}  // namespace detail

/// 0x00715440: how template `a` regards template `b`. An enemy by group or by
/// name is hostile; a friend by group or by name, either way round, friendly;
/// else hostile by default (0x2000) or neutral.
[[nodiscard]] constexpr int templateReaction(const FactionTemplate& a, const FactionTemplate& b) {
    if ((a.enemyGroup & b.factionGroup) != 0) return kHostile;
    if (detail::names(a.enemies, b.faction)) return kHostile;
    if ((a.friendGroup & b.factionGroup) != 0) return kFriendly;
    if (detail::names(a.friends, b.faction)) return kFriendly;
    if ((b.friendGroup & a.factionGroup) != 0) return kFriendly;
    if (detail::names(b.friends, a.faction)) return kFriendly;
    return (a.flags & 0x2000u) != 0 ? kHostile : kNeutral;
}

/// 0x005d0600: a standing - where it starts plus what is earned - as a rank:
/// 42000 exalted (7), 21000 revered, 9000 honored, 3000 friendly (4), from 0
/// neutral (3), from -3000 unfriendly, from -6000 hostile (1), else hated.
[[nodiscard]] constexpr int standingRank(int32_t standing) {
    if (standing >= 42000) return 7;
    if (standing >= 21000) return 6;
    if (standing >= 9000) return 5;
    if (standing >= 3000) return 4;
    if (standing >= 0) return 3;
    if (standing >= -3000) return 2;
    return standing >= -6000 ? 1 : 0;
}

/// A player as the reaction reads one (PLAYER_DUEL_ARBITER, PLAYER_FLAGS and
/// PLAYER_DUEL_TEAM at +0x1008 [0], [8], [0x20]).
struct Player {
    bool found = false;
    uint64_t guid = 0;
    uint64_t duelArbiter = 0;
    uint32_t duelTeam = 0;
    uint32_t playerFlags = 0;  ///< 0x100 contested PvP
    uint32_t unitFlags2 = 0;   ///< UNIT_FIELD_FLAGS_2 (+0xd8), 0x4 ignores reputation
    bool grouped = false;      ///< in the player's party or raid (0x0052c680, 0x00512a00)
};

/// One unit.
struct Who {
    uint64_t guid = 0;
    uint32_t factionTemplate = 0;    ///< UNIT_FIELD_FACTIONTEMPLATE (+0xc4)
    uint32_t unitFlags = 0;          ///< UNIT_FIELD_FLAGS (+0xd4), 0x8 player controlled
    uint32_t unitFlags2 = 0;         ///< UNIT_FIELD_FLAGS_2 (+0xd8)
    uint32_t pvpFlags = 0;           ///< UNIT_FIELD_BYTES_2 byte 1 (+0x1d1), 0x4 free-for-all
    uint64_t charmerOrCreator = 0;   ///< 0x004f5f20
    Player self;                     ///< itself, when a player
    Player behind;                   ///< the player behind it (0x00718b70)
};

/// What the client keeps of the player, which `World` answers:
///   const FactionTemplate* factionTemplate(uint32_t id)  - 0x0065c290
///   bool forced(uint32_t faction, int& reaction)          - 0x005d06a0
///   bool tracked(uint32_t faction)  - a standing kept (0x00718b30)
///   bool atWar(uint32_t faction)                          - 0x005d04b0
///   int rank(uint32_t faction)      - standingRank of it (0x005d0600)
///   uint64_t activePlayer, duelOpponent                   - 0x004d3790, 0x00c22b60

/// 0x0071f770: how a template regards a unit.
template <typename World>
[[nodiscard]] int templateReactionTo(const World& w, uint32_t templateId, const Who& unit) {
    const FactionTemplate* a = w.factionTemplate(templateId);
    const FactionTemplate* b = w.factionTemplate(unit.factionTemplate);
    if (a == nullptr || b == nullptr) return kNeutral;
    if ((unit.unitFlags & 0x8u) != 0 && unit.behind.found && unit.behind.guid == w.activePlayer) {
        if ((a->flags & 0x1000u) != 0 && (unit.behind.playerFlags & 0x100u) != 0) return kHostile;
        if (int forced = 0; w.forced(a->faction, forced)) return forced;
        if ((unit.unitFlags2 & 0x4u) == 0 && w.tracked(a->faction)) return w.rank(a->faction);
    }
    return templateReaction(*a, *b);
}

/// 0x0071f5c0: between two the player controls, one with no player behind
/// it: the player's duel opponent's, else - both players - in a duel on
/// opposite sides.
template <typename World>
[[nodiscard]] bool duelOpponents(const World& w, const Who& a, const Who& b) {
    if ((a.unitFlags & 0x8u) == 0 || (b.unitFlags & 0x8u) == 0) return false;
    const Player& pa = a.self.found ? a.self : a.behind;
    const Player& pb = b.self.found ? b.self : b.behind;
    if (!pa.found || !pb.found) {
        if (a.guid == w.activePlayer && !pb.found)
            return b.charmerOrCreator != 0 && b.charmerOrCreator == w.duelOpponent;
        if (b.guid == w.activePlayer && !pa.found)
            return a.charmerOrCreator != 0 && a.charmerOrCreator == w.duelOpponent;
        return false;
    }
    return pa.duelTeam != 0 && pb.duelTeam != 0 && pa.duelArbiter == pb.duelArbiter && pa.duelTeam != pb.duelTeam;
}

/// 0x007251c0: how `asker` regards `unit`, 0 to 7.
template <typename World>
[[nodiscard]] int unitReaction(const World& w, const Who& asker, const Who& unit) {
    if (asker.guid == unit.guid) return kFriendly;
    const bool askerPc = (asker.unitFlags & 0x8u) != 0;
    if (askerPc && (unit.unitFlags & 0x8u) != 0) {
        const Player& p1 = asker.behind;
        const Player& p2 = unit.behind;
        bool decided = false;
        if (!p1.found || !p2.found) {
            if (asker.guid == w.activePlayer && !p2.found) {
                if (duelOpponents(w, asker, unit)) return kHostile;
            } else if (unit.guid == w.activePlayer && !p1.found) {
                if (duelOpponents(w, unit, asker)) return kHostile;
            }
        } else {
            if (p1.duelTeam != 0 && p2.duelTeam != 0 && p1.duelArbiter == p2.duelArbiter)
                return p1.duelTeam != p2.duelTeam ? kHostile : kFriendly;
            if (p1.guid == p2.guid) return kFriendly;
            decided = (p1.guid == w.activePlayer && p2.grouped) || (p2.guid == w.activePlayer && p1.grouped);
            if (decided) return templateReactionTo(w, asker.factionTemplate, unit);
        }
        if ((asker.pvpFlags & 0x4u) != 0 && (unit.pvpFlags & 0x4u) != 0) return kHostile;
    }
    if (askerPc && asker.behind.found && asker.behind.guid == w.activePlayer) {
        if (const FactionTemplate* t = w.factionTemplate(unit.factionTemplate)) {
            if (int forced = 0; w.forced(t->faction, forced)) return forced;
            if ((asker.behind.unitFlags2 & 0x4u) == 0 && w.tracked(t->faction)) {
                if ((t->flags & 0x1000u) != 0 && (asker.behind.playerFlags & 0x100u) != 0) return kHostile;
                return w.atWar(t->faction) ? kHostile : kFriendly;
            }
        }
    }
    const int r = templateReactionTo(w, asker.factionTemplate, unit);
    return r < 7 ? r : 7;
}

}  // namespace wowee::game::reaction
