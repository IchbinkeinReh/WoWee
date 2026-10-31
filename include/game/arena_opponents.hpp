#pragma once

/// The arena's opponents as the client keeps them (BattlefieldInfo.cpp): five
/// players (0xbe9f48) and five pets each with its owner (0xbe9ef8), filled
/// as they come into sight in an arena and named by the tokens arena1..5 and
/// arenapet1..5. Each change is ARENA_OPPONENT_UPDATE(unit, state) - "seen",
/// "unseen" for one not in sight, "destroyed" for SMSG_ARENA_UNIT_DESTROYED.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wowee::game::arena {

struct OpponentUpdate {
    std::string unit;   ///< "arena2", "arenapet1"
    std::string state;  ///< "seen", "unseen", "destroyed"
    bool operator==(const OpponentUpdate&) const = default;
};

class Roster {
public:
    static constexpr size_t kSlots = 5;
    struct Pet {
        uint64_t pet = 0;
        uint64_t owner = 0;
    };

    /// 0x0054d100: an opponent seen - into its pet's slot where its pet came
    /// first, else the first slot free of a pet that is its own or empty.
    std::vector<OpponentUpdate> opponentSeen(uint64_t guid, bool present) {
        for (size_t i = 0; i < kSlots; ++i)
            if (pets_[i].owner == guid) return setOpponent(i, guid, present);
        for (size_t i = 0; i < kSlots; ++i) {
            if (pets_[i].pet == 0 && (opponents_[i] == guid || opponents_[i] == 0))
                return setOpponent(i, guid, present);
        }
        return {};
    }

    /// 0x0054d1c0: an opponent's pet seen - beside its owner, in its own old
    /// slot, or in the first slot with neither.
    std::vector<OpponentUpdate> petSeen(uint64_t pet, uint64_t owner, bool present) {
        for (size_t i = 0; i < kSlots; ++i) {
            if (opponents_[i] == owner) return setPet(i, pet, owner, present);
            if (pets_[i].owner == owner) return setPet(i, pet, owner, present);
        }
        for (size_t i = 0; i < kSlots; ++i) {
            if (opponents_[i] == 0 && pets_[i].pet == 0) return setPet(i, pet, owner, present);
        }
        return {};
    }

    /// 0x0054ad10: an opponent, an opponent's pet (by what the opponent in
    /// sight has charmed or summoned, `petOf`), or a pet in its slot, gone.
    /// Only the last is cleared.
    std::vector<OpponentUpdate> destroyed(uint64_t guid, const std::function<uint64_t(uint64_t)>& petOf) {
        for (size_t i = 0; i < kSlots; ++i) {
            if (opponents_[i] == guid) return {{token("arena", i), "destroyed"}};
            if (opponents_[i] != 0 && petOf && petOf(opponents_[i]) == guid)
                return {{token("arenapet", i), "destroyed"}};
        }
        for (size_t i = 0; i < kSlots; ++i) {
            if (pets_[i].pet == guid) {
                pets_[i] = {};
                return {{token("arenapet", i), "destroyed"}};
            }
        }
        return {};
    }

    void clear() {
        opponents_.fill(0);
        pets_.fill({});
    }
    [[nodiscard]] uint64_t opponent(size_t slot) const { return slot < kSlots ? opponents_[slot] : 0; }
    [[nodiscard]] uint64_t pet(size_t slot) const { return slot < kSlots ? pets_[slot].pet : 0; }

    /// GetNumArenaOpponents' answer: 0x00549630 walks the five slots and **stops at
    /// the first one whose **two guid pairs are both empty** - the opponent's pair
    /// at 0x00be9f48 and the pet's at 0x00be9ef8, sixteen bytes a slot, and the
    /// pet's *owner* pair is not part of the test. So it is a prefix length rather
    /// than a tally: an opponent destroyed in slot 2 of five cuts it to 1 even with
    /// three still in sight. Both consumers use it as an index into the enemy
    /// panel - `uiparent.lua:1927` anchors the watch frame to
    /// `"ArenaEnemyFrame"..numArenaOpponents`, `blizzard_arenaui.lua:315` to
    /// `"ArenaEnemyFrame"..numOpps.."PetFrame"` - which is what makes the walk stop
    /// instead of count: the client is asking which is the last frame it has.
    [[nodiscard]] size_t numOpponents() const {
        size_t n = 0;
        while (n < kSlots && (opponents_[n] != 0 || pets_[n].pet != 0)) ++n;
        return n;
    }

private:
    static std::string token(const char* kind, size_t slot) { return kind + std::to_string(slot + 1); }
    /// 0x0054b840 and 0x0054b920: "seen", and "unseen" for one not in sight.
    static std::vector<OpponentUpdate> told(const std::string& unit, bool present) {
        std::vector<OpponentUpdate> out = {{unit, "seen"}};
        if (!present) out.push_back({unit, "unseen"});
        return out;
    }
    std::vector<OpponentUpdate> setOpponent(size_t i, uint64_t guid, bool present) {
        opponents_[i] = guid;
        return told(token("arena", i), present);
    }
    std::vector<OpponentUpdate> setPet(size_t i, uint64_t pet, uint64_t owner, bool present) {
        pets_[i] = {pet, owner};
        return told(token("arenapet", i), present);
    }

    std::array<uint64_t, kSlots> opponents_{};
    std::array<Pet, kSlots> pets_{};
};

}  // namespace wowee::game::arena
