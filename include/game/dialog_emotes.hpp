#pragma once

// What an NPC does as it talks: the (delay, emote) pairs a quest list, a
// quest's details, its items asked for, its reward offered and a gossip
// greeting carry, played on the NPC one after another (0x0071a260 queues
// them on the unit, +0x9fc; 0x0073adc0 plays them as they come due).

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace wowee::game {

struct DialogEmote {
    uint32_t delayMs = 0;
    uint32_t emote = 0;  ///< Emotes.dbc id
};

class DialogEmoteQueue {
public:
    /// 0x0071a260: the pairs taken in order, up to four, keeping only those
    /// naming an emote whose row is a one-shot (EmoteSpecProc 0) - asked of
    /// `oneShot`. The first's delay counts from `nowMs`; each later one's
    /// from when the one before it has played (see take). Nothing given
    /// leaves nothing queued.
    template <class OneShot>
    void start(const std::vector<DialogEmote>& pairs, uint32_t nowMs, const OneShot& oneShot) {
        active_ = false;
        slots_ = {};
        if (pairs.empty()) return;
        size_t n = 0;
        for (const DialogEmote& p : pairs) {
            if (n >= slots_.size()) break;
            if (p.emote != 0 && oneShot(p.emote)) slots_[n++] = p;
        }
        slots_[0].delayMs += nowMs;
        active_ = true;
    }

    /// 0x0073adc0, each frame: the emote due now, if any. Once one plays,
    /// the next one's delay is counted from now, plus the animation's
    /// length (`lengthMs`, asked of the emote just played) and half a
    /// second. Nothing left ends the queue.
    template <class LengthMs>
    std::optional<uint32_t> take(uint32_t nowMs, const LengthMs& lengthMs) {
        if (!active_) return std::nullopt;
        size_t i = 0;
        while (i < slots_.size() && slots_[i].emote == 0) ++i;
        if (i == slots_.size()) {
            active_ = false;
            return std::nullopt;
        }
        if (nowMs < slots_[i].delayMs) return std::nullopt;
        const uint32_t emote = slots_[i].emote;
        slots_[i].emote = 0;
        if (i + 1 < slots_.size() && slots_[i + 1].emote != 0) {
            slots_[i + 1].delayMs += nowMs + 500 + lengthMs(emote);
        } else {
            active_ = false;
        }
        return emote;
    }

    [[nodiscard]] bool active() const { return active_; }

private:
    std::array<DialogEmote, 4> slots_{};
    bool active_ = false;
};

}  // namespace wowee::game
