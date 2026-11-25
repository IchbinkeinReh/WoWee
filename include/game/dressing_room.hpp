#pragma once

#include "core/character_component.hpp"
#include "game/character.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// The dressing room's figure (DressUpModelFrame.cpp): what a piece tried on
// takes the place of. The client has no list of worn items to search: a piece
// goes to the slot its inventory type names (0x005980d0), and a weapon to a
// hand, which empties the other one where the two cannot be held together.

namespace wowee::game::dressing_room {

struct Item {
    uint32_t itemId = 0;
    uint32_t display = 0;
    uint8_t inventoryType = 0;
    /// The ItemVisuals row on it, 0 for its display's own (0x004eacd0).
    uint32_t visual = 0;
};

/// What the player the figure is of can hold.
struct Hands {
    /// 0x006ceb00.
    std::function<bool()> dualWield;
    /// 0x007082b0 with the off hand's slot: the item can go there.
    std::function<bool(uint32_t itemId)> fitsOffHand;
};

enum class TryOn : uint8_t {
    Worn,
    /// A neck, ring, trinket or relic: nothing is shown and nothing is said.
    NothingToShow,
    /// ERR_NOT_EQUIPPABLE (0x005216f0(0x15), 0x005980d0's default).
    NotEquippable,
};

inline constexpr int kMainHandSlot = 15;
inline constexpr int kOffHandSlot = 16;
inline constexpr int kRangedSlot = 17;

/// The body slot a piece of armour goes to (the character component's, the
/// first argument of 0x004f2830), or -1.
constexpr int bodySlot(uint8_t inventoryType) {
    switch (inventoryType) {
        case 1:  return 0;   // head
        case 3:  return 1;   // shoulders
        case 16: return 10;  // back
        default: return core::componentItemForInventoryType(inventoryType);
    }
}

/// What 0x005980d0 does with an inventory type, before the item is put on.
constexpr TryOn tryOnKind(uint8_t inventoryType) {
    if (bodySlot(inventoryType) >= 0) return TryOn::Worn;
    switch (inventoryType) {
        case 13: case 14: case 15: case 17: case 21: case 22: case 23: case 25: case 26:
            return TryOn::Worn;
        case 2: case 11: case 12: case 28:
            return TryOn::NothingToShow;
        default:
            return TryOn::NotEquippable;
    }
}

/// 0x007082b0(item, 16, 0): an item that can be put in the off hand. A weapon
/// of two hands or of the main hand (17, 21) only with a learnt spell whose
/// first effect is TITAN_GRIP (155; 0x006e7d24 keeps them at +0x18c0) that
/// names its class and subclass - never a fist weapon (13) nor one flagged
/// 0x40000000. Both rows that name weapons, 46917 and 48716, allow inventory
/// types that fit the hand (0x600000). The rest by the slots their type fits
/// (0x00a2d288).
template <typename GripAllows>
bool itemFitsOffHand(uint32_t itemClass, uint32_t subClass, uint8_t inventoryType, uint32_t flags,
                     GripAllows&& gripAllows) {
    if (itemClass == 2 && (inventoryType == 17 || inventoryType == 21)) {
        if ((flags & 0x40000000u) != 0 || subClass == 13) return false;
        return gripAllows(itemClass, subClass);
    }
    switch (inventoryType) {
        case 13: case 14: case 17: case 21: case 22: case 23: return true;
        default: return false;
    }
}

class Figure {
public:
    /// 0x005989e0: the unit's own items by equipment slot, the ranged one
    /// left out; the hands start over.
    void dress(const std::array<Item, 19>& bySlot, const Hands& hands) {
        *this = Figure{};
        for (int slot = 0; slot < static_cast<int>(bySlot.size()); ++slot) {
            const Item& item = bySlot[static_cast<std::size_t>(slot)];
            if (item.display == 0 || slot == kRangedSlot) continue;
            put(item, hands, slot == kMainHandSlot || slot == kOffHandSlot ? slot : -1);
        }
        // What is in each hand is the unit's own after (0x00598c0b), whatever
        // the dressing left out.
        const auto carried = [&](int slot) {
            const Item& item = bySlot[static_cast<std::size_t>(slot)];
            return item.display != 0 ? item.inventoryType : uint8_t{0};
        };
        main_.type = carried(kMainHandSlot);
        off_.type = carried(kOffHandSlot);
    }

    /// 0x00597ba0: every body slot and both hands emptied.
    void undress() {
        body_.fill(Item{});
        main_ = Hand{};
        off_ = Hand{};
    }

    /// 0x005980d0 for an item that is known.
    TryOn tryOn(const Item& item, const Hands& hands) {
        if (item.display == 0) return TryOn::NothingToShow;
        return put(item, hands, -1);
    }

    /// The figure as it stands.
    [[nodiscard]] std::vector<EquipmentItem> worn() const {
        std::vector<EquipmentItem> out;
        for (const Item& item : body_) {
            if (item.display == 0) continue;
            out.push_back({.displayModel = item.display, .inventoryType = item.inventoryType, .enchantment = 0u});
        }
        if (main_.item.display != 0) {
            out.push_back({.displayModel = main_.item.display, .inventoryType = main_.item.inventoryType,
                           .enchantment = main_.item.visual, .hand = EquipmentItem::Hand::Main});
        }
        if (off_.item.display != 0) {
            out.push_back({.displayModel = off_.item.display, .inventoryType = off_.item.inventoryType,
                           .enchantment = off_.item.visual, .hand = EquipmentItem::Hand::Off});
        }
        return out;
    }

private:
    struct Hand {
        Item item;         ///< What is drawn in it (0x004eacd0; taken off by 0x004eb070).
        uint32_t id = 0;   ///< 0x00c0e4f8, 0x00c0e4fc.
        uint8_t type = 0;  ///< +0x388, +0x390: the kind last held, kept when only the drawing goes.
    };

    /// 0x00597c50: a main hand of one kind and an off hand of the other are
    /// held together.
    [[nodiscard]] bool together(uint8_t mainType, uint8_t offType, const Hands& hands) const {
        if (mainType == 13 || mainType == 21) {
            if (offType == 14 || offType == 23) return true;
            if (offType == 13 || offType == 22) return hands.dualWield && hands.dualWield();
            if (offType != 21 && offType != 17) return false;
            return hands.fitsOffHand && hands.fitsOffHand(off_.id);
        }
        if (mainType != 17 || offType == 15) return false;
        return hands.fitsOffHand && hands.fitsOffHand(main_.id);
    }

    TryOn put(const Item& item, const Hands& hands, int slot) {
        const TryOn kind = tryOnKind(item.inventoryType);
        if (kind != TryOn::Worn) return kind;
        if (const int body = bodySlot(item.inventoryType); body >= 0) {
            body_[static_cast<std::size_t>(body)] = item;
            return kind;
        }
        // The two hands in turn (0x00acf548).
        const auto inTurn = [this] {
            const int hand = nextHand_;
            nextHand_ = hand == kMainHandSlot ? kOffHandSlot : kMainHandSlot;
            return hand;
        };
        if (slot < 0) {
            switch (item.inventoryType) {
                case 13:
                    slot = (main_.type == 13 || main_.type == 21 || main_.type == 17) && hands.dualWield &&
                                   hands.dualWield()
                               ? inTurn()
                               : kMainHandSlot;
                    break;
                case 17:
                case 21:
                    if (hands.fitsOffHand && hands.fitsOffHand(item.itemId)) {
                        slot = inTurn();
                        break;
                    }
                    [[fallthrough]];
                case 25:
                case 26:
                    slot = nextHand_ = kMainHandSlot;
                    break;
                default:  // 14, 15, 22, 23
                    slot = kOffHandSlot;
                    nextHand_ = kMainHandSlot;
                    break;
            }
        }
        Hand& hand = slot == kMainHandSlot ? main_ : off_;
        Hand& other = slot == kMainHandSlot ? off_ : main_;
        hand.id = item.itemId;
        if (other.type != 0) {
            const uint8_t mainType = slot == kMainHandSlot ? item.inventoryType : main_.type;
            const uint8_t offType = slot == kMainHandSlot ? off_.type : item.inventoryType;
            if (!together(mainType, offType, hands)) {
                other.item = Item{};
                other.id = 0;
            }
        }
        hand.item = item;
        hand.type = item.inventoryType;
        return kind;
    }

    std::array<Item, 11> body_{};
    Hand main_;
    Hand off_;
    int nextHand_ = kMainHandSlot;
};

/// The figure of a frame: the unit's own items, or nothing of them after
/// Undress, and what was tried on over them in turn.
inline std::vector<EquipmentItem> dressed(const std::array<Item, 19>& bySlot, bool undressed,
                                          const std::vector<Item>& triedOn, const Hands& hands) {
    Figure figure;
    figure.dress(bySlot, hands);
    if (undressed) figure.undress();
    for (const Item& item : triedOn) figure.tryOn(item, hands);
    return figure.worn();
}

}  // namespace wowee::game::dressing_room
