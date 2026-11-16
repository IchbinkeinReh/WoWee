#pragma once

// How long a chat bubble stays up and how it fades, as the client times it.

#include <cstdint>
#include <string>

namespace wowee::ui::chat_bubble {

/// The bubble's time on screen in milliseconds, before it fades out
/// (0x0056c1c0): 2000 plus 750 a word for anyone else, 1000 plus 500 a word
/// for the player's own. A word ends at a space or tab that follows a
/// non-blank, and the last one counts whether or not one follows it; an
/// empty line stays up for no time at all.
inline uint32_t durationMs(const std::string& text, bool own) {
    if (text.empty()) return 0;
    const uint32_t perWord = own ? 500u : 750u;
    uint32_t ms = (own ? 1000u : 2000u) + perWord;
    bool blank = true;
    for (const char c : text) {
        if (c == ' ' || c == '\t') {
            if (!blank) ms += perWord;
            blank = true;
        } else {
            blank = false;
        }
    }
    return ms;
}

/// The fade in and the fade out, each 250 ms (0x0056c500, 0xfa).
inline constexpr float kFadeSeconds = 0.25f;

/// Shown only within 25 yards of the camera: the client hides a bubble
/// whose squared distance reaches 625 (0x0056c500).
inline constexpr float kRangeYards = 25.0f;

/// A bubble's alpha moved toward shown or hidden by one frame, at the rate
/// that crosses the whole range in one fade - so a fade that turns back
/// half way carries on from where it was (0x0056c240, 0x0056c2b0 start
/// their clocks from the current alpha).
inline float stepAlpha(float alpha, bool shown, float dt) {
    const float step = dt / kFadeSeconds;
    if (shown) return alpha + step >= 1.0f ? 1.0f : alpha + step;
    return alpha - step <= 0.0f ? 0.0f : alpha - step;
}

}  // namespace wowee::ui::chat_bubble
