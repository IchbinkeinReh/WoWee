#pragma once

// The tutorial pop-outs' state, held as the client holds it: two bit sets
// the server fills from SMSG_TUTORIAL_FLAGS (0x00530920 copies the payload
// into both) - "triggered" at 0xbd1aec and "flagged" at 0xbd1afc - and a list
// of the ones seen this session in the order they were seen (0xbd19f0,
// sixty entries of id + 1), which is what the tutorial frame's previous and
// next buttons walk. Ids here count from 0, as the client's own functions
// take them; the Lua functions take them from 1.

#include <array>
#include <cstddef>
#include <cstdint>

namespace wowee::game {

class TutorialState {
public:
    static constexpr uint32_t kCount = 60;  ///< the Lua functions refuse 61 and up
    static constexpr std::size_t kWords = 8; ///< 256 bits, the 32 bytes the server sends

    /// SMSG_TUTORIAL_FLAGS (0x00530920): the payload into both sets.
    void loadFlags(const std::array<uint32_t, kWords>& words) {
        triggered_ = words;
        flagged_ = words;
        loaded_ = true;
    }
    [[nodiscard]] bool loaded() const { return loaded_; }

    /// 0x005222b0, IsTutorialFlagged.
    [[nodiscard]] bool flagged(uint32_t id) const { return id < kWords * 32 && test(flagged_, id); }

    /// 0x00530450: false when it already was; else both bits set, the id put on
    /// the session list (0x00530100) - and the caller sends CMSG_TUTORIAL_FLAG.
    bool flag(uint32_t id) {
        if (id >= kWords * 32 || test(flagged_, id)) return false;
        set(triggered_, id);
        set(flagged_, id);
        remember(id);
        return true;
    }

    enum class Trigger : uint8_t { Nothing, Popup, Flag };

    /// 0x00530840: nothing for one already triggered. Otherwise it is
    /// triggered, and with showTutorials on it pops out (TUTORIAL_TRIGGER);
    /// with it off it is flagged instead, as though it had been seen.
    /// Nothing at all until the server's flags have come (0xbd1ae0 is their
    /// bit count, zero before SMSG_TUTORIAL_FLAGS).
    Trigger trigger(uint32_t id, bool showTutorials) {
        if (!loaded_ || id >= kWords * 32 || test(triggered_, id)) return Trigger::Nothing;
        set(triggered_, id);
        return showTutorials ? Trigger::Popup : Trigger::Flag;
    }

    /// 0x00530700, CanResetTutorials: any triggered at all.
    [[nodiscard]] bool anyTriggered() const {
        for (uint32_t w : triggered_) if (w != 0) return true;
        return false;
    }

    /// 0x00530510, ClearTutorials: every bit set; the session list is left
    /// as it was (its loop only adds the ones still unflagged, and there are
    /// none by then).
    void setAll() {
        triggered_.fill(0xFFFFFFFFu);
        flagged_.fill(0xFFFFFFFFu);
    }

    /// 0x00530630, ResetTutorials: both sets and the session list emptied.
    void reset() {
        triggered_.fill(0);
        flagged_.fill(0);
        order_.fill(0);
    }

    /// 0x00530140, GetNextCompleatedTutorial: the one seen after `id` this
    /// session, as an id counted from 1; 0 for none.
    [[nodiscard]] uint32_t nextSeen(uint32_t id) const {
        bool found = false;
        for (uint32_t e : order_) {
            if (found && e != 0) return e;
            if (e == id + 1) found = true;
        }
        return 0;
    }

    /// 0x00530190, GetPrevCompleatedTutorial: the one seen before it.
    [[nodiscard]] uint32_t prevSeen(uint32_t id) const {
        bool found = false;
        for (std::size_t i = order_.size(); i-- > 0;) {
            if (found && order_[i] != 0) return order_[i];
            if (order_[i] == id + 1) found = true;
        }
        return 0;
    }

private:
    static bool test(const std::array<uint32_t, kWords>& a, uint32_t id) {
        return (a[id >> 5] & (1u << (id & 31))) != 0;
    }
    static void set(std::array<uint32_t, kWords>& a, uint32_t id) { a[id >> 5] |= 1u << (id & 31); }
    void remember(uint32_t id) {
        for (uint32_t& e : order_) {
            if (e == id + 1) return;
            if (e == 0) { e = id + 1; return; }
        }
    }

    std::array<uint32_t, kWords> triggered_{};
    std::array<uint32_t, kWords> flagged_{};
    std::array<uint32_t, kCount> order_{};
    bool loaded_ = false;
};

}  // namespace wowee::game
