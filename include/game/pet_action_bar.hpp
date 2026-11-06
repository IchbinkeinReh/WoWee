#pragma once

// The pet bar's ten slots as the client moves actions between them
// (0x005d5ad0 and 0x005d6020, both over 0xc23508). An action is a packed
// u32: its type in the top byte's low six bits - 1 a spell, 6 a reaction
// mode, 7 a command - and its spell or value in the low 24.

#include <array>
#include <cstdint>

namespace wowee::game::pet_action_bar {

inline constexpr int kSlots = 10;
using Slots = std::array<uint32_t, kSlots>;

[[nodiscard]] constexpr uint32_t typeOf(uint32_t action) { return (action >> 24) & 0x3fu; }
[[nodiscard]] constexpr uint32_t valueOf(uint32_t action) { return action & 0xffffffu; }
inline constexpr uint32_t kTypeSpell = 1;
inline constexpr uint32_t kTypeReact = 6;
inline constexpr uint32_t kTypeCommand = 7;

struct Placed {
    bool changed = false;  ///< nothing at all when false
    int moved = -1;        ///< the other slot that changed, sent first; -1 for none
    /// The action the slot held, which goes to the cursor when nothing took
    /// its place (0x005d6020's tail); 0 when the slot's old action was moved
    /// to another slot instead.
    uint32_t displaced = 0;
};

/// `action` put into `slot`. The same action already elsewhere (the type's
/// flags aside, 0x3fffffff) is taken out of there and the slot's old action
/// goes in its place; a slot holding a reaction mode or a command keeps it, by
/// moving it to the first slot that is neither and is empty - and when there
/// is none the placing fails. A spell action with no spell is put without
/// looking for a twin.
inline Placed place(Slots& slots, int slot, uint32_t action) {
    Placed out;
    if (slot < 0 || slot >= kSlots || action == slots[static_cast<size_t>(slot)]) return out;
    const uint32_t old = slots[static_cast<size_t>(slot)];
    int other = -1;
    if (typeOf(action) != kTypeSpell || valueOf(action) != 0) {
        for (int i = 0; i < kSlots; ++i) {
            if (i != slot && (slots[static_cast<size_t>(i)] & 0x3fffffffu) == (action & 0x3fffffffu)) {
                slots[static_cast<size_t>(i)] = 0;
                other = i;
                break;
            }
        }
    }
    const uint32_t oldType = typeOf(old);
    if (oldType == kTypeReact || oldType == kTypeCommand) {
        if (other < 0) {
            for (int i = 0; i < kSlots; ++i) {
                const uint32_t t = typeOf(slots[static_cast<size_t>(i)]);
                if (t != kTypeReact && t != kTypeCommand && valueOf(slots[static_cast<size_t>(i)]) == 0) {
                    other = i;
                    break;
                }
            }
            if (other < 0) return out;
        }
        slots[static_cast<size_t>(other)] = old;
    } else if (other >= 0) {
        slots[static_cast<size_t>(other)] = old;
    }
    slots[static_cast<size_t>(slot)] = action;
    out.changed = true;
    out.moved = other;
    out.displaced = other < 0 ? old : 0;
    return out;
}

}  // namespace wowee::game::pet_action_bar
