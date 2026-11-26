#pragma once

// The gesture a speaker makes with a chat line (0x00509dd0): a say, yell,
// party or party-leader line from a unit this client has plays a talk
// animation through 0x0073ad70 - laugh when the whole line is one of the
// interface's LAUGH_WORD1.. strings, else (say and yell only) shout for a
// yell, question for a say ending in '?', exclamation for one ending in '!',
// talk otherwise. Party lines gesture only for a laugh word.

#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace game {

enum class ChatGesture : int { None = -1, Talk = 0, Question = 1, Exclamation = 2, Shout = 3, Laugh = 4 };

/// `type` is the chat type's wire value (SAY 1, PARTY 2, YELL 6, PARTY_LEADER
/// 0x33). The laugh words are compared whole, byte for byte (0x0076ea40
/// walks both strings' UTF-8 characters and stops at the first difference).
inline ChatGesture chatGestureFor(uint8_t type, const std::string& text, const std::vector<std::string>& laughWords) {
    if (type != 1 && type != 6 && type != 2 && type != 0x33) return ChatGesture::None;
    for (const auto& word : laughWords)
        if (word == text) return ChatGesture::Laugh;
    if (type != 1 && type != 6) return ChatGesture::None;
    if (type == 6) return ChatGesture::Shout;
    if (!text.empty() && text.back() == '?') return ChatGesture::Question;
    if (!text.empty() && text.back() == '!') return ChatGesture::Exclamation;
    return ChatGesture::Talk;
}

}  // namespace game
}  // namespace wowee
