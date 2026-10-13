#pragma once

#include <cstdint>

namespace wowee::game::go_interaction {

/// A game object's animation states, CGGameObject_C's index into the table
/// at 0x00ada938: Spawn, Closed, Open, Opened, Close, Destroy, Destroyed,
/// Rebuild, then Custom0..3 and Despawn (AnimationData 145..157).
inline constexpr uint8_t kStateOpen = 2;
inline constexpr uint8_t kStateOpened = 3;
inline constexpr uint8_t kStateClose = 4;
inline constexpr uint8_t kStateCustom0 = 8;
inline constexpr uint8_t kStateDespawn = 12;

/// What the type's handler does with its animation.
enum class Action : uint8_t {
    None,
    /// Enter `state` (slot 0xb0, 0x0070d510: nothing if already in it).
    Enter,
    /// Enter `state` only if the model has its animation (0x0070c480).
    EnterIfModelHas,
    /// Enter `state` only if the model is not showing its Open already -
    /// Open, or what AnimationData falls back to without one (0x0070fe10).
    EnterUnlessShowingOpen,
};

struct Step {
    Action action = Action::None;
    uint8_t state = 0;
};

/// A QUESTGIVER's customAnim: of type 2's data fields, the one of kind 0x16
/// (0x00746190 over the table at 0x00a36f34) - data4. 0 when the template is
/// not cached (GameObject_C +0x1a4).
[[nodiscard]] inline uint32_t questGiverCustomAnim(const uint32_t* data) {
    return data ? data[4] : 0u;
}

/// The object became the interaction object (0x0051fce0 -> 0x0070bab0, the
/// type handler's slot 0x98).
///   type 2   0x0070fe10: with no customAnim, Open unless it shows it already;
///            with one, the custom animation 0x0070bde0 picks -
///            0x00a335e8[customAnim + 1], Custom0..3 and Despawn for 1..5
///   type 9   0x0070b560: Open
///   19, 34   0x0070c9f0 (mailbox, guild bank): Custom0
/// Every other type's slot is 0x005eeb70, which does nothing.
[[nodiscard]] inline Step onOpen(uint32_t goType, const uint32_t* data) {
    switch (goType) {
        case 2: {
            const uint32_t custom = questGiverCustomAnim(data);
            if (custom == 0) return {Action::EnterUnlessShowingOpen, kStateOpen};
            // Past 5 the table holds something that is not a state.
            if (custom > 5) return {};
            return {Action::EnterIfModelHas, static_cast<uint8_t>(kStateCustom0 + custom - 1)};
        }
        case 9:
            return {Action::Enter, kStateOpen};
        case 19:
        case 34:
            return {Action::EnterIfModelHas, kStateCustom0};
        default:
            return {};
    }
}

/// The interaction object was cleared (0x00512e60 -> 0x0070bac0, slot 0x9c).
///   type 2   0x0070ff20: Close, with no customAnim and the object in Open
///            or Opened
///   type 9   0x0070b570: Close
/// Every other type's slot does nothing.
[[nodiscard]] inline Step onClose(uint32_t goType, const uint32_t* data, uint8_t current) {
    switch (goType) {
        case 2:
            if (questGiverCustomAnim(data) == 0 &&
                (current == kStateOpen || current == kStateOpened)) {
                return {Action::Enter, kStateClose};
            }
            return {};
        case 9:
            return {Action::Enter, kStateClose};
        default:
            return {};
    }
}

} // namespace wowee::game::go_interaction
