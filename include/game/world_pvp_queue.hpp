#pragma once

// The world PvP (Wintergrasp) queue as the client keeps it: one battle at a
// time, its status, its zone and when an invitation to enter runs out
// (0x00bea4a8..0x00bea4b4, BattlefieldInfo.cpp), and what
// GetWorldPVPQueueStatus answers of it (0x0054c8a0).

#include <cstdint>

namespace wowee::game::world_pvp_queue {

enum Status : uint32_t { kNone = 0, kQueued = 1, kConfirm = 2, kActive = 3 };

struct State {
    uint32_t battleId = 0;   // 0x00bea4a8
    uint32_t zoneId = 0;     // 0x00bea4ac, an AreaTable id
    uint32_t status = kNone; // 0x00bea4b0
    int64_t acceptTime = 0;  // 0x00bea4b4, the server's time the invitation ends

    /// 0x00549500: another battle's state is kept while it has a zone and a
    /// status; a zone or a time of zero leaves the one there.
    void set(uint32_t battle, uint32_t newStatus, uint32_t zone, int64_t time) {
        if (battleId != battle && zoneId != 0 && status != kNone) return;
        if (zone != 0) zoneId = zone;
        if (time != 0) acceptTime = time;
        battleId = battle;
        status = newStatus;
    }
    /// SMSG_BATTLEFIELD_MGR_ENTERED (0x005498c0) and EJECTED for most reasons
    /// (0x005499c0): this battle's state, or one with no zone or status,
    /// goes back to none; the zone and time stay.
    void clear(uint32_t battle) {
        if (battleId == battle || zoneId == 0 || status == kNone) {
            battleId = battle;
            status = kNone;
        }
    }
    /// The world's state at a logout (00540000.c:11119).
    void reset() { *this = State{}; }

    [[nodiscard]] const char* statusName() const {
        switch (status) {
            case kNone: return "none";
            case kQueued: return "queued";
            case kConfirm: return "confirm";
            case kActive: return "active";
            default: return "error";
        }
    }
    /// The fourth value: the seconds left to enter while an invitation is
    /// out (status 2), by the server's time `now` (0x00548700); else 0.
    [[nodiscard]] int64_t secondsLeft(int64_t now) const {
        return status == kConfirm ? acceptTime - now : 0;
    }
};

/// SMSG_BATTLEFIELD_MGR_EJECTED's reasons that let the queue go (the
/// switch of 0x005499c0): 1, 2, 4, 5 and 7 to 11.
inline bool ejectClears(uint8_t reason) {
    switch (reason) {
        case 1: case 2: case 4: case 5: case 7: case 8: case 9: case 10: case 11: return true;
        default: return false;
    }
}

/// BATTLEFIELD_MGR_EJECTED's four flags as 0x005499c0 raises them
/// ("%d%b%b%b%b" after the battle id): the reason is a number, not a mask -
/// the player left (8), relocated, the battle on (status 2), too low (10).
struct EjectFlags {
    bool exited = false;
    bool relocated = false;
    bool battleActive = false;
    bool lowLevel = false;
};
inline EjectFlags ejectFlags(uint8_t reason, uint8_t battleStatus, uint8_t relocated) {
    return EjectFlags{reason == 8, relocated != 0, battleStatus == 2, reason == 10};
}

}  // namespace wowee::game::world_pvp_queue
