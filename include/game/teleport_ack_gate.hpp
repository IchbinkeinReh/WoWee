#pragma once

#include <cstdint>
#include <optional>

namespace wowee::game {

/// When MSG_MOVE_TELEPORT_ACK goes out.
///
/// The client answers a teleport from the handler that takes it, after the
/// player has been moved and the area about the new place read in:
/// 0x006ed0f0 calls 0x006ecf80 (which blocks in 0x0074b620 while the tiles
/// load behind the map's loading screen) and only then sends opcode 0xC7
/// through 0x007413f0. A teleport whose new place needs that load holds the
/// acknowledgement until the load is done; any other goes at once.
///
///   arrive()  - a teleport came; it is pending, not held. What it returns is
///               an earlier one still unanswered, which goes first.
///   hold()    - its place is being loaded (WorldLoader takes it next).
///   settle()  - the teleport's handler is done: the acknowledgement, unless
///               held.
///   release() - the load is done: the held acknowledgement, if any.
class TeleportAckGate {
public:
    struct Ack {
        uint32_t counter = 0;
        uint32_t moveTime = 0;
    };

    [[nodiscard]] std::optional<Ack> arrive(uint32_t counter, uint32_t moveTime) {
        std::optional<Ack> earlier = pending_;
        pending_ = Ack{counter, moveTime};
        held_ = false;
        return earlier;
    }
    void hold() { held_ = true; }
    [[nodiscard]] std::optional<Ack> settle() {
        if (held_) return std::nullopt;
        return take();
    }
    [[nodiscard]] std::optional<Ack> release() {
        held_ = false;
        return take();
    }
    /// A new session's world: the last one's teleport is not answered.
    void drop() {
        pending_.reset();
        held_ = false;
    }
    [[nodiscard]] bool pending() const { return pending_.has_value(); }
    [[nodiscard]] bool held() const { return held_ && pending_.has_value(); }

private:
    std::optional<Ack> take() {
        std::optional<Ack> a = pending_;
        pending_.reset();
        return a;
    }
    std::optional<Ack> pending_;
    bool held_ = false;
};

}  // namespace wowee::game
