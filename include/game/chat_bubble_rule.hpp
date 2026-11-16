#pragma once

// Which chat lines put a bubble over their speaker (0x00720010).

#include <cstdint>

namespace wowee::game::chat_bubble_rule {

/// The CVar a chat type's bubble answers to, or none for a type that never
/// has one.
enum class Setting : uint8_t { None, ChatBubbles, ChatBubblesParty };

/// 0x00720010: SAY (1), YELL (6), MONSTER_SAY (0xc) and MONSTER_YELL (0xe)
/// under chatBubbles; PARTY (2) and PARTY_LEADER (0x33) under
/// chatBubblesParty. Every other type - raid, monster party, whispers,
/// emotes - has no bubble.
[[nodiscard]] constexpr Setting settingFor(uint8_t chatType) {
    switch (chatType) {
        case 0x01: case 0x06: case 0x0c: case 0x0e: return Setting::ChatBubbles;
        case 0x02: case 0x33: return Setting::ChatBubblesParty;
        default: return Setting::None;
    }
}

/// A bubble is made only for a speaker within 25 yards of the player: the
/// squared distance at most 625 (0x00720010).
[[nodiscard]] constexpr bool inRange(float squaredDistance) { return squaredDistance <= 625.0f; }

}  // namespace wowee::game::chat_bubble_rule
