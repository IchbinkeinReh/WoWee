#pragma once

// A gossip greeting (SMSG_NPC_TEXT_UPDATE) and the one line of it the client
// shows each time the window opens (0x0058ac30).

#include <array>
#include <cstdint>
#include <string>

namespace wowee::game::npc_text {

struct Option {
    float probability = 0.0f;
    std::array<std::string, 2> text;   ///< [0] the male text, [1] the female
    uint32_t language = 0;
};

struct NpcText {
    std::array<Option, 8> options;
};

/// Which of the two texts: for a unit, the female one where the unit's
/// gender byte (UNIT_FIELD_BYTES_0 byte 2) is 1 and the male one otherwise;
/// for anything else, the female one only where the first option has no
/// male text.
inline int column(const NpcText& t, bool isUnit, uint8_t gender) {
    if (isUnit) return gender == 1 ? 1 : 0;
    return t.options[0].text[0].empty() ? 1 : 0;
}

/// The option shown, or -1 for none ("Missing gossip text!"). Options whose
/// text in `col` is empty are passed over; of the rest, the first whose
/// running probability reaches (1 - roll) times their total is the one,
/// `roll` being the client's draw in [0, 1).
inline int choose(const NpcText& t, int col, float roll) {
    float total = 0.0f;
    for (const auto& o : t.options)
        if (!o.text[col].empty()) total += o.probability;
    const float threshold = (1.0f - roll) * total;
    float running = 0.0f;
    for (int i = 0; i < 8; ++i) {
        const auto& o = t.options[i];
        if (o.text[col].empty()) continue;
        running += o.probability;
        if (!(running < threshold)) return i;
    }
    return -1;
}

}  // namespace wowee::game::npc_text
