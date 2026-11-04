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
#include <vector>

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
    /// 0x00522270: loaded, and this one not triggered yet.
    [[nodiscard]] bool awaiting(uint32_t id) const { return loaded_ && id < kWords * 32 && !test(triggered_, id); }

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

/// What SMSG_ITEM_PUSH_RESULT's handler (0x00523900) looks at for its
/// tutorials: the packet's receiver and slot, the item's template and the
/// player's level and class.
struct ItemPushTutorialInput {
    bool self = false;            ///< the item went to the local player
    uint32_t itemId = 0;
    uint32_t itemClass = 0;       ///< template +4
    uint32_t subClass = 0;        ///< +8
    uint32_t inventoryType = 0;   ///< +0x28
    bool hasUseSpell = false;     ///< a spell (+0x100) with trigger 0 (+0x114)
    uint8_t bagSlot = 0;          ///< 255 for the backpack and the slots on the player
    uint32_t slot = 0;
    uint32_t playerLevel = 0;
    uint8_t playerClass = 0;
    bool equipTutorialSeen = false;  ///< tutorial 0x37 flagged (0x005222b0)
};

/// The tutorials an item push triggers, in the order 0x00523900 asks for
/// them. On the player's own:
///   - 7 always;
///   - 8 for an item with a use spell, unless it is food or a scroll;
///   - 0x17 for one that can be worn, once tutorial 0x37 has been seen;
///   - 9 for a container;
///   - 0x31 for one put on the key ring (slots 86..117 of the player);
///   - 0x1e for the Hearthstone (6948);
///   - 10 and 11 for food or a scroll after level 1.
/// Then 0x2b for a ranged weapon of the player's who is no hunter, or a wand
/// pushed to anyone.
[[nodiscard]] inline std::vector<uint32_t> itemPushTutorials(const ItemPushTutorialInput& in) {
    // DAT_00a2d288: the equipment slots each inventory type fits.
    static constexpr std::array<uint32_t, 29> kSlotMask = {
        0x0,     0x1,    0x2,      0x4,     0x8,     0x10,    0x20,    0x40,     0x80,       0x100,
        0x200,   0xc00,  0x3000,   0x18000, 0x10000, 0x20000, 0x4000,  0x18000,  0x780000,   0x40000,
        0x10,    0x18000, 0x18000, 0x10000, 0x0,     0x20000, 0x20000, 0x0,      0x20000};
    std::vector<uint32_t> out;
    const bool consumableFoodOrScroll = in.itemClass == 0 && (in.subClass == 4 || in.subClass == 5);
    if (in.self) {
        out.push_back(7);
        if (in.hasUseSpell && !consumableFoodOrScroll) out.push_back(8);
        const uint32_t mask = in.inventoryType < kSlotMask.size() ? kSlotMask[in.inventoryType] : 0;
        if ((mask & 0x7fffffu) != 0 && in.equipTutorialSeen) out.push_back(0x17);
        if (in.itemClass == 1) out.push_back(9);
        if (in.bagSlot == 0xff && in.slot - 0x56u < 0x20u) out.push_back(0x31);
        if (in.itemId == 0x1b24) out.push_back(0x1e);
        if (in.playerLevel > 1 && consumableFoodOrScroll) {
            out.push_back(10);
            out.push_back(11);
        }
    }
    if ((in.self && (in.inventoryType == 15 || in.inventoryType == 26) && in.playerClass != 3) ||
        (in.itemClass == 2 && in.subClass == 0x13))
        out.push_back(0x2b);
    return out;
}

/// The counter 0x0080cce0 keeps as the player casts (DAT_00af53c4,
/// DAT_00d3f604): until tutorial 5 is flagged it stays armed at nought;
/// after, each attack - an Attack effect (78), a ranged spell (Attributes
/// 0x2) or one that repeats (AttributesEx2 0x20) - counts one and anything
/// else disarms it, and more than ten in a row trigger tutorial 0x3b.
struct AttackStreak {
    bool armed = true;
    uint32_t count = 0;
    /// One cast by the player; true where tutorial 0x3b is due.
    bool note(bool tutorial5Flagged, bool attack) {
        if (!tutorial5Flagged) {
            armed = true;
            count = 0;
            return false;
        }
        if (!armed) return false;
        if (!attack) {
            armed = false;
        } else {
            ++count;
        }
        return count > 10;
    }
};

}  // namespace wowee::game
