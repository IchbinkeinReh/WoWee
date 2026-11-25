#pragma once

#include <cstddef>
#include <cstdint>

namespace wowee::audio {

/// Which of its lines a unit says as it is clicked (0x0072a680, 0x00746900): the hello five
/// times, then each of the irritated ones in turn, then the hello again. One unit is
/// counted at a time (0x00ca1190, the unit in 0x00ca1198), and a window's hello or the
/// farewell forgets it (0x00720860).
class NpcLineCounter {
public:
    struct Ask {
        bool irritated = false;
        uint32_t line = 0;  ///< which irritated one
        int next = 0;       ///< the count once it has started
    };

    [[nodiscard]] Ask ask(uint64_t unit, size_t irritatedLines) {
        if (unit != unit_) count_ = 0;
        unit_ = unit;
        if (count_ < kHellos) return {.next = count_ + 1};
        const auto line = static_cast<size_t>(count_ - kHellos);
        if (line >= irritatedLines) return {.next = 0};
        return {.irritated = true, .line = static_cast<uint32_t>(line), .next = count_ + 1};
    }
    void started(const Ask& ask) { count_ = ask.next; }
    void forget() { unit_ = 0; }

private:
    static constexpr int kHellos = 5;
    uint64_t unit_ = 0;
    int count_ = 0;
};

}  // namespace wowee::audio
