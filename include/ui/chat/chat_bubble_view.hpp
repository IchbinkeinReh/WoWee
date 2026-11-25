#pragma once

// A chat bubble as it is shown: what stands in it and where (CGChatBubbleFrame,
// 0x0056cad0).

#include <cstddef>
#include <cstdint>
#include <string>

namespace wowee::ui {

struct ChatBubbleView {
    uint32_t id = 0;         ///< its own, the later made the larger
    std::string text;        ///< as bubbleText has it
    const char* type = "";   ///< whose colour the text has, ChatTypeInfo's key
    float alpha = 1.0f;
    float x = 0.0f, y = 0.0f;  ///< the text's bottom's middle, in 768ths of the screen's height from its lower left
};

namespace chat_bubble {

/// How far over the unit's name's point the text stands (0x0056c500).
inline constexpr float kOverName = 0.7f;

/// The words without their markup (0x007e1680): colours and links are gone, the
/// brackets in a link and pictures too.
[[nodiscard]] inline std::string bubbleText(const std::string& in) {
    std::string out;
    bool link = false;
    for (size_t i = 0; i < in.size();) {
        if (in[i] != '|') {
            if (!link || (in[i] != '[' && in[i] != ']')) out += in[i];
            ++i;
            continue;
        }
        const char tag = i + 1 < in.size() ? in[i + 1] : '\0';
        if (tag == 'c' || tag == 'C') {
            if (i + 10 > in.size()) return out;
            i += 10;
        } else if (tag == 'H') {
            link = true;
            const size_t end = in.find("|h", i + 2);
            if (end == std::string::npos) return out;
            i = end + 2;
        } else if (tag == 'T') {
            const size_t end = in.find("|t", i + 2);
            if (end == std::string::npos) return out;
            i = end + 2;
        } else if (tag == 'h') {
            link = false;
            i += 2;
        } else if (tag == 'r') {
            i += 2;
        } else {
            out += '|';
            if (tag != '\0') out += tag;
            i += 2;
        }
    }
    return out;
}

/// The key of ChatTypeInfo for a line's type (0x00720010's six).
[[nodiscard]] inline const char* typeName(uint8_t chatType) {
    switch (chatType) {
        case 0x02: return "PARTY";
        case 0x06: return "YELL";
        case 0x0c: return "MONSTER_SAY";
        case 0x0e: return "MONSTER_YELL";
        case 0x33: return "PARTY_LEADER";
        default: return "SAY";
    }
}

}  // namespace chat_bubble
}  // namespace wowee::ui
