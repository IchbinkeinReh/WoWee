#pragma once

#include "game/click_to_move.hpp"

#include <cstdint>

// The player's own attack, as the client keeps it and nothing of its own: the
// swing is asked for at any distance, goes on until the server or a change of
// target ends it, and being out of reach is said when the server says so.
namespace wowee::game {

/// CGUnit_C +0xa20..+0xa2c of the player.
struct AutoAttack {
    uint64_t target = 0;        ///< +0xa20: whom it is swung at
    bool startPending = false;  ///< +0xa28: CMSG_ATTACKSWING not answered
    bool stopPending = false;   ///< +0xa2c: CMSG_ATTACKSTOP not answered

    /// 0x0071af90.
    [[nodiscard]] bool attacking() const { return target != 0; }
    /// 0x006e2610: whether a CMSG_ATTACKSWING goes out - none while one is on
    /// or asked for, unless its stop is.
    [[nodiscard]] bool maySwing() const { return stopPending || (target == 0 && !startPending); }
    /// 0x006e1660: whether there is anything to stop with CMSG_ATTACKSTOP.
    [[nodiscard]] bool mayStop() const { return attacking() || startPending; }
    /// 0x0098e540, CMSG_ATTACKSWING sent.
    void swingSent(uint64_t guid) {
        target = guid;
        startPending = true;
    }
    /// 0x007559e0, CMSG_ATTACKSTOP sent.
    void stopSent() { stopPending = true; }
    /// SMSG_ATTACKSTART (0x00756800 -> 0x00754ff0).
    void started(uint64_t guid) {
        startPending = false;
        target = guid;
    }
    /// SMSG_ATTACKSTOP (0x00756770), and the player object's end (0x00729220).
    void stopped() { *this = AutoAttack{}; }
};

/// What the server refused the last swing for (0x00c9eaa8) and when that is
/// said next (0x00c9d544).
class SwingRefusal {
public:
    enum class Why : uint8_t { None, Distance, Facing };
    static constexpr int64_t kRepeatMs = 4000;

    /// SMSG_ATTACKSWING_NOTINRANGE and _BADFACING (0x006cee70).
    void refused(Why why, int64_t nowMs) {
        why_ = why;
        dueMs_ = nowMs;
    }
    /// A swing of the player's that came off (0x006d4ad0), and a new world.
    void clear(int64_t nowMs) { refused(Why::None, nowMs); }
    /// 0x006d5eb0, each frame: said at once and every four seconds after it,
    /// for as long as the attack is on and no swing came off.
    [[nodiscard]] Why due(bool attacking, int64_t nowMs) {
        if (!attacking || why_ == Why::None || nowMs < dueMs_) return Why::None;
        dueMs_ = nowMs + kRepeatMs;
        return why_;
    }

private:
    Why why_ = Why::None;
    int64_t dueMs_ = 0;
};

/// 0x006d4ad0: a swing of the player's that came off at a unit four times the
/// melee reach away, or more, is the attack's last (CMSG_ATTACKSTOP).
inline bool swingLeftBehind(float distanceSq, float reachA, float reachB) {
    const float reach = click_to_move::meleeReach(reachA, reachB);
    return reach * 4.0f * reach * 4.0f <= distanceSq;
}

}  // namespace wowee::game
